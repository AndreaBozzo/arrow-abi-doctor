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
static int refval_import_array(void *ctx, struct ArrowArray *a, char *why,
                               size_t n) {
  Refval               *rv = (Refval *)ctx;
  struct ArrowArrayView view;
  struct ArrowError     err;
  const char           *stage;
  ArrowErrorCode        code;

  memset(&err, 0, sizeof(err));
  if (!rv->schema) {
    snprintf(why, n, "no schema was handed over before the array");
    return 1;
  }
  code = ArrowArrayViewInitFromSchema(&view, rv->schema, &err);
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
  rv->array = a;
  snprintf(why, n, "nanoarrow: valid at %s", level_name(rv->level));
  return 0;
}

static void refval_release_base(void *ctx) {
  Refval *rv = (Refval *)ctx;
  if (rv->array && rv->array->release) rv->array->release(rv->array);
  if (rv->schema && rv->schema->release) rv->schema->release(rv->schema);
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
    abi_worker_run_case(&w, list.paths[i], &consumer, &dg);
  }

  abi_worker_close(&w);
  abi_case_list_free(&list);
  return 0;
}
