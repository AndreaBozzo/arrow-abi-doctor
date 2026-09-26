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
 *   ABI_FAULTY_MODE=segv|exit|hang|silent|drop-offset   (default segv)
 *   ABI_FAULTY_AT=<1-based case index>      (default 1)
 *   ABI_FAULTY_EXIT_CODE=<n>                (default 42, `exit` mode only)
 *
 * `segv` is the interesting one; the others exist because the coordinator
 * claims to report a non-zero exit, a timeout and a truncated stream as
 * distinct outcomes, and a claim about an untested path is a claim.
 *
 * `drop-offset` fails differently: it never dies, it answers wrongly. Every
 * case is reconstructed and digested as handed over, and what the worker
 * "hands back" is the same buffers with ArrowArray.offset zeroed on every node
 * -- what a consumer that ignores the offset reads. Nothing else in the tree
 * produces a silent divergence on demand, and the check that the digest
 * catches one (adapters/dataprof/check_digest_divergence.py) needs one that
 * really happens. ABI_FAULTY_AT does not apply to it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "abi/reconstruct.h"
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

/* --- drop-offset ---------------------------------------------------------- */

static void free_view(struct ArrowArray *v) {
  int64_t i;

  if (!v) return;
  for (i = 0; i < v->n_children; i++)
    free_view(v->children[i]);
  free(v->children);
  free(v);
}

/*
 * A shallow copy of an array tree with every offset zeroed. The copies borrow
 * the original buffers and own nothing, so their release is NULL; a dictionary
 * is borrowed as it stands. NULL child slots -- a B1 case may declare more
 * children than it provides -- stay NULL.
 */
static struct ArrowArray *drop_offsets(const struct ArrowArray *src) {
  struct ArrowArray *dst = (struct ArrowArray *)calloc(1, sizeof(*dst));
  int64_t            i;

  if (!dst) return NULL;
  *dst = *src;
  dst->offset = 0;
  dst->release = NULL;
  dst->private_data = NULL;
  dst->children = NULL;
  dst->n_children = 0;
  if (src->n_children > 0) {
    dst->children = (struct ArrowArray **)calloc((size_t)src->n_children,
                                                 sizeof(*dst->children));
    if (!dst->children) {
      free(dst);
      return NULL;
    }
    dst->n_children = src->n_children;
    for (i = 0; i < src->n_children; i++) {
      if (!src->children[i]) continue;
      dst->children[i] = drop_offsets(src->children[i]);
      if (!dst->children[i]) {
        free_view(dst);
        return NULL;
      }
    }
  }
  return dst;
}

static void run_drop_offset(AbiWorker *w, const char *path) {
  AbiCase            *c = NULL;
  AbiReconstruction  *r = NULL;
  AbiError            err;
  AbiStatus           st;
  struct ArrowSchema *schema;
  struct ArrowArray  *array, *view;
  AbiWorkerDigest     dg;
  AbiWorkerExtras     x;
  char                id[ABICASE_ID_HEX_SIZE];
  char                detail[256];

  memset(&err, 0, sizeof(err));
  memset(&dg, 0, sizeof(dg));
  memset(id, 0, sizeof(id));

  st = abi_case_read_file(path, &c, &err);
  if (st != ABI_OK) {
    snprintf(detail, sizeof(detail), "%s: %s", abi_status_str(st), err.message);
    abi_worker_result(w, path, NULL, "error", detail, NULL);
    return;
  }
  abi_case_id(c, id);
  st = abi_reconstruct(c, &r, &err);
  if (st != ABI_OK) {
    snprintf(detail, sizeof(detail), "%s: %s", abi_status_str(st), err.message);
    abi_worker_result(w, path, id, "error", detail, NULL);
    abi_case_free(c);
    return;
  }

  schema = abi_reconstruction_schema(r);
  array = abi_reconstruction_array(r);
  abi_worker_digest_sent(&dg, schema, array);
  if (array) {
    view = drop_offsets(array);
    if (view) {
      abi_worker_digest_received(&dg, schema, view);
      free_view(view);
    } else {
      snprintf(dg.received_error, sizeof(dg.received_error),
               "out of memory building the offset-free view");
    }
  } else {
    abi_worker_digest_received(&dg, schema, NULL);
  }

  if (array && array->release) array->release(array);
  if (schema && schema->release) schema->release(schema);
  abi_reconstruction_release_all(r);
  memset(&x, 0, sizeof(x));
  x.observer = abi_reconstruction_observer(r);
  x.digest = &dg;
  abi_worker_result(w, path, id, "accepted",
                    "offset dropped on the way back; not a real consumer", &x);
  abi_reconstruction_free(r);
  abi_case_free(c);
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

  w.hands_back = 1;
  abi_worker_header(&w, "faulty", mode);
  if (strcmp(mode, "drop-offset") == 0) {
    for (i = 0; i < list.count; i++)
      run_drop_offset(&w, list.paths[i]);
  } else {
    for (i = 0; i < list.count; i++) {
      if (i + 1 == crash_at) fail_now(mode);
      abi_worker_result(&w, list.paths[i], NULL, "accepted",
                        "not a real consumer", NULL);
    }
  }

  abi_worker_close(&w);
  abi_case_list_free(&list);
  return 0;
}
