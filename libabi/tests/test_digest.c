/*
 * Dual digest tests -- issue #7.
 *
 * The digest exists to tell "the data changed" apart from "the layout changed",
 * so the tests are pairs: two arrays that differ in exactly one way, and an
 * assertion about which of the two digests moves. A test that only checked
 * "equal digests for equal input" would pass against a function returning a
 * constant, so every property below is paired with a case that must differ.
 */
#include <stdio.h>
#include <string.h>

#include "abi/digest.h"

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

#define RUN(fn)                                                                \
  do {                                                                         \
    g_test = #fn;                                                              \
    fn();                                                                      \
  } while (0)

/* --- building arrays by hand --------------------------------------------- */

typedef struct {
  struct ArrowSchema  schema;
  struct ArrowSchema  child_schema;
  struct ArrowSchema *child_schemas[1];
  struct ArrowArray   array;
  struct ArrowArray   child_array;
  struct ArrowArray  *child_arrays[1];
  const void         *buffers[3];
  const void         *child_buffers[3];
} TestArray;

static void set_schema(struct ArrowSchema *s, const char *format) {
  memset(s, 0, sizeof(*s));
  s->format = format;
  s->name = "f0";
  s->flags = ARROW_FLAG_NULLABLE;
}

/* A flat array of `format` over the given buffers. */
static void make(TestArray *t, const char *format, int64_t length,
                 int64_t offset, int64_t null_count, const void *validity,
                 const void *b1, const void *b2) {
  memset(t, 0, sizeof(*t));
  set_schema(&t->schema, format);
  t->buffers[0] = validity;
  t->buffers[1] = b1;
  t->buffers[2] = b2;
  t->array.length = length;
  t->array.null_count = null_count;
  t->array.offset = offset;
  t->array.n_buffers = b2 ? 3 : 2;
  t->array.buffers = t->buffers;
}

/* struct<f0: `format`>, the shape the generated corpus uses. */
static void make_struct(TestArray *t, const char *format, int64_t length,
                        int64_t child_offset, const void *child_validity,
                        const void *b1, const void *b2) {
  memset(t, 0, sizeof(*t));
  set_schema(&t->schema, "+s");
  set_schema(&t->child_schema, format);
  t->child_schemas[0] = &t->child_schema;
  t->schema.n_children = 1;
  t->schema.children = t->child_schemas;

  t->buffers[0] = NULL; /* struct validity omitted: no nulls */
  t->array.length = length;
  t->array.n_buffers = 1;
  t->array.buffers = t->buffers;
  t->array.n_children = 1;
  t->child_arrays[0] = &t->child_array;
  t->array.children = t->child_arrays;

  t->child_buffers[0] = child_validity;
  t->child_buffers[1] = b1;
  t->child_buffers[2] = b2;
  t->child_array.length = length;
  t->child_array.null_count = -1;
  t->child_array.offset = child_offset;
  t->child_array.n_buffers = b2 ? 3 : 2;
  t->child_array.buffers = t->child_buffers;
}

static AbiDigest digest_of(const TestArray *t, const char *what) {
  AbiDigest d;
  AbiError  err;
  AbiStatus st = abi_digest(&t->schema, &t->array, &d, &err);
  if (st != ABI_OK) {
    g_fails++;
    fprintf(stderr, "FAIL [%s] digest of %s failed: %s (%s)\n", g_test, what,
            abi_status_str(st), err.message);
    memset(&d, 0, sizeof(d));
  }
  g_checks++;
  return d;
}

static int same_logical(const AbiDigest *a, const AbiDigest *b) {
  return memcmp(a->logical, b->logical, ABI_DIGEST_BYTES) == 0;
}

static int same_physical(const AbiDigest *a, const AbiDigest *b) {
  return memcmp(a->physical, b->physical, ABI_DIGEST_BYTES) == 0;
}

/* Stores a native int32 at an arbitrary byte offset, alignment be damned. */
static void put_i32_at(uint8_t *base, size_t byte_offset, int32_t v) {
  memcpy(base + byte_offset, &v, sizeof(v));
}

static void put_f64_bits_at(uint8_t *base, size_t byte_offset, uint64_t bits) {
  double d;
  memcpy(&d, &bits, sizeof(d));
  memcpy(base + byte_offset, &d, sizeof(d));
}

/* --- the properties ------------------------------------------------------ */

/*
 * The false positive the dual digest exists to remove: a consumer that keeps a
 * non-zero offset and one that materializes the slice hold the same values in
 * different layouts.
 */
static void test_offset_changes_layout_not_data(void) {
  uint8_t   flat[3 * 4], sliced[4 * 4];
  uint8_t   flat_valid[1] = {0x07};   /* slots 0,1,2 valid */
  uint8_t   sliced_valid[1] = {0x0E}; /* slot 0 null, 1,2,3 valid */
  TestArray a, b;
  AbiDigest da, db;
  int       i;

  for (i = 0; i < 3; i++) {
    put_i32_at(flat, (size_t)i * 4, 1000 + i);
  }
  put_i32_at(sliced, 0, -777); /* outside the window, and null besides */
  for (i = 0; i < 3; i++) {
    put_i32_at(sliced, (size_t)(i + 1) * 4, 1000 + i);
  }

  make(&a, "i", 3, 0, 0, flat_valid, flat, NULL);
  make(&b, "i", 3, 1, 0, sliced_valid, sliced, NULL);
  da = digest_of(&a, "offset 0");
  db = digest_of(&b, "offset 1");

  CHECK(same_logical(&da, &db),
        "the same three values at offset 0 and offset 1 must agree logically");
  CHECK(!same_physical(&da, &db),
        "different offsets are a layout difference and must not agree "
        "physically");
}

/* And the guard on the above: a digest that ignored values would also pass. */
static void test_a_changed_value_changes_the_logical_digest(void) {
  uint8_t   one[2 * 4], two[2 * 4];
  uint8_t   valid[1] = {0x03};
  TestArray a, b;
  AbiDigest da, db;

  put_i32_at(one, 0, 1000);
  put_i32_at(one, 4, 1001);
  put_i32_at(two, 0, 1000);
  put_i32_at(two, 4, 1002);

  make(&a, "i", 2, 0, 0, valid, one, NULL);
  make(&b, "i", 2, 0, 0, valid, two, NULL);
  da = digest_of(&a, "1000,1001");
  db = digest_of(&b, "1000,1002");

  CHECK(!same_logical(&da, &db), "a changed value must change the logical "
                                 "digest");
  CHECK(!same_physical(&da, &db), "a changed value must change the physical "
                                  "digest too");
}

/* Bytes under a null slot are unspecified; a producer may leave anything. */
static void test_null_slot_bytes_are_ignored(void) {
  uint8_t   one[2 * 4], two[2 * 4];
  uint8_t   valid[1] = {0x02}; /* slot 0 null, slot 1 valid */
  TestArray a, b;
  AbiDigest da, db;

  put_i32_at(one, 0, 0);
  put_i32_at(one, 4, 1001);
  put_i32_at(two, 0, 0x5A5A5A5A); /* garbage under the null */
  put_i32_at(two, 4, 1001);

  make(&a, "i", 2, 0, 1, valid, one, NULL);
  make(&b, "i", 2, 0, 1, valid, two, NULL);
  da = digest_of(&a, "zeroed null slot");
  db = digest_of(&b, "garbage null slot");

  CHECK(same_logical(&da, &db),
        "bytes under a null slot must not reach the logical digest");
  CHECK(!same_physical(&da, &db),
        "the physical digest covers the buffers as laid out, garbage included");
}

static void test_signed_zero_and_nan_payloads(void) {
  uint8_t   pos[8], neg[8], nan1[8], nan2[8];
  uint8_t   valid[1] = {0x01};
  TestArray a, b;
  AbiDigest da, db;

  put_f64_bits_at(pos, 0, 0x0000000000000000ull);  /* +0.0 */
  put_f64_bits_at(neg, 0, 0x8000000000000000ull);  /* -0.0 */
  put_f64_bits_at(nan1, 0, 0x7FF8000000000001ull); /* NaN, payload 1 */
  put_f64_bits_at(nan2, 0, 0x7FF8000000000002ull); /* NaN, payload 2 */

  make(&a, "g", 1, 0, 0, valid, pos, NULL);
  make(&b, "g", 1, 0, 0, valid, neg, NULL);
  da = digest_of(&a, "+0.0");
  db = digest_of(&b, "-0.0");
  CHECK(same_logical(&da, &db), "-0.0 and +0.0 are the same logical value");
  CHECK(!same_physical(&da, &db), "-0.0 and +0.0 are different bit patterns");

  make(&a, "g", 1, 0, 0, valid, nan1, NULL);
  make(&b, "g", 1, 0, 0, valid, nan2, NULL);
  da = digest_of(&a, "NaN payload 1");
  db = digest_of(&b, "NaN payload 2");
  CHECK(same_logical(&da, &db), "NaN payloads are not logical content");
  CHECK(!same_physical(&da, &db), "different NaN payloads are different bytes");
}

/* The alignment classes of the model must not move the data. */
static void test_misalignment_is_not_a_data_change(void) {
  uint8_t   aligned[2 * 4], shifted[1 + 2 * 4];
  uint8_t   valid[1] = {0x03};
  TestArray a, b;
  AbiDigest da, db;

  put_i32_at(aligned, 0, 1000);
  put_i32_at(aligned, 4, 1001);
  put_i32_at(shifted, 1, 1000);
  put_i32_at(shifted, 5, 1001);

  make(&a, "i", 2, 0, 0, valid, aligned, NULL);
  make(&b, "i", 2, 0, 0, valid, shifted + 1, NULL);
  da = digest_of(&a, "naturally aligned");
  db = digest_of(&b, "one byte in");

  CHECK(same_logical(&da, &db),
        "a view one byte into its allocation holds the same values");
  CHECK(same_physical(&da, &db),
        "the physical digest covers the bytes of the view, not its address");
}

static void test_utf8_values(void) {
  /* "aa", "bb" and "aa", "bc" over offsets [0,2,4] */
  uint8_t   offsets[3 * 4];
  uint8_t   valid[1] = {0x03};
  TestArray a, b;
  AbiDigest da, db;

  put_i32_at(offsets, 0, 0);
  put_i32_at(offsets, 4, 2);
  put_i32_at(offsets, 8, 4);

  make(&a, "u", 2, 0, 0, valid, offsets, "aabb");
  make(&b, "u", 2, 0, 0, valid, offsets, "aabc");
  da = digest_of(&a, "aa,bb");
  db = digest_of(&b, "aa,bc");
  CHECK(!same_logical(&da, &db), "utf8 bytes are compared raw");

  make(&b, "u", 2, 0, 0, valid, offsets, "aabb");
  db = digest_of(&b, "aa,bb again");
  CHECK(same_logical(&da, &db), "the same strings agree");
  CHECK(same_physical(&da, &db), "and agree physically");
}

/* A struct's window travels to its children -- the corpus shape. */
static void test_struct_child_offset(void) {
  uint8_t   flat[2 * 4], sliced[3 * 4];
  uint8_t   flat_valid[1] = {0x03};
  uint8_t   sliced_valid[1] = {0x06};
  TestArray a, b;
  AbiDigest da, db;

  put_i32_at(flat, 0, 7);
  put_i32_at(flat, 4, 8);
  put_i32_at(sliced, 0, -1);
  put_i32_at(sliced, 4, 7);
  put_i32_at(sliced, 8, 8);

  make_struct(&a, "i", 2, 0, flat_valid, flat, NULL);
  make_struct(&b, "i", 2, 1, sliced_valid, sliced, NULL);
  da = digest_of(&a, "struct, child at offset 0");
  db = digest_of(&b, "struct, child at offset 1");

  CHECK(same_logical(&da, &db),
        "a struct child's own offset is layout, not data");
  CHECK(!same_physical(&da, &db), "the child's offset is a layout difference");
}

static void test_unsupported_types_are_refused(void) {
  uint8_t   data[8];
  uint8_t   valid[1] = {0x01};
  TestArray a;
  AbiDigest d;
  AbiError  err;
  AbiStatus st;

  memset(data, 0, sizeof(data));
  make(&a, "f", 1, 0, 0, valid, data, NULL); /* float32: not in the v0 set */
  st = abi_digest(&a.schema, &a.array, &d, &err);
  g_checks++;
  if (st == ABI_OK) {
    g_fails++;
    fprintf(stderr, "FAIL [%s] float32 was digested rather than refused\n",
            g_test);
  }
  CHECK(strstr(err.message, "\"f\"") != NULL,
        "the error names the format string it could not handle");
}

/*
 * Two ways an array can ask this code to read past a buffer. Refusing is the
 * correct answer: a fault here would be ours and would be reported against
 * whoever handed the array over, which is the same reason validate.c enforces
 * containment on a case.
 */
static void test_out_of_range_windows_are_refused(void) {
  uint8_t   data[3 * 4], offsets[3 * 4];
  uint8_t   valid[1] = {0x07};
  TestArray a;
  AbiDigest d;
  AbiError  err;
  AbiStatus st;

  /* A struct child too short for the window its parent projects onto it. */
  memset(data, 0, sizeof(data));
  make_struct(&a, "i", 2, 0, valid, data, NULL);
  a.array.offset = 1;       /* parent window starts at 1 ... */
  a.child_array.length = 2; /* ... but the child only has two slots */
  st = abi_digest(&a.schema, &a.array, &d, &err);
  g_checks++;
  if (st == ABI_OK) {
    g_fails++;
    fprintf(stderr,
            "FAIL [%s] a child shorter than its parent's window was "
            "digested rather than refused\n",
            g_test);
  }

  /*
   * utf8 offsets that run past the values buffer they describe.
   *
   * Note what this check does and does not discriminate. The array is refused
   * either way, because slot 1's offsets are not monotonic -- but without the
   * bound against the values length, slot 0 is read *first*, and that read runs
   * off the end. Removing the bound and running this suite under ASan reports
   * "global-buffer-overflow ... READ of size 46" on a ten-byte buffer, which is
   * the only way the difference is observable. The return value alone cannot
   * tell the two apart.
   */
  put_i32_at(offsets, 0, 0);
  put_i32_at(offsets, 4, 50); /* slot 0 claims 50 bytes ... */
  put_i32_at(offsets, 8, 10); /* ... of a 10-byte values buffer */
  make(&a, "u", 2, 0, 0, valid, offsets, "0123456789");
  st = abi_digest(&a.schema, &a.array, &d, &err);
  g_checks++;
  if (st == ABI_OK) {
    g_fails++;
    fprintf(stderr,
            "FAIL [%s] utf8 offsets past the values buffer were "
            "digested rather than refused\n",
            g_test);
  }
}

static void test_digest_is_deterministic(void) {
  uint8_t   data[4];
  uint8_t   valid[1] = {0x01};
  TestArray a;
  AbiDigest first, second;

  put_i32_at(data, 0, 42);
  make(&a, "i", 1, 0, 0, valid, data, NULL);
  first = digest_of(&a, "first");
  second = digest_of(&a, "second");
  CHECK(same_logical(&first, &second) && same_physical(&first, &second),
        "digesting the same array twice must give the same answer");
}

int main(void) {
  RUN(test_offset_changes_layout_not_data);
  RUN(test_a_changed_value_changes_the_logical_digest);
  RUN(test_null_slot_bytes_are_ignored);
  RUN(test_signed_zero_and_nan_payloads);
  RUN(test_misalignment_is_not_a_data_change);
  RUN(test_utf8_values);
  RUN(test_struct_child_offset);
  RUN(test_unsupported_types_are_refused);
  RUN(test_out_of_range_windows_are_refused);
  RUN(test_digest_is_deterministic);

  printf("%d checks, %d failure(s)\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
