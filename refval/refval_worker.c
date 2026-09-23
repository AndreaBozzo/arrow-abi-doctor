/*
 * The reference validator: nanoarrow's ArrowArrayViewValidate() behind the
 * worker protocol.
 *
 * A baseline, not ground truth. nanoarrow has had validation bugs of its own in
 * exactly this area (arrow-nanoarrow#626, offset-buffer validation for sliced
 * arrays), so its verdict is one voice among several and never the deciding
 * one: "nanoarrow accepts it, therefore it is valid" is a sentence this project
 * does not write.
 *
 * What it checks is an obligation the specification now states. Arrow's
 * security model asks implementors for validation APIs that detect invalid
 * data "without crashing on invalid data", and ArrowArrayViewValidate() is
 * nanoarrow's. So the property under test is "validated without crashing", not
 * "returned the verdict we expected": a crash or a hang here is a finding at
 * the same weight as a consumer's, and runs behind the same worker isolation.
 *
 * Each case is validated at the level it declares (docs/abicase-format.md 9),
 * whose four values mirror nanoarrow's on purpose, so no translation sits
 * between the case and the verdict. It is told that level through the
 * executor's `begin` hook -- the one consumer that may read what a case
 * expects, because checking the expectation is its job.
 *
 * It takes the structures, validates them through a view that never copies
 * them, and releases them when the call sequence says to. It hands nothing
 * back, so it reports no `received` digest.
 */
#include <stdio.h>
#include <string.h>

#include "abi/callseq.h"
#include "nanoarrow/nanoarrow.h"
#include "worker.h"

typedef struct {
  struct ArrowSchema *schema;
  struct ArrowArray  *array;
  int                 level;
  /* A stream, and what it handed over, in the validator's own storage. */
  struct ArrowArrayStream *stream;
  struct ArrowSchema       stream_schema;
  struct ArrowArray        stream_batch;
} Refval;

static const char *level_name(int level) {
  switch (level) {
  case NANOARROW_VALIDATION_LEVEL_NONE: return "none";
  case NANOARROW_VALIDATION_LEVEL_MINIMAL: return "minimal";
  case NANOARROW_VALIDATION_LEVEL_DEFAULT: return "default";
  case NANOARROW_VALIDATION_LEVEL_FULL: return "full";
  }
  return "?";
}

static void refval_begin(void *ctx, const AbiCase *c) {
  /* ABI_VALIDATE_* and NANOARROW_VALIDATION_LEVEL_* share their values. */
  ((Refval *)ctx)->level = (int)c->expected.validation_level;
}

static int refval_import_schema(void *ctx, struct ArrowSchema *s, char *why,
                                size_t n) {
  (void)why;
  (void)n;
  ((Refval *)ctx)->schema = s;
  return 0;
}

/*
 * The validation. A refusal leaves the array live, which hands it back to the
 * harness: nanoarrow never took ownership of it, it only looked.
 */
/* Validates `a` against `schema` at the case's level: 0, or 1 with why. */
static int validate(Refval *rv, const struct ArrowSchema *schema,
                    const struct ArrowArray *a, char *why, size_t n) {
  struct ArrowArrayView view;
  struct ArrowError     err;
  const char           *stage;
  ArrowErrorCode        code;

  memset(&err, 0, sizeof(err));
  code = ArrowArrayViewInitFromSchema(&view, schema, &err);
  if (code != NANOARROW_OK) {
    snprintf(why, n, "nanoarrow: schema: %s", err.message);
    return 1;
  }
  /*
   * Minimal to attach, then the declared level: SetArray() on its own would
   * validate at the default level whatever the case declared, and a case that
   * asks for `none` or `minimal` has to be held to exactly that.
   */
  stage = "attach";
  code = ArrowArrayViewSetArrayMinimal(&view, a, &err);
  if (code == NANOARROW_OK) {
    stage = level_name(rv->level);
    code = ArrowArrayViewValidate(&view, (enum ArrowValidationLevel)rv->level,
                                  &err);
  }
  ArrowArrayViewReset(&view);
  if (code != NANOARROW_OK) {
    snprintf(why, n, "nanoarrow %s: %s", stage, err.message);
    return 1;
  }
  snprintf(why, n, "nanoarrow: valid at %s", level_name(rv->level));
  return 0;
}

static int refval_import_array(void *ctx, struct ArrowArray *a, char *why,
                               size_t n) {
  Refval *rv = (Refval *)ctx;

  if (!rv->schema) {
    snprintf(why, n, "no schema was handed over before the array");
    return 1;
  }
  if (validate(rv, rv->schema, a, why, n) != 0) return 1;
  rv->array = a;
  return 0;
}

/* --- the C Stream Interface ------------------------------------------------
 */

static int refval_import_stream(void *ctx, struct ArrowArrayStream *s,
                                char *why, size_t n) {
  (void)why;
  (void)n;
  ((Refval *)ctx)->stream = s;
  return 0;
}

/* The schema must at least parse: a batch is validated against it. */
static int refval_stream_get_schema(void *ctx, char *why, size_t n) {
  Refval                *rv = (Refval *)ctx;
  struct ArrowSchemaView sv;
  struct ArrowError      err;
  int                    rc;

  if (rv->stream_schema.release) rv->stream_schema.release(&rv->stream_schema);
  rc = rv->stream->get_schema(rv->stream, &rv->stream_schema);
  if (rc) {
    snprintf(why, n, "get_schema returned %d", rc);
    return rc;
  }
  memset(&err, 0, sizeof(err));
  if (ArrowSchemaViewInit(&sv, &rv->stream_schema, &err) != NANOARROW_OK) {
    snprintf(why, n, "nanoarrow schema: %s", err.message);
    return 1;
  }
  /* All a schema-only stream has to validate; a batch's note replaces it. */
  snprintf(why, n, "nanoarrow: schema valid at %s", level_name(rv->level));
  return 0;
}

/* Each batch is validated as a standalone array would be. */
static int refval_stream_get_next(void *ctx, int *eof, char *why, size_t n) {
  Refval *rv = (Refval *)ctx;
  int     rc;

  if (rv->stream_batch.release) rv->stream_batch.release(&rv->stream_batch);
  rc = rv->stream->get_next(rv->stream, &rv->stream_batch);
  if (rc) {
    snprintf(why, n, "get_next returned %d", rc);
    return rc;
  }
  *eof = rv->stream_batch.release == NULL;
  if (*eof) return 0; /* silent: the last verdict stands */
  if (!rv->stream_schema.release) {
    snprintf(why, n, "a batch arrived before any schema was asked for");
    return 1;
  }
  return validate(rv, &rv->stream_schema, &rv->stream_batch, why, n);
}

static void refval_release_base(void *ctx) {
  Refval *rv = (Refval *)ctx;
  if (rv->array && rv->array->release) rv->array->release(rv->array);
  if (rv->schema && rv->schema->release) rv->schema->release(rv->schema);
  if (rv->stream_batch.release) rv->stream_batch.release(&rv->stream_batch);
  if (rv->stream_schema.release) rv->stream_schema.release(&rv->stream_schema);
  if (rv->stream && rv->stream->release) rv->stream->release(rv->stream);
}

int main(int argc, char **argv) {
  AbiWorkerArgs args;
  AbiCaseList   list;
  AbiWorker     w;
  size_t        i;

  if (abi_worker_parse_args(argc, argv, &args) != 0) return 1;
  if (abi_worker_read_cases(args.cases, &list) != 0) return 1;
  if (abi_worker_open(&args, &w) != 0) {
    abi_case_list_free(&list);
    return 1;
  }

  abi_worker_header(&w, "nanoarrow", NANOARROW_VERSION);
  for (i = 0; i < list.count; i++) {
    Refval          rv;
    AbiConsumer     consumer;
    AbiWorkerDigest dg;

    memset(&rv, 0, sizeof(rv));
    memset(&consumer, 0, sizeof(consumer));
    memset(&dg, 0, sizeof(dg));
    consumer.ctx = &rv;
    consumer.begin = refval_begin;
    consumer.import_schema = refval_import_schema;
    consumer.import_array = refval_import_array;
    consumer.release_base = refval_release_base;
    consumer.import_stream = refval_import_stream;
    consumer.stream_get_schema = refval_stream_get_schema;
    consumer.stream_get_next = refval_stream_get_next;
    abi_worker_run_case(&w, list.paths[i], &consumer, &dg);
  }

  abi_worker_close(&w);
  abi_case_list_free(&list);
  return 0;
}
