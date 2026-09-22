/*
 * The null consumer: the smallest conforming one there is.
 *
 * It imports a reconstruction and immediately releases it, which is exactly
 * what the C Data Interface asks a consumer to do and nothing more. That makes
 * it the control voice of a differential run -- a case that the null consumer
 * cannot survive is a defect on our side of the boundary, because there is no
 * consumer logic left for it to be in. It also needs no third-party library, so
 * it is the one worker available on every host the coordinator runs on.
 *
 * It is not a consumer of *data*. It reads no buffer and compares no value; a
 * silent divergence is invisible to it by construction. It does report the
 * digest of what it was handed (`sent`), which is the harness's half of every
 * comparison a data-reading consumer makes.
 */
#include <stdio.h>
#include <string.h>

#include "abi/abicase.h"
#include "abi/reconstruct.h"
#include "worker.h"

static void run_case(AbiWorker *w, const char *path) {
  AbiCase            *c = NULL;
  AbiReconstruction  *r = NULL;
  AbiError            err;
  AbiStatus           st;
  struct ArrowSchema *schema;
  struct ArrowArray  *array;
  char                id[ABICASE_ID_HEX_SIZE];
  char                detail[256];
  long long           length = -1;
  AbiWorkerDigest     dg;

  memset(&err, 0, sizeof(err));
  memset(&dg, 0, sizeof(dg));
  memset(id, 0, sizeof(id));

  st = abi_case_read_file(path, &c, &err);
  if (st != ABI_OK) {
    snprintf(detail, sizeof(detail), "%s: %s", abi_status_str(st), err.message);
    abi_worker_result(w, path, NULL, "error", detail, NULL, NULL);
    return;
  }
  abi_case_id(c, id);

  st = abi_reconstruct(c, &r, &err);
  if (st != ABI_OK) {
    snprintf(detail, sizeof(detail), "%s: %s", abi_status_str(st), err.message);
    abi_worker_result(w, path, id, "error", detail, NULL, NULL);
    abi_case_free(c);
    return;
  }

  schema = abi_reconstruction_schema(r);
  array = abi_reconstruction_array(r);
  if (array) length = (long long)array->length;

  /*
   * What is handed over, digested before the handoff. There is no `received`:
   * this consumer reads nothing and hands nothing back, and saying so by
   * omission is the point -- a copy of `sent` would claim a round trip that
   * never happened.
   */
  abi_worker_digest_sent(&dg, schema, array);

  /*
   * The consumer's whole behaviour. Released parent-first and by us, from
   * outside the producer's own callbacks, so the observer sees the depth-0
   * entry that distinguishes a consumer release from a nested one.
   */
  if (array && array->release) array->release(array);
  if (schema && schema->release) schema->release(schema);

  snprintf(detail, sizeof(detail), "imported and released, length=%lld",
           length);

  /*
   * release_all() before reading the observer, in that order: it logs whatever
   * the consumer left live as HARNESS_RELEASED, and reading first would report
   * a clean lifecycle for a consumer that never released anything.
   */
  abi_reconstruction_release_all(r);
  abi_worker_result(w, path, id, "accepted", detail,
                    abi_reconstruction_observer(r), &dg);

  abi_reconstruction_free(r);
  abi_case_free(c);
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

  abi_worker_header(&w, "null", NULL);
  for (i = 0; i < list.count; i++)
    run_case(&w, list.paths[i]);

  abi_worker_close(&w);
  abi_case_list_free(&list);
  return 0;
}
