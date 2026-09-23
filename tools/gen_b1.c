/*
 * abicase-gen-b1 -- build Corpus B1: producers that are invalid in structure
 * or data, each case resting on one quoted clause.
 *
 *   abicase-gen-b1 <out-dir>     writes <name>.abicase and manifest.tsv
 *
 * B1 is not a product of equivalence classes (docs/coverage-matrix.md 0). It is
 * an enumerated list of specific defects, so each case is written out here by
 * hand, with its clause beside it, and the committed files are what this
 * program produces: tools/check_b1.py regenerates them and requires the bytes
 * to match, so a case cannot drift from its definition.
 *
 * Every case has a `spec_clause` that names an entry of docs/spec-citations.md,
 * where the clause is quoted verbatim. Two kinds of expectation:
 *
 *   REJECT       the defect is in something a consumer can see -- a pointer, a
 *                count, a value -- so a validator at the declared level must
 *                refuse it, and say why.
 *   UNSPECIFIED  the defect is a buffer smaller than the array needs. The C
 *                Data Interface carries no buffer sizes, so no consumer can
 *                see it; the specification puts the obligation on the
 *                producer alone. What a consumer does with one is recorded,
 *                never judged: a crash here is not, by itself, a defect.
 *
 * Framing, as everywhere in this project: robustness against trusted-but-
 * invalid producers and against validation boundaries, never hardening against
 * malicious ones (docs/spec-citations.md, the framing rule).
 *
 * Every case is one field inside a struct, like Corpus A, so that a record-
 * batch importer reaches the defect instead of refusing the shape first.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "abi/abicase.h"
#include "abi/arrow_abi.h"
#include "fixture.h"

#define GEN_VERSION "abicase-gen-b1 1"

typedef struct {
  AbiCase       *c;
  AbiSchemaNode *field_s;
  AbiArrayNode  *root_a;
  AbiArrayNode  *field_a;
} Shell;

static void put_i32(uint8_t *p, int32_t v) {
  uint32_t u = (uint32_t)v;
  p[0] = (uint8_t)u;
  p[1] = (uint8_t)(u >> 8);
  p[2] = (uint8_t)(u >> 16);
  p[3] = (uint8_t)(u >> 24);
}

/* struct<f0: format> of `length` rows, root validity omitted. */
static int shell(Shell *s, const char *format, int64_t length,
                 AbiValidationLevel level, AbiExpectedOutcome outcome,
                 const char *clause, const char *notes) {
  AbiSchemaNode *root_s;

  memset(s, 0, sizeof(*s));
  s->c = abi_case_new(ABI_CLASS_B1);
  if (!s->c) return 0;
  if (abi_case_set_provenance(s->c, 0, "", 0, GEN_VERSION, ABI_DOCTOR_VERSION,
                              "") != ABI_OK ||
      abi_case_set_expected(s->c, level, outcome, clause, notes) != ABI_OK) {
    return 0;
  }
  root_s = abi_schema_new(s->c, "+s");
  s->field_s = abi_schema_new(s->c, format);
  if (!root_s || !s->field_s ||
      abi_schema_set_name(s->c, root_s, "") != ABI_OK ||
      abi_schema_set_name(s->c, s->field_s, "f0") != ABI_OK) {
    return 0;
  }
  s->field_s->flags = ARROW_FLAG_NULLABLE;
  if (abi_schema_add_child(s->c, root_s, s->field_s) != ABI_OK) return 0;
  abi_case_set_schema(s->c, root_s);

  s->root_a = abi_array_new(s->c);
  s->field_a = abi_array_new(s->c);
  if (!s->root_a || !s->field_a) return 0;
  s->root_a->length = length;
  s->field_a->length = length;
  if (abi_array_add_null_buffer(s->c, s->root_a, ABI_ROLE_VALIDITY) != ABI_OK) {
    return 0;
  }
  abi_case_set_array(s->c, s->root_a);
  return 1;
}

/* Attaches the field and the default call sequence. */
static AbiCase *finish(Shell *s) {
  if (abi_array_add_child(s->c, s->root_a, s->field_a) != ABI_OK ||
      abi_case_add_op(s->c, ABI_OP_IMPORT_SCHEMA, 0, 0) != ABI_OK ||
      abi_case_add_op(s->c, ABI_OP_IMPORT_ARRAY, 0, 0) != ABI_OK ||
      abi_case_add_op(s->c, ABI_OP_RELEASE_BASE, 0, 0) != ABI_OK) {
    abi_case_free(s->c);
    return NULL;
  }
  return s->c;
}

/* A view onto a fresh allocation holding exactly `bytes`. */
static int view(Shell *s, AbiBufferRole role, const void *bytes,
                uint64_t size) {
  uint32_t id;
  if (abi_case_add_allocation(s->c, bytes, size, 8, &id) != ABI_OK) return 0;
  return abi_array_add_buffer(s->c, s->field_a, role, id, 0, size) == ABI_OK;
}

static const uint8_t INT32_1234[16] = {1, 0, 0, 0, 2, 0, 0, 0,
                                       3, 0, 0, 0, 4, 0, 0, 0};

/* --- detectable: REJECT --------------------------------------------------- */

static AbiCase *validity_absent_with_nulls(void) {
  Shell s;
  if (!shell(&s, "i", 4, ABI_VALIDATE_MINIMAL, ABI_EXPECT_REJECT,
             "cdi-null-buffers",
             "null_count is 1 and the validity buffer is NULL; a NULL "
             "validity buffer is allowed only when null_count is 0"))
    return NULL;
  s.field_a->null_count = 1;
  if (abi_array_add_null_buffer(s.c, s.field_a, ABI_ROLE_VALIDITY) != ABI_OK ||
      !view(&s, ABI_ROLE_DATA, INT32_1234, sizeof(INT32_1234))) {
    abi_case_free(s.c);
    return NULL;
  }
  return finish(&s);
}

static AbiCase *data_buffer_null(void) {
  Shell s;
  if (!shell(&s, "i", 4, ABI_VALIDATE_MINIMAL, ABI_EXPECT_REJECT,
             "cdi-null-buffers",
             "the data buffer of a 4-row int32 array is NULL; its size would "
             "be 16 bytes, not 0"))
    return NULL;
  if (abi_array_add_null_buffer(s.c, s.field_a, ABI_ROLE_VALIDITY) != ABI_OK ||
      abi_array_add_null_buffer(s.c, s.field_a, ABI_ROLE_DATA) != ABI_OK) {
    abi_case_free(s.c);
    return NULL;
  }
  return finish(&s);
}

static AbiCase *children_fewer_than_declared(void) {
  Shell    s;
  AbiCase *c;
  if (!shell(&s, "i", 4, ABI_VALIDATE_MINIMAL, ABI_EXPECT_REJECT,
             "cdi-children",
             "the struct array declares n_children = 2 for a one-field "
             "struct and provides one pointer; the second slot is NULL"))
    return NULL;
  if (abi_array_add_null_buffer(s.c, s.field_a, ABI_ROLE_VALIDITY) != ABI_OK ||
      !view(&s, ABI_ROLE_DATA, INT32_1234, sizeof(INT32_1234))) {
    abi_case_free(s.c);
    return NULL;
  }
  c = finish(&s);
  /* After the add: adding a child resets the declared count. */
  if (c) abi_array_set_declared_children(s.root_a, 2);
  return c;
}

static AbiCase *null_count_disagrees(void) {
  /* Bits 1 and 3 set: slots 0 and 2 are null -- two nulls, one declared. */
  static const uint8_t bitmap[1] = {0x0A};
  Shell                s;
  if (!shell(&s, "i", 4, ABI_VALIDATE_FULL, ABI_EXPECT_REJECT, "cdi-null-count",
             "the validity bitmap has 2 null slots and null_count says 1"))
    return NULL;
  s.field_a->null_count = 1;
  if (!view(&s, ABI_ROLE_VALIDITY, bitmap, sizeof(bitmap)) ||
      !view(&s, ABI_ROLE_DATA, INT32_1234, sizeof(INT32_1234))) {
    abi_case_free(s.c);
    return NULL;
  }
  return finish(&s);
}

static AbiCase *offsets_decreasing(void) {
  uint8_t offsets[16];
  Shell   s;
  if (!shell(&s, "u", 3, ABI_VALIDATE_FULL, ABI_EXPECT_REJECT,
             "col-offsets-monotonic",
             "offsets are 0, 3, 1, 4: slot 1 would have length -2"))
    return NULL;
  put_i32(offsets + 0, 0);
  put_i32(offsets + 4, 3);
  put_i32(offsets + 8, 1);
  put_i32(offsets + 12, 4);
  if (abi_array_add_null_buffer(s.c, s.field_a, ABI_ROLE_VALIDITY) != ABI_OK ||
      !view(&s, ABI_ROLE_OFFSETS, offsets, sizeof(offsets)) ||
      !view(&s, ABI_ROLE_DATA, "abcd", 4)) {
    abi_case_free(s.c);
    return NULL;
  }
  return finish(&s);
}

/* The shared B1 fixture, with this corpus's clause and level. */
static AbiCase *dictionary_index_out_of_range(void) {
  AbiCase *c = abi_fixture_bad_dict_index();
  if (!c) return NULL;
  if (abi_case_set_provenance(c, 0, "", 0, GEN_VERSION, ABI_DOCTOR_VERSION,
                              "") != ABI_OK ||
      abi_case_set_expected(c, ABI_VALIDATE_FULL, ABI_EXPECT_REJECT,
                            "col-dictionary-indices",
                            "index 99 into a two-value dictionary") != ABI_OK) {
    abi_case_free(c);
    return NULL;
  }
  return c;
}

/* --- undetectable: UNSPECIFIED -------------------------------------------- */

#define UNDETECTABLE                                                           \
  " -- not detectable by a consumer: the C Data Interface carries no buffer "  \
  "sizes, and the obligation is the producer's"

static AbiCase *data_buffer_too_small(void) {
  Shell s;
  if (!shell(&s, "i", 4, ABI_VALIDATE_NONE, ABI_EXPECT_UNSPECIFIED,
             "cdi-buffer-size",
             "4 int32 rows over a zero-byte data buffer" UNDETECTABLE))
    return NULL;
  if (abi_array_add_null_buffer(s.c, s.field_a, ABI_ROLE_VALIDITY) != ABI_OK ||
      !view(&s, ABI_ROLE_DATA, NULL, 0)) {
    abi_case_free(s.c);
    return NULL;
  }
  return finish(&s);
}

static AbiCase *offsets_buffer_empty(void) {
  Shell s;
  if (!shell(&s, "u", 0, ABI_VALIDATE_NONE, ABI_EXPECT_UNSPECIFIED,
             "col-offsets-length",
             "a zero-length utf8 array whose offsets buffer is zero bytes; it "
             "must hold one entry -- the realization coverage_model_ver 1 "
             "wrongly called valid" UNDETECTABLE))
    return NULL;
  if (abi_array_add_null_buffer(s.c, s.field_a, ABI_ROLE_VALIDITY) != ABI_OK ||
      !view(&s, ABI_ROLE_OFFSETS, NULL, 0) ||
      !view(&s, ABI_ROLE_DATA, NULL, 0)) {
    abi_case_free(s.c);
    return NULL;
  }
  return finish(&s);
}

static AbiCase *last_offset_beyond_values(void) {
  uint8_t offsets[16];
  Shell   s;
  if (!shell(&s, "u", 3, ABI_VALIDATE_NONE, ABI_EXPECT_UNSPECIFIED,
             "cdi-buffer-size",
             "offsets end at 50 over a 3-byte values buffer" UNDETECTABLE))
    return NULL;
  put_i32(offsets + 0, 0);
  put_i32(offsets + 4, 1);
  put_i32(offsets + 8, 2);
  put_i32(offsets + 12, 50);
  if (abi_array_add_null_buffer(s.c, s.field_a, ABI_ROLE_VALIDITY) != ABI_OK ||
      !view(&s, ABI_ROLE_OFFSETS, offsets, sizeof(offsets)) ||
      !view(&s, ABI_ROLE_DATA, "abc", 3)) {
    abi_case_free(s.c);
    return NULL;
  }
  return finish(&s);
}

static AbiCase *length_beyond_int32(void) {
  Shell s;
  if (!shell(&s, "i", (int64_t)INT32_MAX + 1, ABI_VALIDATE_NONE,
             ABI_EXPECT_UNSPECIFIED, "cdi-buffer-size",
             "2^31 int32 rows over a 16-byte data buffer: a length past "
             "INT32_MAX, declared and not backed" UNDETECTABLE))
    return NULL;
  if (abi_array_add_null_buffer(s.c, s.field_a, ABI_ROLE_VALIDITY) != ABI_OK ||
      !view(&s, ABI_ROLE_DATA, INT32_1234, sizeof(INT32_1234))) {
    abi_case_free(s.c);
    return NULL;
  }
  return finish(&s);
}

/* --- driver --------------------------------------------------------------- */

typedef struct {
  const char *name;
  AbiCase *(*build)(void);
} Entry;

static const Entry CASES[] = {
    {"validity-absent-with-nulls", validity_absent_with_nulls},
    {"data-buffer-null", data_buffer_null},
    {"children-fewer-than-declared", children_fewer_than_declared},
    {"null-count-disagrees", null_count_disagrees},
    {"offsets-decreasing", offsets_decreasing},
    {"dictionary-index-out-of-range", dictionary_index_out_of_range},
    {"data-buffer-too-small", data_buffer_too_small},
    {"offsets-buffer-empty", offsets_buffer_empty},
    {"last-offset-beyond-values", last_offset_beyond_values},
    {"length-beyond-int32", length_beyond_int32},
};

static const char *outcome_name(uint8_t o) {
  static const char *names[] = {"ACCEPT", "REJECT", "EITHER", "UNSPECIFIED"};
  return o <= ABI_EXPECT_UNSPECIFIED ? names[o] : "?";
}

static const char *level_name(uint8_t l) {
  static const char *names[] = {"none", "minimal", "default", "full"};
  return l <= ABI_VALIDATE_FULL ? names[l] : "?";
}

int main(int argc, char **argv) {
  char   path[4096];
  FILE  *manifest;
  size_t i;

  if (argc != 2) {
    fprintf(stderr, "usage: abicase-gen-b1 <out-dir>\n");
    return 2;
  }
  snprintf(path, sizeof(path), "%s/manifest.tsv", argv[1]);
  manifest = fopen(path, "wb");
  if (!manifest) {
    fprintf(stderr, "error: cannot write %s\n", path);
    return 1;
  }
  fprintf(manifest,
          "# name\tcase_id\texpected\tvalidation_level\tspec_clause\n");
  for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
    AbiCase  *c = CASES[i].build();
    AbiStatus st;
    char      id[ABICASE_ID_HEX_SIZE];

    if (!c) {
      fprintf(stderr, "error: %s: could not build\n", CASES[i].name);
      fclose(manifest);
      return 1;
    }
    snprintf(path, sizeof(path), "%s/%s.abicase", argv[1], CASES[i].name);
    st = abi_case_write_file(c, path);
    if (st == ABI_OK) st = abi_case_id(c, id);
    if (st != ABI_OK) {
      fprintf(stderr, "error: %s: %s\n", CASES[i].name, abi_status_str(st));
      abi_case_free(c);
      fclose(manifest);
      return 1;
    }
    fprintf(manifest, "%s\t%s\t%s\t%s\t%.*s\n", CASES[i].name, id,
            outcome_name(c->expected.expected_outcome),
            level_name(c->expected.validation_level),
            (int)c->expected.spec_clause.size,
            (const char *)c->expected.spec_clause.data);
    abi_case_free(c);
  }
  fclose(manifest);
  printf("wrote %lu B1 case(s) to %s\n",
         (unsigned long)(sizeof(CASES) / sizeof(CASES[0])), argv[1]);
  return 0;
}
