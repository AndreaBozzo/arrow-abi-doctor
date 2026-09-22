/*
 * CALLSEQ executor and lifecycle state machine tests -- M1, issue #5.
 *
 * The consumer here is a test double that can be told to misbehave in each of
 * the ways the state machine exists to name: refuse an import and leave the
 * structure live, refuse and release it, take it and never release it, move it
 * into storage of its own, release a child directly. Every run is judged from
 * the observer's log, the same way a worker's is, and every run must also leave
 * the allocator balanced: a verdict over a run that leaked is not one to trust.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi/callseq.h"
#include "abi/lifecycle.h"
#include "abi/reconstruct.h"
#include "fixture.h"

static int         g_checks = 0;
static int         g_fails = 0;
static const char *g_test = "?";

#define CHECKF(cond, fmt, ...)                                                 \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_fails++;                                                               \
      fprintf(stderr, "FAIL [%s] %s:%d: " fmt "\n", g_test, __FILE__,          \
              __LINE__, __VA_ARGS__);                                          \
    }                                                                          \
  } while (0)

/* --- the test consumer ---------------------------------------------------- */

enum { REFUSE_NONE = 0, REFUSE_LEAVE_LIVE, REFUSE_AND_RELEASE };

typedef struct {
  struct ArrowSchema *schema;
  struct ArrowArray  *array;
  int                 refuse_array;
  int                 never_release;
  int                 internal_move; /* take into own storage, untracked */
  struct ArrowSchema  own_schema;
  struct ArrowArray   own_array;
} TestConsumer;

static int tc_import_schema(void *ctx, struct ArrowSchema *s, char *why,
                            size_t n) {
  TestConsumer *tc = (TestConsumer *)ctx;
  (void)why;
  (void)n;
  if (tc->internal_move) {
    /* A legal move a real engine makes and tells nobody about. */
    tc->own_schema = *s;
    s->release = NULL;
    tc->schema = &tc->own_schema;
  } else {
    tc->schema = s;
  }
  return 0;
}

static int tc_import_array(void *ctx, struct ArrowArray *a, char *why,
                           size_t n) {
  TestConsumer *tc = (TestConsumer *)ctx;
  if (tc->refuse_array) {
    snprintf(why, n, "test consumer refuses arrays");
    if (tc->refuse_array == REFUSE_AND_RELEASE) a->release(a);
    return 1;
  }
  if (tc->internal_move) {
    tc->own_array = *a;
    a->release = NULL;
    tc->array = &tc->own_array;
  } else {
    tc->array = a;
  }
  return 0;
}

static void tc_release_base(void *ctx) {
  TestConsumer *tc = (TestConsumer *)ctx;
  if (tc->never_release) return;
  if (tc->array && tc->array->release) tc->array->release(tc->array);
  if (tc->schema && tc->schema->release) tc->schema->release(tc->schema);
}

static void tc_release_child(void *ctx, uint32_t i) {
  TestConsumer      *tc = (TestConsumer *)ctx;
  struct ArrowArray *child = tc->array->children[i];
  child->release(child);
}

static void tc_release_dictionary(void *ctx) {
  TestConsumer *tc = (TestConsumer *)ctx;
  tc->array->dictionary->release(tc->array->dictionary);
}

static AbiConsumer consumer_of(TestConsumer *tc) {
  AbiConsumer c;
  memset(&c, 0, sizeof(c));
  c.ctx = tc;
  c.import_schema = tc_import_schema;
  c.import_array = tc_import_array;
  c.release_base = tc_release_base;
  c.release_child = tc_release_child;
  c.release_dictionary = tc_release_dictionary;
  return c;
}

/* --- running a case ------------------------------------------------------- */

typedef struct {
  AbiCallseqResult    result;
  AbiLifecycleVerdict strict;
  AbiLifecycleVerdict loose;
  int                 leaked;
  uint64_t            moved_array_addr;
  uint64_t            released_array_addr;
  uint64_t            original_array_addr;
  int                 original_released_by_callback;
} Run;

/* The smoke fixture with its CALLSEQ replaced by `ops` (count 0: none). */
static AbiCase *smoke_with(const AbiOp *ops, uint32_t count) {
  AbiCase *c = abi_fixture_smoke();
  uint32_t i;
  if (!c) return NULL;
  c->op_count = 0;
  for (i = 0; i < count; i++)
    abi_case_add_op(c, (AbiOpCode)ops[i].code, ops[i].arg0, ops[i].arg1);
  return c;
}

static void run_case(AbiCase *c, TestConsumer *tc, Run *out) {
  AbiReconstruction *r = NULL;
  AbiConsumer        consumer = consumer_of(tc);
  const AbiObserver *o;
  uint32_t           i;

  memset(out, 0, sizeof(*out));
  if (abi_reconstruct(c, &r, NULL) != ABI_OK) return;
  out->original_array_addr = (uint64_t)(uintptr_t)abi_reconstruction_array(r);

  abi_callseq_run(r, c, &consumer, &out->result);
  abi_reconstruction_release_all(r);

  o = abi_reconstruction_observer(r);
  abi_lifecycle_verify(o, 1, &out->strict);
  abi_lifecycle_verify(o, 0, &out->loose);
  out->leaked = abi_reconstruction_leaked(r);
  for (i = 0; i < o->event_count; i++) {
    const AbiEvent *e = &o->events[i];
    if (e->kind == ABI_EV_ARRAY_MOVED) out->moved_array_addr = e->addr;
    if (e->kind == ABI_EV_ARRAY_RELEASE_ENTER && e->depth == 0 &&
        strcmp(e->path, "/") == 0) {
      out->released_array_addr = e->addr;
      if (e->addr == out->original_array_addr) {
        out->original_released_by_callback = 1;
      }
    }
  }
  abi_reconstruction_free(r);
}

static int count_rule(const AbiLifecycleVerdict *v, AbiLifecycleRule rule) {
  uint32_t i;
  int      n = 0;
  for (i = 0; i < v->count && i < ABI_LC_MAX_FINDINGS; i++)
    n += v->findings[i].rule == rule;
  return n;
}

static void describe(const AbiLifecycleVerdict *v) {
  uint32_t i;
  for (i = 0; i < v->count && i < ABI_LC_MAX_FINDINGS; i++) {
    fprintf(stderr, "    finding: %s %s %s\n",
            abi_lifecycle_rule_str((AbiLifecycleRule)v->findings[i].rule),
            v->findings[i].is_array ? "array" : "schema", v->findings[i].path);
  }
}

#define OP(code)      {(uint16_t)(code), 0, 0}
#define OP1(code, a0) {(uint16_t)(code), (a0), 0}

/* --- the tests ------------------------------------------------------------ */

static void test_default_path_is_clean(void) {
  AbiCase     *c = smoke_with(NULL, 0);
  TestConsumer tc;
  Run          run;

  g_test = "default path";
  memset(&tc, 0, sizeof(tc));
  run_case(c, &tc, &run);
  CHECKF(run.result.outcome == ABI_CALLSEQ_ACCEPTED && run.result.defaulted,
         "outcome %s, defaulted %d",
         abi_callseq_outcome_str(run.result.outcome), run.result.defaulted);
  CHECKF(run.result.executed == 3, "executed %u ops", run.result.executed);
  CHECKF(run.strict.count == 0, "%u finding(s)", run.strict.count);
  if (run.strict.count) describe(&run.strict);
  CHECKF(!run.leaked, "%s", "leaked");
  abi_case_free(c);
}

static void test_a_move_transfers_ownership_without_a_release(void) {
  static const AbiOp ops[] = {OP(ABI_OP_MOVE_STRUCT), OP(ABI_OP_IMPORT_SCHEMA),
                              OP(ABI_OP_IMPORT_ARRAY), OP(ABI_OP_RELEASE_BASE)};
  AbiCase           *c = smoke_with(ops, 4);
  TestConsumer       tc;
  Run                run;

  g_test = "moved";
  memset(&tc, 0, sizeof(tc));
  run_case(c, &tc, &run);
  CHECKF(run.result.outcome == ABI_CALLSEQ_ACCEPTED, "outcome %s: %s",
         abi_callseq_outcome_str(run.result.outcome), run.result.detail);
  CHECKF(run.result.executed == 4 && run.result.trace[0] == ABI_OP_MOVE_STRUCT,
         "executed %u, first %u", run.result.executed, run.result.trace[0]);
  CHECKF(run.moved_array_addr != 0 &&
             run.moved_array_addr != run.original_array_addr,
         "%s", "no move to a new address was logged");
  /* One release, entered where the structure was moved to. The moved-from
     original was marked released without its callback ever running. */
  CHECKF(run.released_array_addr == run.moved_array_addr,
         "released at %llx, moved to %llx",
         (unsigned long long)run.released_array_addr,
         (unsigned long long)run.moved_array_addr);
  CHECKF(!run.original_released_by_callback, "%s",
         "the moved-from structure's callback ran");
  CHECKF(run.strict.count == 0, "%u finding(s)", run.strict.count);
  if (run.strict.count) describe(&run.strict);
  CHECKF(!run.leaked, "%s", "leaked");
  abi_case_free(c);
}

static void test_a_consumer_that_never_releases_is_named(void) {
  AbiCase     *c = smoke_with(NULL, 0);
  TestConsumer tc;
  Run          run;

  g_test = "never releases";
  memset(&tc, 0, sizeof(tc));
  tc.never_release = 1;
  run_case(c, &tc, &run);
  CHECKF(count_rule(&run.loose, ABI_LC_NOT_RELEASED_BY_CONSUMER) == 2,
         "%d not-released findings, expected schema and array",
         count_rule(&run.loose, ABI_LC_NOT_RELEASED_BY_CONSUMER));
  CHECKF(!run.leaked, "%s", "the harness cleanup must still balance");
  abi_case_free(c);
}

static void test_a_refusal_that_leaves_it_live_is_not_blamed(void) {
  AbiCase     *c = smoke_with(NULL, 0);
  TestConsumer tc;
  Run          run;

  g_test = "refuse, leave live";
  memset(&tc, 0, sizeof(tc));
  tc.refuse_array = REFUSE_LEAVE_LIVE;
  run_case(c, &tc, &run);
  CHECKF(run.result.outcome == ABI_CALLSEQ_REJECTED, "outcome %s",
         abi_callseq_outcome_str(run.result.outcome));
  CHECKF(strstr(run.result.detail, "refuses arrays") != NULL, "detail %s",
         run.result.detail);
  /* The schema it took and never released is its own; the array it refused
     and handed back is the harness's to clean up. */
  CHECKF(count_rule(&run.loose, ABI_LC_NOT_RELEASED_BY_CONSUMER) == 1 &&
             !run.loose.findings[0].is_array,
         "%u finding(s)", run.loose.count);
  if (run.loose.count != 1) describe(&run.loose);
  CHECKF(!run.leaked, "%s", "leaked");
  abi_case_free(c);
}

static void test_a_refusal_that_releases_is_the_consumers_release(void) {
  AbiCase     *c = smoke_with(NULL, 0);
  TestConsumer tc;
  Run          run;

  g_test = "refuse and release";
  memset(&tc, 0, sizeof(tc));
  tc.refuse_array = REFUSE_AND_RELEASE;
  run_case(c, &tc, &run);
  CHECKF(run.result.outcome == ABI_CALLSEQ_REJECTED, "outcome %s",
         abi_callseq_outcome_str(run.result.outcome));
  CHECKF(count_rule(&run.loose, ABI_LC_RELEASED_BEFORE_IMPORT) == 0, "%s",
         "a release inside a refused import is still after the handoff");
  CHECKF(!run.leaked, "%s", "leaked");
  abi_case_free(c);
}

static void test_a_child_released_by_the_consumer_is_a_violation(void) {
  static const AbiOp ops[] = {OP(ABI_OP_IMPORT_SCHEMA), OP(ABI_OP_IMPORT_ARRAY),
                              OP1(ABI_OP_RELEASE_CHILD, 0),
                              OP(ABI_OP_RELEASE_BASE)};
  AbiCase           *c = smoke_with(ops, 4);
  TestConsumer       tc;
  Run                run;

  g_test = "release child";
  memset(&tc, 0, sizeof(tc));
  run_case(c, &tc, &run);
  CHECKF(run.result.outcome == ABI_CALLSEQ_ACCEPTED, "outcome %s: %s",
         abi_callseq_outcome_str(run.result.outcome), run.result.detail);
  CHECKF(count_rule(&run.loose, ABI_LC_CHILD_RELEASED_BY_CONSUMER) == 1,
         "%u finding(s)", run.loose.count);
  if (run.loose.count != 1) describe(&run.loose);
  CHECKF(!run.leaked, "%s", "the producer's release must still free it all");
  abi_case_free(c);
}

static void test_an_untracked_move_is_only_stale_when_strict(void) {
  AbiCase     *c = smoke_with(NULL, 0);
  TestConsumer tc;
  Run          run;

  g_test = "untracked move";
  memset(&tc, 0, sizeof(tc));
  tc.internal_move = 1;
  run_case(c, &tc, &run);
  CHECKF(run.loose.count == 0, "%u finding(s) without strict locations",
         run.loose.count);
  CHECKF(count_rule(&run.strict, ABI_LC_RELEASED_AT_STALE_LOCATION) == 2,
         "%d stale-location findings",
         count_rule(&run.strict, ABI_LC_RELEASED_AT_STALE_LOCATION));
  CHECKF(!run.leaked, "%s", "leaked");
  abi_case_free(c);
}

static void test_unperformable_ops_are_refused_by_name(void) {
  static const AbiOp stream[] = {OP(ABI_OP_STREAM_GET_SCHEMA)};
  static const AbiOp uaf[] = {OP(ABI_OP_IMPORT_SCHEMA), OP(ABI_OP_IMPORT_ARRAY),
                              OP(ABI_OP_RELEASE_BASE),
                              OP(ABI_OP_USE_AFTER_RELEASE)};
  static const AbiOp early[] = {OP(ABI_OP_RELEASE_BASE)};
  static const AbiOp dict[] = {OP(ABI_OP_IMPORT_SCHEMA),
                               OP(ABI_OP_IMPORT_ARRAY),
                               OP(ABI_OP_RELEASE_DICTIONARY)};
  AbiCase           *c;
  TestConsumer       tc;
  Run                run;

  g_test = "refused ops";
  memset(&tc, 0, sizeof(tc));
  c = smoke_with(stream, 1);
  run_case(c, &tc, &run);
  CHECKF(run.result.outcome == ABI_CALLSEQ_UNSUPPORTED &&
             strstr(run.result.detail, "#4") != NULL,
         "stream: %s %s", abi_callseq_outcome_str(run.result.outcome),
         run.result.detail);
  CHECKF(!run.leaked, "%s", "stream: leaked");
  abi_case_free(c);

#if !defined(ABI_ENABLE_USE_AFTER_RELEASE)
  memset(&tc, 0, sizeof(tc));
  c = smoke_with(uaf, 4);
  run_case(c, &tc, &run);
  CHECKF(run.result.outcome == ABI_CALLSEQ_UNSUPPORTED &&
             run.result.executed == 3,
         "use after release: %s after %u ops",
         abi_callseq_outcome_str(run.result.outcome), run.result.executed);
  abi_case_free(c);
#else
  (void)uaf;
#endif

  memset(&tc, 0, sizeof(tc));
  c = smoke_with(early, 1);
  run_case(c, &tc, &run);
  CHECKF(run.result.outcome == ABI_CALLSEQ_INVALID, "release first: %s",
         abi_callseq_outcome_str(run.result.outcome));
  CHECKF(run.loose.count == 0 && !run.leaked,
         "release first: %u finding(s), leaked %d", run.loose.count,
         run.leaked);
  abi_case_free(c);

  memset(&tc, 0, sizeof(tc));
  c = smoke_with(dict, 3);
  run_case(c, &tc, &run);
  CHECKF(run.result.outcome == ABI_CALLSEQ_INVALID &&
             strstr(run.result.detail, "no dictionary") != NULL,
         "dictionary: %s %s", abi_callseq_outcome_str(run.result.outcome),
         run.result.detail);
  abi_case_free(c);
}

static void test_a_consumer_without_the_callback_is_unsupported(void) {
  AbiCase           *c = smoke_with(NULL, 0);
  TestConsumer       tc;
  AbiConsumer        consumer;
  AbiReconstruction *r = NULL;
  AbiCallseqResult   result;

  g_test = "missing callback";
  memset(&tc, 0, sizeof(tc));
  consumer = consumer_of(&tc);
  consumer.release_base = NULL;
  if (abi_reconstruct(c, &r, NULL) == ABI_OK) {
    abi_callseq_run(r, c, &consumer, &result);
    CHECKF(result.outcome == ABI_CALLSEQ_UNSUPPORTED &&
               strstr(result.detail, "RELEASE_BASE") != NULL,
           "%s %s", abi_callseq_outcome_str(result.outcome), result.detail);
    abi_reconstruction_free(r);
  }
  abi_case_free(c);
}

int main(void) {
  test_default_path_is_clean();
  test_a_move_transfers_ownership_without_a_release();
  test_a_consumer_that_never_releases_is_named();
  test_a_refusal_that_leaves_it_live_is_not_blamed();
  test_a_refusal_that_releases_is_the_consumers_release();
  test_a_child_released_by_the_consumer_is_a_violation();
  test_an_untracked_move_is_only_stale_when_strict();
  test_unperformable_ops_are_refused_by_name();
  test_a_consumer_without_the_callback_is_unsupported();

  printf("%d checks, %d failure(s)\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
