/*
 * The worker side of docs/worker-protocol.md, factored out so that every
 * C-family adapter reports in exactly the same shape.
 *
 * The coordinator distinguishes "the consumer refused this case" from "this
 * consumer died" by what reached the results file before the process ended, so
 * the one rule that matters here is that abi_worker_result() flushes. A worker
 * that buffers loses its whole run at the first fault, and the case that
 * mattered most is the one it fails to report.
 */
#ifndef ABI_WORKER_H
#define ABI_WORKER_H

#include <stddef.h>
#include <stdio.h>

#include "abi/callseq.h"
#include "abi/digest.h"
#include "abi/lifecycle.h"
#include "abi/reconstruct.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ABI_WORKER_PROTOCOL 1

typedef struct {
  const char *cases;
  const char *results;
  const char *consumer;
} AbiWorkerArgs;

typedef struct {
  char **paths;
  size_t count;
} AbiCaseList;

typedef struct {
  FILE *out;
  /*
   * The consumer moves what it is handed into storage of its own, which the
   * log cannot see, so a release there is not judged against the address it
   * was handed at. Zero for a consumer whose every move is the executor's.
   */
  int consumer_moves;
} AbiWorker;

/*
 * Parses the invocation in docs/worker-protocol.md. Prints usage to stderr and
 * returns non-zero on anything it does not recognise: an adapter that silently
 * ignores an argument it was given reports on a run nobody asked for.
 */
int abi_worker_parse_args(int argc, char **argv, AbiWorkerArgs *out);

/* One path per line; blank lines and `#` comments skipped. Non-zero on error.
 */
int  abi_worker_read_cases(const char *path, AbiCaseList *out);
void abi_case_list_free(AbiCaseList *l);

int  abi_worker_open(const AbiWorkerArgs *args, AbiWorker *w);
void abi_worker_close(AbiWorker *w);

/*
 * Line 1: who this consumer is. Host, compiler and sanitizer state belong to
 * the observation rather than to the case (docs/abicase-format.md 4), which is
 * why they are reported here and never written into an .abicase.
 * `version` may be NULL when the consumer does not expose one.
 */
void abi_worker_header(AbiWorker *w, const char *consumer, const char *version);

/*
 * What was handed to the consumer and what it handed back, digested
 * (docs/digest.md). Each half is in one of three states -- computed, failed
 * with a reason, or absent -- and they are written as such. A consumer that
 * hands nothing back has no `received`, never a copy of `sent`: absent has to
 * stay distinguishable from equal, or a consumer that returns nothing reads as
 * one that returned the data intact.
 */
/*
 * Room for "<status>: " plus the whole of AbiError.message, so a reason is
 * never cut short -- a truncated reason is a report nobody can act on.
 */
#define ABI_WORKER_REASON_SIZE 256

typedef struct {
  int       have_sent;
  AbiDigest sent;
  char      sent_error[ABI_WORKER_REASON_SIZE];
  int       have_received;
  AbiDigest received;
  char      received_error[ABI_WORKER_REASON_SIZE];
} AbiWorkerDigest;

/* Fills one half from `schema` + `array`; a failure is recorded, not fatal. */
void abi_worker_digest_sent(AbiWorkerDigest *d, const struct ArrowSchema *s,
                            const struct ArrowArray *a);
void abi_worker_digest_received(AbiWorkerDigest *d, const struct ArrowSchema *s,
                                const struct ArrowArray *a);

/*
 * The optional blocks of a result line. Each is written when non-NULL: the
 * observer once a reconstruction exists, the digest when anything was
 * digested, the call sequence when it was executed, the lifecycle verdict when
 * the log was judged.
 */
typedef struct {
  const AbiObserver         *observer;
  const AbiWorkerDigest     *digest;
  const AbiCallseqResult    *callseq;
  const AbiLifecycleVerdict *lifecycle;
  int                        lifecycle_strict;
} AbiWorkerExtras;

/*
 * One case, one line, flushed before returning. `x` may be NULL when the case
 * never got as far as a reconstruction. `status` is "accepted", "rejected" or
 * "error" -- and "rejected" is a compatibility entry, not a defect, so nothing
 * here treats it as a failure.
 */
void abi_worker_result(AbiWorker *w, const char *case_path, const char *id,
                       const char *status, const char *detail,
                       const AbiWorkerExtras *x);

/*
 * Runs one case the standard way for an in-process C consumer: read,
 * reconstruct, digest what is handed over, execute the case's CALLSEQ against
 * `consumer`, clean up, judge the lifecycle -- strictly unless the consumer
 * moves what it holds (`consumer_moves`) -- and write the line.
 * `received` is filled by the consumer when it hands something back, or left
 * zeroed.
 */
void abi_worker_run_case(AbiWorker *w, const char *case_path,
                         const AbiConsumer *consumer,
                         AbiWorkerDigest   *digest_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ABI_WORKER_H */
