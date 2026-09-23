/*
 * The CALLSEQ executor: run a case's call sequence against a consumer, instead
 * of the fixed import / consume / release path.
 *
 * The sequence is encoded, validated and class-checked by the format
 * (docs/abicase-format.md 8); this is what makes it do something. A consumer
 * says what it can do through AbiConsumer, and an op it cannot do is refused
 * by name -- never quietly replaced by the default path, which would report a
 * case as run when the part that made it a case was skipped.
 *
 * What the ops mean here:
 *
 *   IMPORT_SCHEMA / IMPORT_ARRAY   hand the base structure, where it now
 *                                  lives, to the consumer
 *   MOVE_STRUCT                    move both base structures to fresh storage,
 *                                  per Arrow move semantics
 *   RELEASE_BASE                   the consumer releases what it holds
 *   RELEASE_CHILD / _DICTIONARY    class C: the consumer releases a child or
 *                                  the dictionary directly, which the
 *                                  specification forbids. Safe to perform --
 *                                  the observer records the violation and the
 *                                  producer's own release still walks the tree
 *   USE_AFTER_RELEASE              class C: a read through a released array.
 *                                  Genuine undefined behaviour in this
 *                                  process, so it is refused unless the build
 *                                  defines ABI_ENABLE_USE_AFTER_RELEASE, and
 *                                  belongs only behind worker isolation
 *   STREAM_GET_SCHEMA / _GET_NEXT  a stream case (abi_case_is_stream): the
 *   / _GET_LAST_ERROR              first stream op hands the stream over, and
 *                                  each op is the consumer calling the
 *                                  stream's own callback
 *   EXPECT_EOF                     the last get_next answered EOF; anything
 *                                  else is the harness's producer misbehaving,
 *                                  so the sequence is invalid
 *   NOP                            nothing
 *
 * A stream case hands over a stream, not a schema and an array: IMPORT_* and
 * MOVE_STRUCT in one are invalid, and a stream op on a reconstruction that is
 * not a stream (abi_reconstruct_stream) is unsupported.
 *
 * A case with no CALLSEQ runs IMPORT_SCHEMA, IMPORT_ARRAY, RELEASE_BASE.
 */
#ifndef ABI_CALLSEQ_H
#define ABI_CALLSEQ_H

#include <stddef.h>

#include "abi/abicase.h"
#include "abi/reconstruct.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What a consumer does. Each import returns 0 when the consumer took the
 * structure and non-zero when it refused it, writing why into `detail` -- which
 * it may also fill on success, as a note the result line then carries. On a
 * refusal ownership stays with the caller, which is how the executor knows the
 * harness, not the consumer, has to clean up. A NULL callback is an op this
 * consumer does not perform.
 */
typedef struct {
  void *ctx;
  /*
   * Called once before the first op, with the case itself. For the reference
   * validator (refval/), which is told the case's declared validation_level.
   * A consumer under test must leave this NULL: an engine that could read what
   * a case expects of it would no longer be the thing being measured.
   */
  void (*begin)(void *ctx, const AbiCase *c);
  int (*import_schema)(void *ctx, struct ArrowSchema *schema, char *detail,
                       size_t detail_size);
  int (*import_array)(void *ctx, struct ArrowArray *array, char *detail,
                      size_t detail_size);
  /* Release everything the consumer holds, through the base structures. */
  void (*release_base)(void *ctx);
  /* Class C misuse, performed on what the consumer holds. */
  void (*release_child)(void *ctx, uint32_t index);
  void (*release_dictionary)(void *ctx);
  void (*use_after_release)(void *ctx);
  /*
   * The C Stream Interface. import_stream takes the stream, as the imports
   * above take a schema or an array. The rest are the consumer calling the
   * stream's callbacks; stream_get_next reports whether the answer was EOF.
   * A non-zero return is a refusal, with why in `detail`.
   */
  int (*import_stream)(void *ctx, struct ArrowArrayStream *stream, char *detail,
                       size_t detail_size);
  int (*stream_get_schema)(void *ctx, char *detail, size_t detail_size);
  int (*stream_get_next)(void *ctx, int *eof, char *detail, size_t detail_size);
  int (*stream_get_last_error)(void *ctx, char *detail, size_t detail_size);
} AbiConsumer;

typedef enum {
  ABI_CALLSEQ_ACCEPTED = 0, /* every op ran and the consumer took the case */
  ABI_CALLSEQ_REJECTED,     /* the consumer refused an import */
  ABI_CALLSEQ_UNSUPPORTED,  /* an op this consumer or build cannot perform */
  ABI_CALLSEQ_INVALID       /* the sequence makes no sense: release before
                               import, an import of a missing array */
} AbiCallseqOutcome;

const char *abi_callseq_outcome_str(AbiCallseqOutcome outcome);

#define ABI_CALLSEQ_TRACE_MAX 32

typedef struct {
  AbiCallseqOutcome outcome;
  int               defaulted; /* no CALLSEQ: the default path ran */
  uint32_t          op_count;  /* ops in the sequence, default path included */
  uint32_t          executed;  /* ops actually performed */
  uint16_t          trace[ABI_CALLSEQ_TRACE_MAX]; /* codes, in order run */
  char              detail[256];
} AbiCallseqResult;

/*
 * Runs `c`'s CALLSEQ against the reconstruction `r` of that same case. When it
 * returns, whatever the consumer left live has been released by the harness,
 * wherever a move put it, and logged as such; the caller still owns `r` and
 * reads the observer from it.
 */
void abi_callseq_run(AbiReconstruction *r, const AbiCase *c,
                     const AbiConsumer *consumer, AbiCallseqResult *out);

const char *abi_op_str(AbiOpCode code);

/* Non-zero when the case's CALLSEQ uses the C Stream Interface: reconstruct it
   with abi_reconstruct_stream(). */
int abi_case_is_stream(const AbiCase *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ABI_CALLSEQ_H */
