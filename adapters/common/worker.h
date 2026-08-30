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

#include "abi/reconstruct.h"

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
 * One case, one line, flushed before returning. `obs` may be NULL when the
 * case never got as far as a reconstruction. `status` is "accepted",
 * "rejected" or "error" -- and "rejected" is a compatibility entry, not a
 * defect, so nothing here treats it as a failure.
 */
void abi_worker_result(AbiWorker *w, const char *case_path, const char *id,
                       const char *status, const char *detail,
                       const AbiObserver *obs);

#endif /* ABI_WORKER_H */
