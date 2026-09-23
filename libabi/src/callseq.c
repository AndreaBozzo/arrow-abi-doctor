/*
 * The CALLSEQ executor of abi/callseq.h.
 *
 * The executor tracks where each base structure currently lives, because after
 * MOVE_STRUCT the reconstruction's own copy is a released shell and the live
 * structure is in storage the executor allocated. Every handoff, move and
 * refusal goes into the observer's log through reconstruct.h, so the lifecycle
 * state machine judges the run from the same log the report prints.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi/callseq.h"

const char *abi_callseq_outcome_str(AbiCallseqOutcome outcome) {
  switch (outcome) {
  case ABI_CALLSEQ_ACCEPTED: return "accepted";
  case ABI_CALLSEQ_REJECTED: return "rejected";
  case ABI_CALLSEQ_UNSUPPORTED: return "unsupported";
  case ABI_CALLSEQ_INVALID: return "invalid";
  }
  return "?";
}

const char *abi_op_str(AbiOpCode code) {
  switch (code) {
  case ABI_OP_NOP: return "NOP";
  case ABI_OP_IMPORT_SCHEMA: return "IMPORT_SCHEMA";
  case ABI_OP_IMPORT_ARRAY: return "IMPORT_ARRAY";
  case ABI_OP_STREAM_GET_SCHEMA: return "STREAM_GET_SCHEMA";
  case ABI_OP_STREAM_GET_NEXT: return "STREAM_GET_NEXT";
  case ABI_OP_STREAM_GET_LAST_ERROR: return "STREAM_GET_LAST_ERROR";
  case ABI_OP_RELEASE_BASE: return "RELEASE_BASE";
  case ABI_OP_RELEASE_CHILD: return "RELEASE_CHILD";
  case ABI_OP_RELEASE_DICTIONARY: return "RELEASE_DICTIONARY";
  case ABI_OP_MOVE_STRUCT: return "MOVE_STRUCT";
  case ABI_OP_USE_AFTER_RELEASE: return "USE_AFTER_RELEASE";
  case ABI_OP_EXPECT_EOF: return "EXPECT_EOF";
  }
  return "?";
}

typedef struct {
  AbiReconstruction  *r;
  const AbiConsumer  *consumer;
  AbiCallseqResult   *out;
  struct ArrowSchema *schema; /* where the schema lives now */
  struct ArrowArray  *array;  /* where the array lives now; NULL if none */
  struct ArrowSchema *moved_schema; /* storage this executor owns, if moved */
  struct ArrowArray  *moved_array;
  int                 schema_imported;
  int                 array_imported;
  /* Stream mode: the stream, and what the last get_next() answered. */
  struct ArrowArrayStream *stream;
  int                      stream_imported;
  int                      got_next;
  int                      last_eof;
} Exec;

int abi_case_is_stream(const AbiCase *c) {
  uint32_t i;
  for (i = 0; c && i < c->op_count; i++) {
    switch ((AbiOpCode)c->ops[i].code) {
    case ABI_OP_STREAM_GET_SCHEMA:
    case ABI_OP_STREAM_GET_NEXT:
    case ABI_OP_STREAM_GET_LAST_ERROR:
    case ABI_OP_EXPECT_EOF: return 1;
    default: break;
    }
  }
  return 0;
}

static AbiCallseqOutcome stop(Exec *x, AbiCallseqOutcome outcome,
                              const char *what, AbiOpCode code) {
  snprintf(x->out->detail, sizeof(x->out->detail), "%s: %s", abi_op_str(code),
           what);
  return outcome;
}

static AbiCallseqOutcome op_move(Exec *x) {
  struct ArrowSchema *s;
  struct ArrowArray  *a = NULL;

  /*
   * A consumer moves what it holds; the harness moves what it still owns.
   * Moving a structure out from under a consumer that was handed it would be
   * the harness breaking the handoff, so it is refused as a sequence error.
   */
  if (x->schema_imported || x->array_imported) {
    return stop(x, ABI_CALLSEQ_INVALID, "a structure was already handed over",
                ABI_OP_MOVE_STRUCT);
  }
  s = (struct ArrowSchema *)calloc(1, sizeof(*s));
  if (x->array) a = (struct ArrowArray *)calloc(1, sizeof(*a));
  if (!s || (x->array && !a)) {
    free(s);
    free(a);
    return stop(x, ABI_CALLSEQ_INVALID, "out of memory", ABI_OP_MOVE_STRUCT);
  }
  abi_reconstruction_move_schema(x->r, s, x->schema);
  if (x->array) abi_reconstruction_move_array(x->r, a, x->array);

  /* The previous storage is a released shell now; only ours is freed here. */
  free(x->moved_schema);
  free(x->moved_array);
  x->schema = x->moved_schema = s;
  if (x->array) x->array = x->moved_array = a;
  return ABI_CALLSEQ_ACCEPTED;
}

static AbiCallseqOutcome op_import(Exec *x, AbiOpCode code) {
  const AbiConsumer *c = x->consumer;
  int                is_array = code == ABI_OP_IMPORT_ARRAY;
  int                refused;
  char               why[192];

  if (is_array ? !c->import_array : !c->import_schema) {
    return stop(x, ABI_CALLSEQ_UNSUPPORTED, "not performed by this consumer",
                code);
  }
  if (is_array && !x->array) {
    return stop(x, ABI_CALLSEQ_INVALID, "the case has no array", code);
  }
  if (is_array ? x->array_imported : x->schema_imported) {
    return stop(x, ABI_CALLSEQ_INVALID, "already handed over", code);
  }

  /*
   * Noted before the call: a consumer may release inside its own import, and
   * that release has to come after the handoff in the log or it would read as
   * a release of something the consumer was never given.
   */
  why[0] = 0;
  if (is_array) {
    abi_reconstruction_note_array_import(x->r, x->array);
    refused = c->import_array(c->ctx, x->array, why, sizeof(why));
    x->array_imported = !refused;
  } else {
    abi_reconstruction_note_schema_import(x->r, x->schema);
    refused = c->import_schema(c->ctx, x->schema, why, sizeof(why));
    x->schema_imported = !refused;
  }
  if (!refused) {
    /* A consumer may say what it made of the structure even when it took it:
       the reference validator says at which level it passed. */
    if (why[0]) snprintf(x->out->detail, sizeof(x->out->detail), "%s", why);
    return ABI_CALLSEQ_ACCEPTED;
  }

  /*
   * A refusal. Some consumers release what they refused (Arrow C++'s
   * ImportArray does, even on failure) and some leave it live. Live means
   * ownership came back, and the harness's later cleanup is then correct
   * rather than a consumer that never released.
   */
  if (is_array && x->array->release) {
    abi_reconstruction_note_array_returned(x->r, x->array);
  } else if (!is_array && x->schema->release) {
    abi_reconstruction_note_schema_returned(x->r, x->schema);
  }
  return stop(x, ABI_CALLSEQ_REJECTED, why[0] ? why : "refused", code);
}

static AbiCallseqOutcome op_misuse(Exec *x, const AbiOp *op) {
  const AbiConsumer *c = x->consumer;
  AbiOpCode          code = (AbiOpCode)op->code;

  if (code == ABI_OP_USE_AFTER_RELEASE) {
#if defined(ABI_ENABLE_USE_AFTER_RELEASE)
    if (!c->use_after_release) {
      return stop(x, ABI_CALLSEQ_UNSUPPORTED, "not performed by this consumer",
                  code);
    }
    c->use_after_release(c->ctx);
    return ABI_CALLSEQ_ACCEPTED;
#else
    return stop(x, ABI_CALLSEQ_UNSUPPORTED,
                "undefined behaviour in this process; build with "
                "ABI_ENABLE_USE_AFTER_RELEASE and run it behind worker "
                "isolation",
                code);
#endif
  }
  if (!x->array_imported) {
    return stop(x, ABI_CALLSEQ_INVALID, "the array was never handed over",
                code);
  }
  if (code == ABI_OP_RELEASE_CHILD) {
    if (!c->release_child) {
      return stop(x, ABI_CALLSEQ_UNSUPPORTED, "not performed by this consumer",
                  code);
    }
    if ((int64_t)op->arg0 >= x->array->n_children) {
      return stop(x, ABI_CALLSEQ_INVALID, "no such child", code);
    }
    c->release_child(c->ctx, op->arg0);
    return ABI_CALLSEQ_ACCEPTED;
  }
  if (!c->release_dictionary) {
    return stop(x, ABI_CALLSEQ_UNSUPPORTED, "not performed by this consumer",
                code);
  }
  if (!x->array->dictionary) {
    return stop(x, ABI_CALLSEQ_INVALID, "the array has no dictionary", code);
  }
  c->release_dictionary(c->ctx);
  return ABI_CALLSEQ_ACCEPTED;
}

/* The handoff of a stream: the first stream op performs it. */
static AbiCallseqOutcome stream_handoff(Exec *x, AbiOpCode code) {
  const AbiConsumer *c = x->consumer;
  char               why[192];
  int                refused;

  if (x->stream_imported) return ABI_CALLSEQ_ACCEPTED;
  if (!c->import_stream) {
    return stop(x, ABI_CALLSEQ_UNSUPPORTED,
                "this consumer does not take a stream", code);
  }
  why[0] = 0;
  abi_reconstruction_note_stream_import(x->r, x->stream);
  refused = c->import_stream(c->ctx, x->stream, why, sizeof(why));
  if (!refused) {
    x->stream_imported = 1;
    return ABI_CALLSEQ_ACCEPTED;
  }
  if (x->stream->release) {
    abi_reconstruction_note_stream_returned(x->r, x->stream);
  }
  return stop(x, ABI_CALLSEQ_REJECTED, why[0] ? why : "refused", code);
}

static AbiCallseqOutcome op_stream(Exec *x, AbiOpCode code) {
  const AbiConsumer *c = x->consumer;
  AbiCallseqOutcome  o;
  char               why[192];
  int                refused = 0, eof = 0;

  if (!x->stream) {
    return stop(x, ABI_CALLSEQ_UNSUPPORTED,
                "not reconstructed as a stream (abi_reconstruct_stream)", code);
  }
  if (code == ABI_OP_EXPECT_EOF) {
    if (!x->got_next || !x->last_eof) {
      return stop(x, ABI_CALLSEQ_INVALID,
                  "the stream had not answered EOF -- the harness's producer "
                  "delivered more than the case holds",
                  code);
    }
    return ABI_CALLSEQ_ACCEPTED;
  }
  o = stream_handoff(x, code);
  if (o != ABI_CALLSEQ_ACCEPTED) return o;

  why[0] = 0;
  switch (code) {
  case ABI_OP_STREAM_GET_SCHEMA:
    if (!c->stream_get_schema) break;
    refused = c->stream_get_schema(c->ctx, why, sizeof(why));
    goto answered;
  case ABI_OP_STREAM_GET_NEXT:
    if (!c->stream_get_next) break;
    refused = c->stream_get_next(c->ctx, &eof, why, sizeof(why));
    x->got_next = 1;
    x->last_eof = eof;
    goto answered;
  case ABI_OP_STREAM_GET_LAST_ERROR:
    if (!c->stream_get_last_error) break;
    refused = c->stream_get_last_error(c->ctx, why, sizeof(why));
    goto answered;
  default: break;
  }
  return stop(x, ABI_CALLSEQ_UNSUPPORTED, "not performed by this consumer",
              code);

answered:
  if (refused)
    return stop(x, ABI_CALLSEQ_REJECTED, why[0] ? why : "refused", code);
  if (why[0]) snprintf(x->out->detail, sizeof(x->out->detail), "%s", why);
  return ABI_CALLSEQ_ACCEPTED;
}

static AbiCallseqOutcome run_op(Exec *x, const AbiOp *op) {
  AbiOpCode code = (AbiOpCode)op->code;

  if (x->stream &&
      (code == ABI_OP_IMPORT_SCHEMA || code == ABI_OP_IMPORT_ARRAY ||
       code == ABI_OP_MOVE_STRUCT)) {
    return stop(x, ABI_CALLSEQ_INVALID,
                "a stream case hands over a stream, not its parts", code);
  }
  switch (code) {
  case ABI_OP_NOP: return ABI_CALLSEQ_ACCEPTED;
  case ABI_OP_MOVE_STRUCT: return op_move(x);
  case ABI_OP_IMPORT_SCHEMA:
  case ABI_OP_IMPORT_ARRAY: return op_import(x, code);
  case ABI_OP_RELEASE_BASE:
    if (!x->consumer->release_base) {
      return stop(x, ABI_CALLSEQ_UNSUPPORTED, "not performed by this consumer",
                  code);
    }
    if (!x->schema_imported && !x->array_imported && !x->stream_imported) {
      return stop(x, ABI_CALLSEQ_INVALID, "nothing was handed over", code);
    }
    x->consumer->release_base(x->consumer->ctx);
    return ABI_CALLSEQ_ACCEPTED;
  case ABI_OP_RELEASE_CHILD:
  case ABI_OP_RELEASE_DICTIONARY:
  case ABI_OP_USE_AFTER_RELEASE: return op_misuse(x, op);
  case ABI_OP_STREAM_GET_SCHEMA:
  case ABI_OP_STREAM_GET_NEXT:
  case ABI_OP_STREAM_GET_LAST_ERROR:
  case ABI_OP_EXPECT_EOF: return op_stream(x, code);
  }
  return stop(x, ABI_CALLSEQ_INVALID, "unknown op", code);
}

void abi_callseq_run(AbiReconstruction *r, const AbiCase *c,
                     const AbiConsumer *consumer, AbiCallseqResult *out) {
  static const AbiOp DEFAULT_OPS[] = {{ABI_OP_IMPORT_SCHEMA, 0, 0},
                                      {ABI_OP_IMPORT_ARRAY, 0, 0},
                                      {ABI_OP_RELEASE_BASE, 0, 0}};
  AbiOp              defaults[3];
  const AbiOp       *ops;
  uint32_t           count, i;
  Exec               x;

  memset(out, 0, sizeof(*out));
  memset(&x, 0, sizeof(x));
  x.r = r;
  x.consumer = consumer;
  x.out = out;
  x.schema = abi_reconstruction_schema(r);
  x.array = abi_reconstruction_array(r);
  x.stream = abi_reconstruction_stream(r);

  if (c->op_count) {
    ops = c->ops;
    count = c->op_count;
  } else {
    /* The default path, less IMPORT_ARRAY for a schema-only case. */
    memcpy(defaults, DEFAULT_OPS, sizeof(defaults));
    count = 0;
    for (i = 0; i < 3; i++) {
      if (defaults[i].code == ABI_OP_IMPORT_ARRAY && !x.array) continue;
      defaults[count++] = defaults[i];
    }
    ops = defaults;
    out->defaulted = 1;
  }

  out->op_count = count;
  out->outcome = ABI_CALLSEQ_ACCEPTED;
  if (consumer->begin) consumer->begin(consumer->ctx, c);
  for (i = 0; i < count; i++) {
    AbiCallseqOutcome o = run_op(&x, &ops[i]);
    if (o != ABI_CALLSEQ_ACCEPTED) {
      out->outcome = o;
      break;
    }
    if (out->executed < ABI_CALLSEQ_TRACE_MAX) {
      out->trace[out->executed] = ops[i].code;
    }
    out->executed++;
  }

  /*
   * A sequence that stops early -- a refusal, or one the harness cannot run as
   * written -- ends the case, and the consumer lets go of what it had already
   * taken: the schema, when it is the array it refused. Without that, the
   * harness would release the consumer's schema and the state machine would
   * blame the consumer for a release it was never let make. A stream consumer
   * holds its schemas and batches in storage of its own, which only it can
   * release. Not traced as an op: the sequence did not ask for it.
   */
  if (out->outcome != ABI_CALLSEQ_ACCEPTED && consumer->release_base &&
      (x.schema_imported || x.array_imported || x.stream_imported)) {
    consumer->release_base(consumer->ctx);
  }

  /*
   * Whatever is still live, wherever it lives, is released by the harness and
   * logged as such -- including the consumer's structures when a sequence
   * stopped early or never released them. The state machine decides which of
   * those was the consumer's failing and which was simply the harness's job.
   */
  /* The stream first: releasing it releases a batch it still owns. */
  abi_reconstruction_harness_release_stream(r, x.stream);
  abi_reconstruction_harness_release_array(r, x.array);
  abi_reconstruction_harness_release_schema(r, x.schema);
  free(x.moved_schema);
  free(x.moved_array);
}
