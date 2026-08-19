/*
 * abicase -- inspect and verify .abicase files.
 *
 * M0 has no adapters and no observer, so this is the only executable a person
 * runs. Its `dump` output is deliberately plain, fully determined by the file,
 * and free of anything host-specific: two architectures decoding the same file
 * must print the same bytes, which is how the cross-architecture requirement is
 * checked rather than asserted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi/abicase.h"
#include "fixture.h"

static int fail(const char *what, AbiStatus st, const AbiError *err) {
  fprintf(stderr, "error: %s: %s\n", what, abi_status_str(st));
  if (err && err->message[0]) {
    fprintf(stderr, "       %s (at byte %llu)\n", err->message,
            (unsigned long long)err->offset);
  }
  return 1;
}

static const char *class_name(AbiClass c) {
  switch (c) {
    case ABI_CLASS_A:  return "A";
    case ABI_CLASS_B1: return "B1";
    case ABI_CLASS_B2: return "B2";
    case ABI_CLASS_C:  return "C";
  }
  return "?";
}

static const char *role_name(uint8_t r) {
  static const char *names[] = {"unspecified", "validity", "data",
                                "offsets",     "type_ids", "union_offsets",
                                "sizes",       "views",    "variadic_data"};
  return (r <= ABI_ROLE__MAX) ? names[r] : "?";
}

static const char *fill_name(AbiFill f) {
  switch (f) {
    case ABI_FILL_RAW:     return "RAW";
    case ABI_FILL_ZERO:    return "ZERO";
    case ABI_FILL_RLE:     return "RLE";
    case ABI_FILL_PATTERN: return "PATTERN";
  }
  return "?";
}

static const char *outcome_name(uint8_t o) {
  static const char *names[] = {"ACCEPT", "REJECT", "EITHER", "UNSPECIFIED"};
  return (o <= ABI_EXPECT_UNSPECIFIED) ? names[o] : "?";
}

static const char *level_name(uint8_t l) {
  static const char *names[] = {"none", "minimal", "default", "full"};
  return (l <= ABI_VALIDATE_FULL) ? names[l] : "?";
}

/* Bytes may hold arbitrary values; print them unambiguously and portably. */
static void print_bytes(const AbiBytes *b) {
  uint32_t i;
  putchar('"');
  for (i = 0; i < b->size; i++) {
    unsigned char ch = b->data[i];
    if (ch == '"' || ch == '\\') {
      printf("\\%c", ch);
    } else if (ch >= 0x20 && ch < 0x7F) {
      putchar((int)ch);
    } else {
      printf("\\x%02x", (unsigned)ch);
    }
  }
  putchar('"');
}

static void print_schema(const AbiSchemaNode *n, const char *path, int indent) {
  uint32_t i;
  char     child_path[256];

  printf("%*s%s format=", indent, "", path);
  print_bytes(&n->format);
  if (n->has_name) {
    printf(" name=");
    print_bytes(&n->name);
  } else {
    printf(" name=<absent>");
  }
  printf(" flags=0x%llx n_children=%lu provided=%lu\n",
         (unsigned long long)n->flags, (unsigned long)n->n_children,
         (unsigned long)n->child_count);

  if (n->has_metadata) {
    printf("%*s  metadata: %lu pair(s)\n", indent, "",
           (unsigned long)n->metadata_count);
    for (i = 0; i < n->metadata_count; i++) {
      printf("%*s    ", indent, "");
      print_bytes(&n->metadata[i].key);
      printf(" = ");
      print_bytes(&n->metadata[i].value);
      putchar('\n');
    }
  }
  for (i = 0; i < n->child_count; i++) {
    snprintf(child_path, sizeof(child_path), "%s%lu/", path, (unsigned long)i);
    print_schema(n->children[i], child_path, indent + 2);
  }
  if (n->dictionary) {
    snprintf(child_path, sizeof(child_path), "%sdict/", path);
    print_schema(n->dictionary, child_path, indent + 2);
  }
}

static void print_array(const AbiArrayNode *n, const char *path, int indent) {
  uint32_t i;
  char     child_path[256];

  printf("%*s%s length=%lld null_count=%lld offset=%lld n_buffers=%lu "
         "provided=%lu n_children=%lu provided=%lu\n",
         indent, "", path, (long long)n->length, (long long)n->null_count,
         (long long)n->offset, (unsigned long)n->n_buffers,
         (unsigned long)n->buffer_count, (unsigned long)n->n_children,
         (unsigned long)n->child_count);

  for (i = 0; i < n->buffer_count; i++) {
    const AbiBufferView *v = &n->buffers[i];
    if (!v->present) {
      printf("%*s  buffer[%lu] role=%-10s NULL\n", indent, "", (unsigned long)i,
             role_name(v->role));
    } else {
      printf("%*s  buffer[%lu] role=%-10s alloc=%lu offset=%llu len=%llu\n",
             indent, "", (unsigned long)i, role_name(v->role),
             (unsigned long)v->allocation_id,
             (unsigned long long)v->byte_offset,
             (unsigned long long)v->logical_length);
    }
  }
  for (i = 0; i < n->child_count; i++) {
    snprintf(child_path, sizeof(child_path), "%s%lu/", path, (unsigned long)i);
    print_array(n->children[i], child_path, indent + 2);
  }
  if (n->dictionary) {
    snprintf(child_path, sizeof(child_path), "%sdict/", path);
    print_array(n->dictionary, child_path, indent + 2);
  }
}

/* Counts how many views reference one allocation, for the topology report. */
static void count_refs(const AbiArrayNode *n, uint32_t alloc_id, const char *path,
                       int *count) {
  uint32_t i;
  char     child_path[256];

  for (i = 0; i < n->buffer_count; i++) {
    const AbiBufferView *v = &n->buffers[i];
    if (v->present && v->allocation_id == alloc_id) {
      if (*count == 0) printf("    referenced by:");
      printf(" %s[%lu]@%llu", path, (unsigned long)i,
             (unsigned long long)v->byte_offset);
      (*count)++;
    }
  }
  for (i = 0; i < n->child_count; i++) {
    snprintf(child_path, sizeof(child_path), "%s%lu/", path, (unsigned long)i);
    count_refs(n->children[i], alloc_id, child_path, count);
  }
  if (n->dictionary) {
    snprintf(child_path, sizeof(child_path), "%sdict/", path);
    count_refs(n->dictionary, alloc_id, child_path, count);
  }
}

static int cmd_dump(const char *path) {
  AbiCase  *c = NULL;
  AbiError  err;
  AbiStatus st;
  uint32_t  i;
  char      id[ABICASE_ID_HEX_SIZE];

  memset(&err, 0, sizeof(err));
  st = abi_case_read_file(path, &c, &err);
  if (st != ABI_OK) return fail(path, st, &err);

  if (abi_case_id(c, id) == ABI_OK) printf("case_id: %s\n", id);
  printf("class:   %s\n", class_name(c->cls));

  printf("\nprovenance:\n");
  printf("  seed:               0x%016llx\n",
         (unsigned long long)c->provenance.seed);
  printf("  rng:                ");
  print_bytes(&c->provenance.rng_algorithm);
  printf(" v%lu\n", (unsigned long)c->provenance.rng_version);
  printf("  generator_version:  ");
  print_bytes(&c->provenance.generator_version);
  printf("\n  abi_doctor_version: ");
  print_bytes(&c->provenance.abi_doctor_version);
  printf("\n  spec_revision:      ");
  print_bytes(&c->provenance.spec_revision);
  putchar('\n');

  printf("\nallocations: %lu\n", (unsigned long)c->alloc_count);
  for (i = 0; i < c->alloc_count; i++) {
    const AbiAllocation *a = &c->allocations[i];
    uint64_t             payload = 0;
    uint32_t             period = 0, runs = 0;
    AbiFill f = abi_fill_choose(a->bytes, a->size_bytes, &payload, &period, &runs);

    printf("  [%lu] size=%llu align=%lu fill=%s(payload %llu)\n",
           (unsigned long)i, (unsigned long long)a->size_bytes,
           (unsigned long)a->alignment, fill_name(f),
           (unsigned long long)payload);
    if (c->array) {
      int refs = 0;
      count_refs(c->array, i, "/", &refs);
      if (refs > 0) {
        /* the topological fact the allocation/view model exists to preserve */
        if (refs > 1) printf("   <- ALIASED by %d views", refs);
        putchar('\n');
      } else {
        printf("    referenced by: (nothing)\n");
      }
    }
  }

  printf("\nschema:\n");
  print_schema(c->schema, "/", 2);

  if (c->array) {
    printf("\narray:\n");
    print_array(c->array, "/", 2);
  } else {
    printf("\narray: (absent -- schema-only case)\n");
  }

  if (c->op_count) {
    printf("\ncall_sequence: %lu op(s)\n", (unsigned long)c->op_count);
    for (i = 0; i < c->op_count; i++) {
      printf("  [%lu] code=%u arg0=%lu arg1=%lu\n", (unsigned long)i,
             (unsigned)c->ops[i].code, (unsigned long)c->ops[i].arg0,
             (unsigned long)c->ops[i].arg1);
    }
  } else {
    printf("\ncall_sequence: (absent)\n");
  }

  printf("\nexpected:\n");
  printf("  validation_level: %s\n", level_name(c->expected.validation_level));
  printf("  outcome:          %s\n", outcome_name(c->expected.expected_outcome));
  printf("  spec_clause:      ");
  print_bytes(&c->expected.spec_clause);
  printf("\n  notes:            ");
  print_bytes(&c->expected.notes);
  putchar('\n');

  abi_case_free(c);
  return 0;
}

/*
 * Decode, re-encode, compare. This is the M0 round-trip guarantee applied to an
 * arbitrary file rather than to a fixture, and it is what the cross-
 * architecture check runs on the other host's output.
 */
static int cmd_verify(const char *path) {
  uint8_t  *orig = NULL, *again = NULL;
  size_t    n_orig = 0, n_again = 0;
  AbiCase  *c = NULL;
  AbiError  err;
  AbiStatus st;
  char      id[ABICASE_ID_HEX_SIZE];
  int       rc = 0;

  memset(&err, 0, sizeof(err));
  st = abi_read_file(path, &orig, &n_orig);
  if (st != ABI_OK) return fail(path, st, NULL);

  st = abi_case_decode(orig, n_orig, &c, &err);
  if (st != ABI_OK) {
    free(orig);
    return fail(path, st, &err);
  }

  st = abi_case_encode(c, &again, &n_again);
  if (st != ABI_OK) {
    abi_case_free(c);
    free(orig);
    return fail("re-encode", st, NULL);
  }

  abi_case_id(c, id);
  if (n_orig != n_again || memcmp(orig, again, n_orig) != 0) {
    printf("FAIL %s: re-encode differs (%llu -> %llu bytes)\n", path,
           (unsigned long long)n_orig, (unsigned long long)n_again);
    rc = 1;
  } else {
    printf("ok   %s: %llu bytes, class %s, case_id %s\n", path,
           (unsigned long long)n_orig, class_name(c->cls), id);
  }

  abi_free(again);
  abi_case_free(c);
  free(orig);
  return rc;
}

static int cmd_id(const char *path) {
  uint8_t  *data = NULL;
  size_t    n = 0;
  char      id[ABICASE_ID_HEX_SIZE];
  AbiStatus st = abi_read_file(path, &data, &n);
  if (st != ABI_OK) return fail(path, st, NULL);
  st = abi_case_id_of_bytes(data, n, id);
  free(data);
  if (st != ABI_OK) return fail(path, st, NULL);
  printf("%s\n", id);
  return 0;
}

/*
 * Emits the shared fixture. Run on two architectures, the resulting files must
 * be byte-identical: that is the cross-architecture property, measured.
 */
static int cmd_selftest(const char *out_path) {
  AbiCase  *c = abi_fixture_rich();
  uint8_t  *data = NULL;
  size_t    n = 0;
  char      id[ABICASE_ID_HEX_SIZE];
  AbiStatus st;

  if (!c) return fail("fixture", ABI_ERR_NO_MEMORY, NULL);
  st = abi_case_encode(c, &data, &n);
  if (st != ABI_OK) {
    abi_case_free(c);
    return fail("encode", st, NULL);
  }
  abi_case_id_of_bytes(data, n, id);
  printf("fixture:  rich\n");
  printf("size:     %llu\n", (unsigned long long)n);
  printf("case_id:  %s\n", id);

  if (out_path) {
    st = abi_case_write_file(c, out_path);
    if (st != ABI_OK) {
      abi_free(data);
      abi_case_free(c);
      return fail(out_path, st, NULL);
    }
    printf("written:  %s\n", out_path);
  }
  abi_free(data);
  abi_case_free(c);
  return 0;
}

static int usage(void) {
  fprintf(stderr,
          "abicase " ABI_DOCTOR_VERSION "\n"
          "\n"
          "usage:\n"
          "  abicase dump      <file>       structure and topology, host-independent\n"
          "  abicase verify    <file>       decode, re-encode, require byte equality\n"
          "  abicase id        <file>       canonical case id\n"
          "  abicase selftest  [-o <file>]  emit the shared fixture\n");
  return 2;
}

int main(int argc, char **argv) {
  if (argc < 2) return usage();

  if (strcmp(argv[1], "dump") == 0 && argc == 3) return cmd_dump(argv[2]);
  if (strcmp(argv[1], "verify") == 0 && argc == 3) return cmd_verify(argv[2]);
  if (strcmp(argv[1], "id") == 0 && argc == 3) return cmd_id(argv[2]);
  if (strcmp(argv[1], "selftest") == 0) {
    if (argc == 2) return cmd_selftest(NULL);
    if (argc == 4 && strcmp(argv[2], "-o") == 0) return cmd_selftest(argv[3]);
  }
  return usage();
}
