/*
 * Reconstruction and lifecycle tests -- M0.5.
 *
 * What is under test is not "does it build a struct" but the four things a
 * consumer can actually observe and that a later differential run depends on:
 *
 *   - aliasing is a real address match, not equal contents
 *   - an intentionally misaligned view really is misaligned
 *   - the producer's release walks its own children, and the observer can tell
 *     that apart from a consumer reaching in and releasing one directly
 *   - allocation and free counts balance, measured rather than asserted
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi_internal.h" /* abi_case_validate() */
#include "abi/reconstruct.h"
#include "fixture.h"

static int         g_checks = 0;
static int         g_fails = 0;
static const char *g_test = "?";

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_fails++;                                                               \
      fprintf(stderr, "FAIL [%s] %s:%d: %s\n", g_test, __FILE__, __LINE__,     \
              (msg));                                                          \
    }                                                                          \
  } while (0)

#define CHECKF(cond, fmt, ...)                                                 \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_fails++;                                                               \
      fprintf(stderr, "FAIL [%s] %s:%d: " fmt "\n", g_test, __FILE__,          \
              __LINE__, __VA_ARGS__);                                          \
    }                                                                          \
  } while (0)

#define RUN(fn)                                                                \
  do {                                                                         \
    g_test = #fn;                                                              \
    fn();                                                                      \
  } while (0)

/* Counts events of one kind in the log. */
static int count_events(const AbiObserver *o, AbiEventKind kind) {
  uint32_t i;
  int      n = 0;
  for (i = 0; i < o->event_count; i++) {
    if (o->events[i].kind == (uint8_t)kind) n++;
  }
  return n;
}

static const AbiEvent *find_event(const AbiObserver *o, AbiEventKind kind,
                                  const char *path) {
  uint32_t i;
  for (i = 0; i < o->event_count; i++) {
    if (o->events[i].kind == (uint8_t)kind &&
        strcmp(o->events[i].path, path) == 0) {
      return &o->events[i];
    }
  }
  return NULL;
}

/* --- structure ----------------------------------------------------------- */

static void test_reconstruct_structure(void) {
  AbiCase            *c = abi_fixture_rich();
  AbiReconstruction  *r = NULL;
  AbiError            err;
  struct ArrowSchema *s;
  struct ArrowArray  *a;

  if (!c) return;
  memset(&err, 0, sizeof(err));
  CHECKF(abi_reconstruct(c, &r, &err) == ABI_OK, "reconstruct failed: %s",
         err.message);
  if (!r) {
    abi_case_free(c);
    return;
  }

  s = abi_reconstruction_schema(r);
  a = abi_reconstruction_array(r);
  CHECK(s != NULL && a != NULL, "schema and array exported");

  CHECK(strcmp(s->format, "+s") == 0, "root format is +s");
  CHECK(s->name == NULL, "absent name reconstructs as a NULL pointer");
  CHECK(s->metadata != NULL, "metadata present");
  CHECK(s->n_children == 2, "root declares two children");
  CHECK(s->children[0] != NULL && strcmp(s->children[0]->format, "i") == 0,
        "child 0 is int32");
  CHECK(s->children[1] != NULL && strcmp(s->children[1]->format, "c") == 0,
        "child 1 is int8 dictionary indices");
  CHECK(s->children[1]->dictionary != NULL &&
            strcmp(s->children[1]->dictionary->format, "u") == 0,
        "dictionary value type is utf8");
  CHECK(s->children[1]->dictionary->name != NULL &&
            s->children[1]->dictionary->name[0] == '\0',
        "empty name reconstructs as an empty string, not NULL");
  CHECK(s->release != NULL && a->release != NULL, "release callbacks armed");

  CHECK(a->length == 8, "root length");
  CHECK(a->children[0]->null_count == -1, "null_count -1 survives");
  CHECK(a->children[1]->offset == 1, "non-zero offset survives");
  CHECK(a->buffers[0] == NULL, "NULL validity buffer stays NULL");

  /*
   * The property the whole allocation/view model exists for. Equal contents
   * would not be enough: these must be the same address, or nothing built on
   * ownership or lifetime means anything.
   */
  CHECK(a->children[0]->buffers[0] == a->children[1]->buffers[0],
        "aliased buffers must reconstruct to one address");
  CHECK(a->children[0]->buffers[1] != a->children[1]->buffers[1],
        "distinct buffers must not collide");

  /* Intentional misalignment: a view at byte_offset 1 into a 64-byte-aligned
     allocation must actually land on an odd address. */
  {
    uintptr_t p = (uintptr_t)a->children[1]->buffers[1];
    CHECKF((p % 2u) == 1u, "misaligned view is at %p, expected an odd address",
           a->children[1]->buffers[1]);
    CHECKF(((p - 1u) % 64u) == 0u,
           "the backing allocation should be 64-byte aligned, base %p",
           (const void *)(p - 1u));
  }

  /* buffer contents actually arrived */
  {
    const uint8_t *validity = (const uint8_t *)a->children[0]->buffers[0];
    CHECK(validity[0] == 0xFF && validity[15] == 0xFF,
          "validity bitmap contents copied");
  }

  abi_reconstruction_free(r);
  abi_case_free(c);
}

/*
 * Metadata is the one buffer written in native byte order, because that is what
 * Arrow's wire form specifies. Decoding it the same way proves the encoder and
 * the host agree, on either endianness.
 */
static void test_metadata_wire_format(void) {
  AbiCase           *c = abi_fixture_rich();
  AbiReconstruction *r = NULL;
  const char        *md;
  int32_t            n_kv = 0, klen = 0, vlen = 0;
  size_t             off = 0;

  if (!c) return;
  if (abi_reconstruct(c, &r, NULL) != ABI_OK) {
    abi_case_free(c);
    return;
  }
  md = abi_reconstruction_schema(r)->metadata;
  CHECK(md != NULL, "metadata buffer present");
  if (md) {
    memcpy(&n_kv, md, sizeof(n_kv));
    off = sizeof(n_kv);
    CHECKF(n_kv == 2, "metadata declares %ld pairs, expected 2", (long)n_kv);

    memcpy(&klen, md + off, sizeof(klen));
    off += sizeof(klen);
    CHECK(klen == 6 && memcmp(md + off, "origin", 6) == 0, "first key");
    off += (size_t)klen;
    memcpy(&vlen, md + off, sizeof(vlen));
    off += sizeof(vlen);
    CHECK(vlen == 10 && memcmp(md + off, "abi-doctor", 10) == 0, "first value");
    off += (size_t)vlen;

    memcpy(&klen, md + off, sizeof(klen));
    off += sizeof(klen);
    CHECK(klen == 6 && memcmp(md + off, "binary", 6) == 0, "second key");
    off += (size_t)klen;
    memcpy(&vlen, md + off, sizeof(vlen));
    off += sizeof(vlen);
    CHECK(vlen == 3 && memcmp(md + off, "a\0b", 3) == 0,
          "value with an embedded NUL survives into the wire form");
  }
  abi_reconstruction_free(r);
  abi_case_free(c);
}

/* --- lifecycle ------------------------------------------------------------ */

static void test_release_ordering_and_balance(void) {
  AbiCase           *c = abi_fixture_rich();
  AbiReconstruction *r = NULL;
  const AbiObserver *o;
  const AbiEvent    *root_enter, *child_enter;

  if (!c) return;
  if (abi_reconstruct(c, &r, NULL) != ABI_OK) {
    abi_case_free(c);
    return;
  }

  /* the consumer releases only the base structures */
  abi_reconstruction_schema(r)->release(abi_reconstruction_schema(r));
  abi_reconstruction_array(r)->release(abi_reconstruction_array(r));

  o = abi_reconstruction_observer(r);
  CHECK(o->violations == 0, "a conforming release must record no violations");

  root_enter = find_event(o, ABI_EV_ARRAY_RELEASE_ENTER, "/");
  child_enter = find_event(o, ABI_EV_ARRAY_RELEASE_ENTER, "/0/");
  CHECK(root_enter != NULL && child_enter != NULL, "release events recorded");
  if (root_enter && child_enter) {
    /*
     * This is the distinction counters cannot make: the root was entered from
     * outside (depth 0, by the consumer) and the child from inside the
     * producer's own callback (depth 1).
     */
    CHECK(root_enter->depth == 0 && root_enter->by_consumer == 1,
          "root release is entered by the consumer at depth 0");
    CHECK(child_enter->depth == 1 && child_enter->by_consumer == 0,
          "child release is entered by the producer at depth 1");
    CHECK(root_enter->seq < child_enter->seq,
          "the parent must be entered before its child is released");
  }

  /* every ENTER has its EXIT */
  CHECKF(count_events(o, ABI_EV_ARRAY_RELEASE_ENTER) ==
             count_events(o, ABI_EV_ARRAY_RELEASE_EXIT),
         "array release enter/exit mismatch: %d vs %d",
         count_events(o, ABI_EV_ARRAY_RELEASE_ENTER),
         count_events(o, ABI_EV_ARRAY_RELEASE_EXIT));

  /* the dictionary is part of the tree the producer owns */
  CHECK(find_event(o, ABI_EV_ARRAY_RELEASE_ENTER, "/1/dict/") != NULL,
        "dictionary array released by the producer");

  /* measured, not declared */
  CHECKF(!abi_reconstruction_leaked(r),
         "allocation delta after release: %lld bytes / %lld blocks",
         (long long)(o->alloc.bytes_allocated - o->alloc.bytes_freed),
         (long long)(o->alloc.blocks_allocated - o->alloc.blocks_freed));
  CHECK(o->alloc.blocks_allocated > 0, "the allocator was actually exercised");

  printf("  release log for the rich fixture:\n");
  abi_reconstruction_print_log(r, stdout);

  abi_reconstruction_free(r);
  abi_case_free(c);
}

/*
 * C-obs: the consumer must release only the base structure. Reaching in and
 * releasing a child directly is a violation, and the point of the event log is
 * that it is observed rather than deduced from a counter.
 */
static void test_consumer_releasing_child_is_a_violation(void) {
  AbiCase           *c = abi_fixture_rich();
  AbiReconstruction *r = NULL;
  const AbiObserver *o;
  struct ArrowArray *a;
  const AbiEvent    *v;

  if (!c) return;
  if (abi_reconstruct(c, &r, NULL) != ABI_OK) {
    abi_case_free(c);
    return;
  }
  a = abi_reconstruction_array(r);

  /* the misuse under test */
  a->children[0]->release(a->children[0]);

  o = abi_reconstruction_observer(r);
  CHECKF(o->violations == 1, "expected 1 violation, observed %lu",
         (unsigned long)o->violations);
  v = find_event(o, ABI_EV_VIOLATION_CHILD_RELEASED_BY_CONSUMER, "/0/");
  CHECK(v != NULL, "the violation is recorded against the child's path");
  if (v) CHECK(v->depth == 0, "a consumer-entered release is at depth 0");

  /* the parent must survive releasing an already-released child */
  a->release(a);
  CHECK(a->release == NULL, "root still releases cleanly afterwards");
  /* schema and array have independent lifetimes; both must go before the
     allocation delta means anything */
  abi_reconstruction_schema(r)->release(abi_reconstruction_schema(r));
  CHECKF(!abi_reconstruction_leaked(r),
         "delta after a misused release: %lld bytes",
         (long long)(o->alloc.bytes_allocated - o->alloc.bytes_freed));

  abi_reconstruction_free(r);
  abi_case_free(c);
}

/*
 * The specification lets a consumer move a structure with a bitwise copy, mark
 * the source released, and invoke the callback on the destination. A producer
 * may not assume the structure stays at one address, which is what private_data
 * is for. This is the positive half of the pair; the negative half -- a
 * producer storing pointers into its own struct -- is a Corpus B1 case.
 */
static void test_move_semantics(void) {
  AbiCase           *c = abi_fixture_rich();
  AbiReconstruction *r = NULL;
  struct ArrowArray  moved;
  struct ArrowArray *src;

  if (!c) return;
  if (abi_reconstruct(c, &r, NULL) != ABI_OK) {
    abi_case_free(c);
    return;
  }
  src = abi_reconstruction_array(r);

  memcpy(&moved, src, sizeof(moved)); /* bitwise move */
  src->release = NULL;                /* source marked released, no callback */

  CHECK(moved.release != NULL, "the destination owns the release callback");
  moved.release(&moved);
  CHECK(moved.release == NULL, "released after the move");
  abi_reconstruction_schema(r)->release(abi_reconstruction_schema(r));

  CHECKF(!abi_reconstruction_leaked(r), "delta after a move: %lld bytes",
         (long long)(abi_reconstruction_observer(r)->alloc.bytes_allocated -
                     abi_reconstruction_observer(r)->alloc.bytes_freed));
  CHECK(abi_reconstruction_observer(r)->violations == 0,
        "a move is conforming and must not be flagged");

  abi_reconstruction_free(r);
  abi_case_free(c);
}

/* A consumer that imports and never releases: we clean up and say so. */
static void test_unreleased_is_cleaned_up_and_logged(void) {
  AbiCase           *c = abi_fixture_rich();
  AbiReconstruction *r = NULL;
  const AbiObserver *o;
  int                harness_releases;

  if (!c) return;
  if (abi_reconstruct(c, &r, NULL) != ABI_OK) {
    abi_case_free(c);
    return;
  }
  /* deliberately no release call by the "consumer" */

  abi_reconstruction_release_all(r);
  o = abi_reconstruction_observer(r);

  harness_releases = count_events(o, ABI_EV_HARNESS_RELEASED);
  CHECKF(harness_releases == 2,
         "expected 2 HARNESS_RELEASED events (schema and array), got %d",
         harness_releases);
  /*
   * The harness enters the release at depth 0, exactly where a consumer
   * would, so depth alone attributes this cleanup to a consumer that released
   * nothing -- and every report then says released_by_consumer for it.
   */
  {
    uint32_t i;
    int      attributed = 0;
    for (i = 0; i < o->event_count; i++) {
      const AbiEvent *e = &o->events[i];
      if ((e->kind == ABI_EV_SCHEMA_RELEASE_ENTER ||
           e->kind == ABI_EV_ARRAY_RELEASE_ENTER) &&
          e->by_consumer)
        attributed++;
    }
    CHECKF(attributed == 0,
           "%d harness release(s) attributed to a consumer that released "
           "nothing",
           attributed);
  }
  CHECKF(!abi_reconstruction_leaked(r),
         "harness cleanup must balance: %lld bytes / %lld blocks",
         (long long)(o->alloc.bytes_allocated - o->alloc.bytes_freed),
         (long long)(o->alloc.blocks_allocated - o->alloc.blocks_freed));

  abi_reconstruction_free(r);
  abi_case_free(c);
}

/* --- class B1 shapes ------------------------------------------------------ */

/*
 * A B1 case may declare more buffers than it provides. The extra slots must be
 * real, NULL-initialized pointers: a consumer walking n_buffers then hits a
 * NULL, which is the defect under test and is unambiguously in the consumer.
 * Allocating only the provided count would have the consumer read past our
 * array instead, making the fault ours.
 */
static void test_b1_overdeclared_counts(void) {
  AbiCase           *c = abi_case_new(ABI_CLASS_B1);
  AbiReconstruction *r = NULL;
  AbiArrayNode      *node;
  struct ArrowArray *a;
  uint8_t            data[16];
  uint32_t           id = 0;
  int64_t            i;

  if (!c) return;
  memset(data, 0x7E, sizeof(data));
  abi_case_add_allocation(c, data, sizeof(data), 8, &id);
  abi_case_set_schema(c, abi_schema_new(c, "i"));

  node = abi_array_new(c);
  node->length = 4;
  abi_array_add_null_buffer(c, node, ABI_ROLE_VALIDITY);
  abi_array_add_buffer(c, node, ABI_ROLE_DATA, id, 0, 16);
  abi_array_set_declared_buffers(node,
                                 7); /* after the adds -- see the header */
  abi_case_set_array(c, node);

  CHECK(abi_case_validate(c, NULL) == ABI_OK, "B1 over-declaration is valid");
  if (abi_reconstruct(c, &r, NULL) != ABI_OK) {
    abi_case_free(c);
    return;
  }
  a = abi_reconstruction_array(r);
  CHECKF(a->n_buffers == 7, "n_buffers reports the declared %lld, not 2",
         (long long)a->n_buffers);
  CHECK(a->buffers[1] != NULL, "the provided data buffer is present");
  for (i = 2; i < 7; i++) {
    CHECKF(a->buffers[i] == NULL, "over-declared slot %lld must be NULL",
           (long long)i);
  }

  a->release(a);
  abi_reconstruction_schema(r)->release(abi_reconstruction_schema(r));
  CHECKF(!abi_reconstruction_leaked(r), "delta on an over-declared case: %lld",
         (long long)(abi_reconstruction_observer(r)->alloc.bytes_allocated -
                     abi_reconstruction_observer(r)->alloc.bytes_freed));
  abi_reconstruction_free(r);
  abi_case_free(c);
}

static void test_schema_only_case(void) {
  AbiCase           *c = abi_fixture_minimal();
  AbiReconstruction *r = NULL;

  if (!c) return;
  CHECK(abi_reconstruct(c, &r, NULL) == ABI_OK, "schema-only reconstructs");
  if (r) {
    CHECK(abi_reconstruction_schema(r) != NULL, "schema present");
    CHECK(abi_reconstruction_array(r) == NULL,
          "no array for a schema-only case");
    CHECK(strcmp(abi_reconstruction_schema(r)->format, "i") == 0,
          "format is i");
    abi_reconstruction_schema(r)->release(abi_reconstruction_schema(r));
    CHECK(!abi_reconstruction_leaked(r), "schema-only release balances");
    abi_reconstruction_free(r);
  }
  abi_case_free(c);
}

/* Every case round-tripped through the file must reconstruct identically. */
static void test_reconstruct_from_file_bytes(void) {
  AbiCase           *c = abi_fixture_rich();
  uint8_t           *bytes = NULL;
  size_t             n = 0;
  AbiCase           *back = NULL;
  AbiReconstruction *r1 = NULL, *r2 = NULL;

  if (!c) return;
  if (abi_case_encode(c, &bytes, &n) != ABI_OK ||
      abi_case_decode(bytes, n, &back, NULL) != ABI_OK) {
    CHECK(0, "encode/decode round trip failed");
    abi_case_free(c);
    abi_free(bytes);
    return;
  }
  if (abi_reconstruct(c, &r1, NULL) == ABI_OK &&
      abi_reconstruct(back, &r2, NULL) == ABI_OK) {
    struct ArrowArray *a1 = abi_reconstruction_array(r1);
    struct ArrowArray *a2 = abi_reconstruction_array(r2);
    CHECK(a1->length == a2->length && a1->offset == a2->offset,
          "reconstruction from decoded bytes matches the original case");
    CHECK(a1->n_buffers == a2->n_buffers, "buffer counts match");
    /* aliasing must survive the file, not just the in-memory case */
    CHECK(a2->children[0]->buffers[0] == a2->children[1]->buffers[0],
          "aliasing survives a trip through the file format");
    CHECK(memcmp(a1->children[0]->buffers[1], a2->children[0]->buffers[1],
                 32) == 0,
          "buffer contents match after a file round trip");
  }
  abi_reconstruction_free(r1);
  abi_reconstruction_free(r2);
  abi_case_free(back);
  abi_case_free(c);
  abi_free(bytes);
}

/*
 * The B1 fixture behind an M0.5 finding. It must be a well-formed *container*
 * even though the Arrow data it describes is invalid -- those are independent
 * notions, and a case that cannot be built or replayed cannot be reported.
 */
static void test_bad_dict_index_fixture(void) {
  AbiCase           *c = abi_fixture_bad_dict_index();
  AbiReconstruction *r = NULL;
  struct ArrowArray *a;

  if (!c) return;
  CHECKF(abi_case_validate(c, NULL) == ABI_OK,
         "the B1 fixture must be a valid container, got %s",
         abi_status_str(abi_case_validate(c, NULL)));
  CHECK(c->cls == ABI_CLASS_B1, "declared class B1");
  CHECK(c->expected.expected_outcome == ABI_EXPECT_REJECT,
        "expected outcome is a clean rejection");
  {
    uint8_t *b1 = NULL, *b2 = NULL;
    size_t   n1 = 0, n2 = 0;
    AbiCase *back = NULL;
    if (abi_case_encode(c, &b1, &n1) == ABI_OK &&
        abi_case_decode(b1, n1, &back, NULL) == ABI_OK &&
        abi_case_encode(back, &b2, &n2) == ABI_OK) {
      CHECK(n1 == n2 && memcmp(b1, b2, n1) == 0,
            "the B1 fixture round-trips byte-identically");
    } else {
      CHECK(0, "the B1 fixture must survive the file format");
    }
    abi_case_free(back);
    abi_free(b1);
    abi_free(b2);
  }

  CHECK(abi_reconstruct(c, &r, NULL) == ABI_OK, "reconstructs");
  if (r) {
    a = abi_reconstruction_array(r);
    CHECK(a->children[0]->dictionary != NULL, "dictionary array present");
    /* the out-of-range index really is in the buffer we hand over */
    CHECK(((const uint8_t *)a->children[0]->buffers[1])[2] == 99,
          "index 99 is present in the reconstructed data buffer");
    CHECK(a->children[0]->dictionary->length == 2, "dictionary has two values");
    a->release(a);
    abi_reconstruction_schema(r)->release(abi_reconstruction_schema(r));
    CHECK(!abi_reconstruction_leaked(r), "B1 fixture release balances");
    abi_reconstruction_free(r);
  }
  abi_case_free(c);
}

int main(void) {
  RUN(test_reconstruct_structure);
  RUN(test_metadata_wire_format);
  RUN(test_release_ordering_and_balance);
  RUN(test_consumer_releasing_child_is_a_violation);
  RUN(test_move_semantics);
  RUN(test_unreleased_is_cleaned_up_and_logged);
  RUN(test_b1_overdeclared_counts);
  RUN(test_schema_only_case);
  RUN(test_reconstruct_from_file_bytes);
  RUN(test_bad_dict_index_fixture);

  printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
