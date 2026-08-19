/*
 * Canonical encoder -- docs/abicase-format.md.
 *
 * Every section is built into its own buffer so its size is known before its
 * header is written, then appended in ascending tag order. The header is
 * assembled last, because it carries the payload digest.
 */
#include <stdlib.h>

#include "abi_internal.h"

static int enc_section(AbiBuf *payload, uint16_t tag, const AbiBuf *body,
                       uint32_t *section_count) {
  if (body->failed) return 0;
  if (body->size > 0xFFFFFFFFu) return 0;
  if (!abi_buf_u16(payload, tag)) return 0;
  if (!abi_buf_u16(payload, (uint16_t)ABICASE_SECTION_VERSION)) return 0;
  if (!abi_buf_u32(payload, (uint32_t)body->size)) return 0;
  if (!abi_buf_put(payload, body->data, body->size)) return 0;
  (*section_count)++;
  return 1;
}

static int enc_provenance(AbiBuf *b, const AbiCase *c) {
  const AbiProvenance *p = &c->provenance;
  return abi_buf_u64(b, p->seed) && abi_buf_bytes(b, p->rng_algorithm) &&
         abi_buf_u32(b, p->rng_version) &&
         abi_buf_bytes(b, p->generator_version) &&
         abi_buf_bytes(b, p->abi_doctor_version) &&
         abi_buf_bytes(b, p->spec_revision);
}

static int enc_allocations(AbiBuf *b, const AbiCase *c) {
  uint32_t i;

  if (!abi_buf_u32(b, c->alloc_count)) return 0;
  for (i = 0; i < c->alloc_count; i++) {
    const AbiAllocation *a = &c->allocations[i];
    uint64_t             payload_size = 0;
    uint32_t             period = 0, run_count = 0;
    AbiFill fill = abi_fill_choose(a->bytes, a->size_bytes, &payload_size,
                                   &period, &run_count);

    if (!abi_buf_u64(b, a->size_bytes)) return 0;
    if (!abi_buf_u32(b, a->alignment)) return 0;
    if (!abi_buf_u8(b, (uint8_t)fill)) return 0;
    if (!abi_buf_zeros(b, 3)) return 0;
    if (!abi_fill_write(b, a->bytes, a->size_bytes, fill, period, run_count)) {
      return 0;
    }
  }
  return 1;
}

static int enc_schema_node(AbiBuf *b, const AbiSchemaNode *n) {
  uint8_t  presence = 0;
  uint32_t i;

  if (!abi_buf_bytes(b, n->format)) return 0;
  if (n->has_name) presence |= 1u;
  if (n->has_metadata) presence |= 2u;
  if (!abi_buf_u8(b, presence)) return 0;
  if (n->has_name && !abi_buf_bytes(b, n->name)) return 0;
  if (n->has_metadata) {
    if (!abi_buf_u32(b, n->metadata_count)) return 0;
    for (i = 0; i < n->metadata_count; i++) {
      if (!abi_buf_bytes(b, n->metadata[i].key)) return 0;
      if (!abi_buf_bytes(b, n->metadata[i].value)) return 0;
    }
  }
  if (!abi_buf_i64(b, n->flags)) return 0;
  if (!abi_buf_u32(b, n->n_children)) return 0;
  if (!abi_buf_u32(b, n->child_count)) return 0;
  for (i = 0; i < n->child_count; i++) {
    if (!enc_schema_node(b, n->children[i])) return 0;
  }
  if (!abi_buf_u8(b, n->dictionary ? 1u : 0u)) return 0;
  if (n->dictionary && !enc_schema_node(b, n->dictionary)) return 0;
  return 1;
}

static int enc_array_node(AbiBuf *b, const AbiArrayNode *n) {
  uint32_t i;

  if (!abi_buf_i64(b, n->length)) return 0;
  if (!abi_buf_i64(b, n->null_count)) return 0;
  if (!abi_buf_i64(b, n->offset)) return 0;
  if (!abi_buf_u32(b, n->n_buffers)) return 0;
  if (!abi_buf_u32(b, n->buffer_count)) return 0;
  for (i = 0; i < n->buffer_count; i++) {
    const AbiBufferView *v = &n->buffers[i];
    if (!abi_buf_u8(b, v->role)) return 0;
    if (!abi_buf_u8(b, v->present)) return 0;
    if (!abi_buf_zeros(b, 2)) return 0;
    if (v->present) {
      if (!abi_buf_u32(b, v->allocation_id)) return 0;
      if (!abi_buf_u64(b, v->byte_offset)) return 0;
      if (!abi_buf_u64(b, v->logical_length)) return 0;
    }
  }
  if (!abi_buf_u32(b, n->n_children)) return 0;
  if (!abi_buf_u32(b, n->child_count)) return 0;
  for (i = 0; i < n->child_count; i++) {
    if (!enc_array_node(b, n->children[i])) return 0;
  }
  if (!abi_buf_u8(b, n->dictionary ? 1u : 0u)) return 0;
  if (n->dictionary && !enc_array_node(b, n->dictionary)) return 0;
  return 1;
}

static int enc_callseq(AbiBuf *b, const AbiCase *c) {
  uint32_t i;
  if (!abi_buf_u32(b, c->op_count)) return 0;
  for (i = 0; i < c->op_count; i++) {
    if (!abi_buf_u16(b, c->ops[i].code)) return 0;
    if (!abi_buf_u32(b, c->ops[i].arg0)) return 0;
    if (!abi_buf_u32(b, c->ops[i].arg1)) return 0;
  }
  return 1;
}

static int enc_expected(AbiBuf *b, const AbiCase *c) {
  return abi_buf_u8(b, c->expected.validation_level) &&
         abi_buf_u8(b, c->expected.expected_outcome) && abi_buf_zeros(b, 2) &&
         abi_buf_bytes(b, c->expected.spec_clause) &&
         abi_buf_bytes(b, c->expected.notes);
}

AbiStatus abi_case_encode(const AbiCase *c, uint8_t **out_data,
                          size_t *out_size) {
  AbiBuf    payload, body;
  AbiStatus st;
  uint32_t  section_count = 0;
  size_t    total;
  uint8_t  *out = NULL;
  uint8_t   digest[32];
  int       ok = 1;

  if (!c || !out_data || !out_size) return ABI_ERR_INVALID_ARGUMENT;

  /*
   * Validate before emitting. The encoder must never produce a file its own
   * decoder rejects, and the decoder runs these same rules on the way back in.
   */
  st = abi_case_validate(c, NULL);
  if (st != ABI_OK) return st;

  abi_buf_init(&payload);
  abi_buf_init(&body);

#define ABI_EMIT(tag, expr)                                    \
  do {                                                         \
    body.size = 0;                                             \
    body.failed = 0;                                           \
    ok = ok && (expr);                                         \
    ok = ok && enc_section(&payload, (tag), &body, &section_count); \
  } while (0)

  ABI_EMIT(ABICASE_TAG_PROVENANCE, enc_provenance(&body, c));
  ABI_EMIT(ABICASE_TAG_ALLOCATIONS, enc_allocations(&body, c));
  ABI_EMIT(ABICASE_TAG_SCHEMA, enc_schema_node(&body, c->schema));
  if (c->array) ABI_EMIT(ABICASE_TAG_ARRAY, enc_array_node(&body, c->array));
  /* An empty CALLSEQ section is not canonical: omit it instead. */
  if (c->op_count) ABI_EMIT(ABICASE_TAG_CALLSEQ, enc_callseq(&body, c));
  ABI_EMIT(ABICASE_TAG_EXPECTED, enc_expected(&body, c));

#undef ABI_EMIT

  abi_buf_reset(&body);
  if (!ok || payload.failed) {
    abi_buf_reset(&payload);
    return ABI_ERR_NO_MEMORY;
  }

  total = (size_t)ABICASE_HEADER_SIZE + payload.size;
  if (total > ABI_LIMIT_TOTAL_SIZE) {
    abi_buf_reset(&payload);
    return ABI_ERR_LIMIT_EXCEEDED;
  }

  out = (uint8_t *)malloc(total);
  if (!out) {
    abi_buf_reset(&payload);
    return ABI_ERR_NO_MEMORY;
  }

  out[0] = (uint8_t)ABICASE_MAGIC0;
  out[1] = (uint8_t)ABICASE_MAGIC1;
  out[2] = (uint8_t)ABICASE_MAGIC2;
  out[3] = (uint8_t)ABICASE_MAGIC3;
  abi_store_u16(out + 4, (uint16_t)ABICASE_SCHEMA_VERSION);
  abi_store_u16(out + 6, (uint16_t)ABICASE_HEADER_SIZE);
  abi_store_u32(out + 8, ABICASE_BOM);
  out[12] = (uint8_t)c->cls;
  out[13] = 0;
  abi_store_u16(out + 14, 0);
  abi_store_u32(out + 16, section_count);
  abi_store_u32(out + 20, (uint32_t)total);

  if (payload.size) memcpy(out + ABICASE_HEADER_SIZE, payload.data, payload.size);
  abi_sha256(out + ABICASE_HEADER_SIZE, payload.size, digest);
  memcpy(out + 24, digest, ABICASE_ID_BYTES);

  abi_buf_reset(&payload);
  *out_data = out;
  *out_size = total;
  return ABI_OK;
}

AbiStatus abi_case_id(const AbiCase *c, char *out_hex) {
  uint8_t  *data = NULL;
  size_t    size = 0;
  AbiStatus st;

  if (!c || !out_hex) return ABI_ERR_INVALID_ARGUMENT;
  /*
   * Derived by encoding rather than by hashing the model directly: the identity
   * of a case is the identity of its canonical bytes, and computing it a second
   * way would be a second definition to keep in sync.
   */
  st = abi_case_encode(c, &data, &size);
  if (st != ABI_OK) return st;
  abi_hex(data + 24, ABICASE_ID_BYTES, out_hex);
  free(data);
  return ABI_OK;
}

AbiStatus abi_case_id_of_bytes(const uint8_t *data, size_t size, char *out_hex) {
  if (!data || !out_hex) return ABI_ERR_INVALID_ARGUMENT;
  if (size < ABICASE_HEADER_SIZE) return ABI_ERR_TRUNCATED;
  if (data[0] != ABICASE_MAGIC0 || data[1] != ABICASE_MAGIC1 ||
      data[2] != ABICASE_MAGIC2 || data[3] != ABICASE_MAGIC3) {
    return ABI_ERR_BAD_MAGIC;
  }
  abi_hex(data + 24, ABICASE_ID_BYTES, out_hex);
  return ABI_OK;
}
