/*
 * A worker that fails on purpose, in each of the ways a real consumer fails.
 *
 * The acceptance condition for worker isolation is that a crashing consumer is
 * reported as an observed datum while every other consumer still finishes, and
 * that it is verified by actually crashing one rather than by reading the code
 * path. This is the thing that crashes. It is a test instrument, never a voice
 * in a differential run: it consumes nothing and its results say nothing about
 * Arrow.
 *
 * Configured by environment rather than by argument, so that the invocation in
 * docs/worker-protocol.md stays the same for every worker and the coordinator
 * needs no special case to launch this one.
 *
 *   ABI_FAULTY_MODE=segv|exit|hang|silent   (default segv)
 *   ABI_FAULTY_AT=<1-based case index>      (default 1)
 *   ABI_FAULTY_EXIT_CODE=<n>                (default 42, `exit` mode only)
 *
 * `segv` is the interesting one; the others exist because the coordinator
 * claims to report a non-zero exit, a timeout and a truncated stream as
 * distinct outcomes, and a claim about an untested path is a claim.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "worker.h"

static void sleep_a_while(void) {
#if defined(_WIN32)
  Sleep(3600 * 1000);
#else
  sleep(3600);
#endif
}

static void fail_now(const char *mode) {
  if (strcmp(mode, "exit") == 0) {
    /*
     * A consumer that calls exit() on bad input. The code is configurable
     * because -1 is the interesting one: on Windows it arrives at the parent as
     * 0xFFFFFFFF, which a coordinator testing the whole NTSTATUS error range
     * would file as a fault.
     */
    const char *code = getenv("ABI_FAULTY_EXIT_CODE");
    exit(code ? (int)strtol(code, NULL, 10) : 42);
  }
  if (strcmp(mode, "hang") == 0) {
    /* A consumer that deadlocks. Only the coordinator's clock ends this. */
    sleep_a_while();
    exit(0);
  }
  if (strcmp(mode, "silent") == 0) {
    /* Exits cleanly having reported nothing further: the stream is short but
       the process looks fine, so only the case accounting catches it. */
    exit(0);
  }

  /*
   * volatile, so that the store is not a dead one the optimizer may delete or
   * fold into a trap instruction of its own choosing. On POSIX this raises
   * SIGSEGV; on Windows it is an access violation, which the parent sees as
   * exit code 0xC0000005.
   */
  {
    volatile int *p = (volatile int *)0;
    *p = 1;
  }
  exit(0); /* not reached; keeps the compiler from warning about the path */
}

int main(int argc, char **argv) {
  AbiWorkerArgs args;
  AbiCaseList   list;
  AbiWorker     w;
  const char   *mode = getenv("ABI_FAULTY_MODE");
  const char   *at = getenv("ABI_FAULTY_AT");
  size_t        crash_at;
  size_t        i;

  if (!mode) mode = "segv";
  crash_at = at ? (size_t)strtoul(at, NULL, 10) : 1;
  if (crash_at == 0) crash_at = 1;

  if (abi_worker_parse_args(argc, argv, &args) != 0) return 1;
  if (abi_worker_read_cases(args.cases, &list) != 0) return 1;
  if (abi_worker_open(&args, &w) != 0) {
    abi_case_list_free(&list);
    return 1;
  }

  abi_worker_header(&w, "faulty", mode);
  for (i = 0; i < list.count; i++) {
    if (i + 1 == crash_at) fail_now(mode);
    abi_worker_result(&w, list.paths[i], NULL, "accepted",
                      "not a real consumer", NULL);
  }

  abi_worker_close(&w);
  abi_case_list_free(&list);
  return 0;
}
