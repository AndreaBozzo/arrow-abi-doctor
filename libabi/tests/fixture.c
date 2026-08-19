#include <string.h>

#include "fixture.h"

AbiCase *abi_fixture_rich(void) {
  AbiCase       *c = abi_case_new(ABI_CLASS_A);
  AbiSchemaNode *root, *s_int, *s_dict_idx, *s_dict_val;
  AbiArrayNode  *a_root, *a_int, *a_dict_idx, *a_dict_val;
  uint32_t       al_validity = 0, al_offsets = 0, al_data = 0, al_text = 0,
                 al_runs = 0;
  uint8_t validity[16], offsets[64], data[32], text[8], runs[16];
  size_t  i;

  if (!c) return NULL;

  memset(validity, 0xFF, sizeof(validity));            /* PATTERN period 1 */
  memset(offsets, 0, sizeof(offsets));                 /* ZERO             */
  for (i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 7 + 3); /* RAW */
  memcpy(text, "abcdefgh", 8);                         /* RAW              */
  /* dictionary indices, and they must be in range for a 2-value
     dictionary: this is a class A case, so the data has to be valid.
     Still two runs, so the RLE fill encoding is still exercised. */
  for (i = 0; i < sizeof(runs); i++) runs[i] = (uint8_t)(i < 8 ? 0x00 : 0x01);

  abi_case_set_provenance(c, 0xDEADBEEFCAFEBABEull, "xoshiro256++", 1,
                          "handwritten", ABI_DOCTOR_VERSION,
                          "arrow-c-data-2026-03");
  abi_case_set_expected(c, ABI_VALIDATE_FULL, ABI_EXPECT_ACCEPT,
                        "C Data Interface: Structure definitions",
                        "rich fixture: alias, misalignment, dictionary");

  abi_case_add_allocation(c, validity, sizeof(validity), 8, &al_validity);
  abi_case_add_allocation(c, offsets, sizeof(offsets), 8, &al_offsets);
  abi_case_add_allocation(c, data, sizeof(data), 8, &al_data);
  abi_case_add_allocation(c, text, sizeof(text), 1, &al_text);
  abi_case_add_allocation(c, runs, sizeof(runs), 64, &al_runs);

  /* schema: struct< a: int32, b: dictionary<int8, utf8> > */
  root = abi_schema_new(c, "+s");
  abi_schema_set_name(c, root, NULL); /* absent name, not an empty one */
  root->flags = 2;                    /* ARROW_FLAG_NULLABLE */
  abi_schema_add_metadata(c, root, "origin", 6, "abi-doctor", 10);
  abi_schema_add_metadata(c, root, "binary", 6, "a\0b", 3); /* embedded NUL */

  s_int = abi_schema_new(c, "i");
  abi_schema_set_name(c, s_int, "a");
  s_int->flags = 2;

  s_dict_idx = abi_schema_new(c, "c");
  abi_schema_set_name(c, s_dict_idx, "b");
  s_dict_idx->flags = 3;
  s_dict_val = abi_schema_new(c, "u");
  abi_schema_set_name(c, s_dict_val, ""); /* empty name, not an absent one */
  abi_schema_set_dictionary(s_dict_idx, s_dict_val);

  abi_schema_add_child(c, root, s_int);
  abi_schema_add_child(c, root, s_dict_idx);
  abi_case_set_schema(c, root);

  /* array mirroring the schema */
  a_root = abi_array_new(c);
  a_root->length = 8;
  a_root->null_count = 0;
  a_root->offset = 0;
  abi_array_add_null_buffer(c, a_root, ABI_ROLE_VALIDITY);

  a_int = abi_array_new(c);
  a_int->length = 8;
  a_int->null_count = -1; /* not computed */
  a_int->offset = 0;
  abi_array_add_buffer(c, a_int, ABI_ROLE_VALIDITY, al_validity, 0, 16);
  abi_array_add_buffer(c, a_int, ABI_ROLE_DATA, al_data, 0, 32);

  a_dict_idx = abi_array_new(c);
  a_dict_idx->length = 8;
  a_dict_idx->null_count = -1;
  a_dict_idx->offset = 1; /* non-zero offset */
  /* exact alias of a_int's validity buffer: same allocation, same offset */
  abi_array_add_buffer(c, a_dict_idx, ABI_ROLE_VALIDITY, al_validity, 0, 16);
  /* deliberately misaligned view into a 64-byte-aligned allocation */
  abi_array_add_buffer(c, a_dict_idx, ABI_ROLE_DATA, al_runs, 1, 15);

  a_dict_val = abi_array_new(c);
  a_dict_val->length = 2;
  a_dict_val->null_count = 0;
  a_dict_val->offset = 0;
  abi_array_add_null_buffer(c, a_dict_val, ABI_ROLE_VALIDITY);
  abi_array_add_buffer(c, a_dict_val, ABI_ROLE_OFFSETS, al_offsets, 0, 12);
  abi_array_add_buffer(c, a_dict_val, ABI_ROLE_DATA, al_text, 0, 8);
  abi_array_set_dictionary(a_dict_idx, a_dict_val);

  abi_array_add_child(c, a_root, a_int);
  abi_array_add_child(c, a_root, a_dict_idx);
  abi_case_set_array(c, a_root);

  abi_case_add_op(c, ABI_OP_IMPORT_SCHEMA, 0, 0);
  abi_case_add_op(c, ABI_OP_IMPORT_ARRAY, 0, 0);
  abi_case_add_op(c, ABI_OP_RELEASE_BASE, 0, 0);
  return c;
}

AbiCase *abi_fixture_minimal(void) {
  AbiCase *c = abi_case_new(ABI_CLASS_A);
  if (!c) return NULL;
  abi_case_set_expected(c, ABI_VALIDATE_MINIMAL, ABI_EXPECT_ACCEPT,
                        "C Data Interface: format strings", "schema only");
  abi_case_set_schema(c, abi_schema_new(c, "i"));
  return c;
}

AbiCase *abi_fixture_smoke(void) {
  AbiCase       *c = abi_case_new(ABI_CLASS_A);
  AbiSchemaNode *root, *s_int, *s_str;
  AbiArrayNode  *a_root, *a_int, *a_str;
  uint32_t       al_validity = 0, al_ints = 0, al_offsets = 0, al_chars = 0;
  /* int32 [10, null, 30, 40], little-endian */
  static const unsigned char ints[16] = {10, 0, 0, 0, 0, 0, 0, 0,
                                         30, 0, 0, 0, 40, 0, 0, 0};
  /* validity: rows 0,2,3 valid, row 1 null  -> 0b00001101 */
  static const unsigned char validity[1] = {0x0D};
  /* offsets [0,3,3,6,9] over "foobarbaz" -> "foo", "", "bar", "baz" */
  static const unsigned char offsets[20] = {0, 0, 0, 0, 3, 0, 0, 0, 3, 0,
                                            0, 0, 6, 0, 0, 0, 9, 0, 0, 0};
  static const char chars[9] = {'f', 'o', 'o', 'b', 'a', 'r', 'b', 'a', 'z'};

  if (!c) return NULL;

  abi_case_set_provenance(c, 0, "", 0, "handwritten", ABI_DOCTOR_VERSION,
                          "arrow-c-data-2026-03");
  abi_case_set_expected(c, ABI_VALIDATE_FULL, ABI_EXPECT_ACCEPT,
                        "C Data Interface: Structure definitions",
                        "smoke fixture: plainly valid struct<int32, utf8>");

  abi_case_add_allocation(c, validity, sizeof(validity), 8, &al_validity);
  abi_case_add_allocation(c, ints, sizeof(ints), 8, &al_ints);
  abi_case_add_allocation(c, offsets, sizeof(offsets), 8, &al_offsets);
  abi_case_add_allocation(c, chars, sizeof(chars), 8, &al_chars);

  root = abi_schema_new(c, "+s");
  abi_schema_set_name(c, root, "");
  s_int = abi_schema_new(c, "i");
  abi_schema_set_name(c, s_int, "a");
  s_int->flags = 2; /* ARROW_FLAG_NULLABLE */
  s_str = abi_schema_new(c, "u");
  abi_schema_set_name(c, s_str, "b");
  s_str->flags = 2;
  abi_schema_add_child(c, root, s_int);
  abi_schema_add_child(c, root, s_str);
  abi_case_set_schema(c, root);

  a_root = abi_array_new(c);
  a_root->length = 4;
  a_root->null_count = 0;
  abi_array_add_null_buffer(c, a_root, ABI_ROLE_VALIDITY);

  a_int = abi_array_new(c);
  a_int->length = 4;
  a_int->null_count = 1;
  abi_array_add_buffer(c, a_int, ABI_ROLE_VALIDITY, al_validity, 0, 1);
  abi_array_add_buffer(c, a_int, ABI_ROLE_DATA, al_ints, 0, 16);

  a_str = abi_array_new(c);
  a_str->length = 4;
  a_str->null_count = 0;
  abi_array_add_null_buffer(c, a_str, ABI_ROLE_VALIDITY);
  abi_array_add_buffer(c, a_str, ABI_ROLE_OFFSETS, al_offsets, 0, 20);
  abi_array_add_buffer(c, a_str, ABI_ROLE_DATA, al_chars, 0, 9);

  abi_array_add_child(c, a_root, a_int);
  abi_array_add_child(c, a_root, a_str);
  abi_case_set_array(c, a_root);
  return c;
}

AbiCase *abi_fixture_bad_dict_index(void) {
  AbiCase       *c = abi_case_new(ABI_CLASS_B1);
  AbiSchemaNode *root, *s_idx, *s_val;
  AbiArrayNode  *a_root, *a_idx, *a_val;
  uint32_t       al_idx = 0, al_offsets = 0, al_chars = 0;
  /* int8 indices; 99 is out of range for a 2-value dictionary */
  static const unsigned char indices[4] = {0, 1, 99, 0};
  /* offsets [0,2,4] over "aabb" -> "aa", "bb" */
  static const unsigned char offsets[12] = {0, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0};
  static const char chars[4] = {'a', 'a', 'b', 'b'};

  if (!c) return NULL;

  abi_case_set_provenance(c, 0, "", 0, "handwritten", ABI_DOCTOR_VERSION,
                          "arrow-c-data-2026-03");
  abi_case_set_expected(
      c, ABI_VALIDATE_FULL, ABI_EXPECT_REJECT,
      "Columnar format: Dictionary-encoded layout",
      "index 99 is out of range for a 2-value dictionary; a conforming "
      "consumer must refuse cleanly rather than read out of bounds");

  abi_case_add_allocation(c, indices, sizeof(indices), 8, &al_idx);
  abi_case_add_allocation(c, offsets, sizeof(offsets), 8, &al_offsets);
  abi_case_add_allocation(c, chars, sizeof(chars), 8, &al_chars);

  root = abi_schema_new(c, "+s");
  abi_schema_set_name(c, root, "");
  s_idx = abi_schema_new(c, "c");
  abi_schema_set_name(c, s_idx, "d");
  s_val = abi_schema_new(c, "u");
  abi_schema_set_name(c, s_val, "");
  abi_schema_set_dictionary(s_idx, s_val);
  abi_schema_add_child(c, root, s_idx);
  abi_case_set_schema(c, root);

  a_root = abi_array_new(c);
  a_root->length = 4;
  a_root->null_count = 0;
  abi_array_add_null_buffer(c, a_root, ABI_ROLE_VALIDITY);

  a_idx = abi_array_new(c);
  a_idx->length = 4;
  a_idx->null_count = 0;
  abi_array_add_null_buffer(c, a_idx, ABI_ROLE_VALIDITY);
  abi_array_add_buffer(c, a_idx, ABI_ROLE_DATA, al_idx, 0, 4);

  a_val = abi_array_new(c);
  a_val->length = 2;
  a_val->null_count = 0;
  abi_array_add_null_buffer(c, a_val, ABI_ROLE_VALIDITY);
  abi_array_add_buffer(c, a_val, ABI_ROLE_OFFSETS, al_offsets, 0, 12);
  abi_array_add_buffer(c, a_val, ABI_ROLE_DATA, al_chars, 0, 4);
  abi_array_set_dictionary(a_idx, a_val);

  abi_array_add_child(c, a_root, a_idx);
  abi_case_set_array(c, a_root);
  return c;
}
