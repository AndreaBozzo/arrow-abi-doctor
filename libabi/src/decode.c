/*
 * Strict canonical decoder -- docs/abicase-format.md.
 *
 * Rejects anything a conforming encoder would not have produced: non-ascending
 * or duplicated sections, non-zero reserved bytes, non-minimal allocation
 * payloads, trailing bytes inside a section or after the last one. That
 * strictness is what makes encode(decode(b)) == b hold byte for byte, and it is
 * cheaper than trying to normalize on the way in.
 *
 * Nothing here trusts the input. Every count is checked against both a fixed
 * limit and the bytes actually remaining before anything is allocated for it,
 * so a corrupt file fails instead of asking for memory it will never fill.
 */
#include <stdlib.h>

#include "abi_internal.h"

typedef struct {
  AbiCur   cur;
  AbiCase *c;
  uint32_t nodes;   /* budget shared across one tree */
  uint32_t buffers; /* budget shared across all buffer views */
} AbiDec;

/*
 * A count is only credible if the bytes to satisfy it are present. Checking
 * before allocating turns a memory bomb into a clean truncation error.
 */
static int dec_count_guard(AbiDec *d, uint64_t count, uint64_t min_bytes_each,
                           uint64_t limit, const char *what) {
  size_t remaining;
  if (d->cur.failed) return 0;
  if (count > limit) {
    return abi_cur_fail(&d->cur, ABI_ERR_LIMIT_EXCEEDED, d->cur.pos,
                        "%s count %llu exceeds limit %llu", what,
                        (unsigned long long)count, (unsigned long long)limit);
  }
  remaining = d->cur.size - d->cur.pos;
  if (min_bytes_each && count > (uint64_t)remaining / min_bytes_each) {
    return abi_cur_fail(&d->cur, ABI_ERR_TRUNCATED, d->cur.pos,
                        "%s count %llu needs at least %llu bytes, %llu remain",
                        what, (unsigned long long)count,
                        (unsigned long long)(count * min_bytes_each),
                        (unsigned long long)remaining);
  }
  return 1;
}

/* --- sections ------------------------------------------------------------ */

static int dec_provenance(AbiDec *d) {
  AbiProvenance *p = &d->c->provenance;
  p->seed = abi_cur_u64(&d->cur);
  if (!abi_cur_bytes(&d->cur, d->c->arena, &p->rng_algorithm)) return 0;
  p->rng_version = abi_cur_u32(&d->cur);
  if (!abi_cur_bytes(&d->cur, d->c->arena, &p->generator_version)) return 0;
  if (!abi_cur_bytes(&d->cur, d->c->arena, &p->abi_doctor_version)) return 0;
  if (!abi_cur_bytes(&d->cur, d->c->arena, &p->spec_revision)) return 0;
  return !d->cur.failed;
}

static int dec_allocations(AbiDec *d) {
  uint32_t count, i;

  count = abi_cur_u32(&d->cur);
  if (d->cur.failed) return 0;
  /* 8 size + 4 alignment + 1 fill + 3 reserved = 16 bytes minimum per record */
  if (!dec_count_guard(d, count, 16, ABI_LIMIT_ALLOC_COUNT, "allocation")) {
    return 0;
  }

  d->c->allocations =
      ABI_ARENA_ARRAY(d->c->arena, AbiAllocation, count ? count : 1);
  if (!d->c->allocations) {
    return abi_cur_fail(&d->cur, ABI_ERR_NO_MEMORY, d->cur.pos,
                        "out of memory");
  }
  d->c->alloc_capacity = count;

  for (i = 0; i < count; i++) {
    AbiAllocation *a = &d->c->allocations[i];
    size_t         at;
    uint8_t        fill;

    at = d->cur.pos;
    a->size_bytes = abi_cur_u64(&d->cur);
    if (d->cur.failed) return 0;
    if (a->size_bytes > ABI_LIMIT_ALLOC_BYTES) {
      return abi_cur_fail(&d->cur, ABI_ERR_LIMIT_EXCEEDED, at,
                          "allocation %lu of %llu bytes exceeds limit %u",
                          (unsigned long)i, (unsigned long long)a->size_bytes,
                          (unsigned)ABI_LIMIT_ALLOC_BYTES);
    }

    at = d->cur.pos;
    a->alignment = abi_cur_u32(&d->cur);
    if (d->cur.failed) return 0;
    if (a->alignment == 0 || (a->alignment & (a->alignment - 1)) != 0 ||
        a->alignment > ABI_LIMIT_ALIGNMENT) {
      return abi_cur_fail(&d->cur, ABI_ERR_INVALID_ARGUMENT, at,
                          "allocation %lu alignment %lu must be a power of two "
                          "in [1,%u]",
                          (unsigned long)i, (unsigned long)a->alignment,
                          (unsigned)ABI_LIMIT_ALIGNMENT);
    }

    fill = abi_cur_u8(&d->cur);
    if (!abi_cur_zeros(&d->cur, 3, "allocation.reserved")) return 0;
    if (!abi_fill_read(&d->cur, d->c->arena, a->size_bytes, fill, &a->bytes)) {
      return 0;
    }
    d->c->alloc_count = i + 1;
  }
  d->c->alloc_count = count;
  return !d->cur.failed;
}

static AbiSchemaNode *dec_schema_node(AbiDec *d, uint32_t depth) {
  AbiSchemaNode *n;
  uint8_t        presence, has_dict;
  uint32_t       i;
  size_t         at;

  if (d->cur.failed) return NULL;
  if (depth > ABI_LIMIT_TREE_DEPTH) {
    abi_cur_fail(&d->cur, ABI_ERR_LIMIT_EXCEEDED, d->cur.pos,
                 "schema depth exceeds %u", (unsigned)ABI_LIMIT_TREE_DEPTH);
    return NULL;
  }
  if (++d->nodes > ABI_LIMIT_TREE_NODES) {
    abi_cur_fail(&d->cur, ABI_ERR_LIMIT_EXCEEDED, d->cur.pos,
                 "schema node count exceeds %u",
                 (unsigned)ABI_LIMIT_TREE_NODES);
    return NULL;
  }

  n = ABI_ARENA_NEW(d->c->arena, AbiSchemaNode);
  if (!n) {
    abi_cur_fail(&d->cur, ABI_ERR_NO_MEMORY, d->cur.pos, "out of memory");
    return NULL;
  }
  if (!abi_cur_bytes(&d->cur, d->c->arena, &n->format)) return NULL;

  at = d->cur.pos;
  presence = abi_cur_u8(&d->cur);
  if (d->cur.failed) return NULL;
  if (presence & ~0x03u) {
    abi_cur_fail(&d->cur, ABI_ERR_NOT_CANONICAL, at,
                 "schema presence bits 0x%02x has undefined bits set",
                 (unsigned)presence);
    return NULL;
  }
  n->has_name = (presence & 0x01u) ? 1 : 0;
  n->has_metadata = (presence & 0x02u) ? 1 : 0;

  if (n->has_name) {
    if (!abi_cur_bytes(&d->cur, d->c->arena, &n->name)) return NULL;
  } else if (!abi_arena_cstr(d->c->arena, &n->name, "")) {
    abi_cur_fail(&d->cur, ABI_ERR_NO_MEMORY, d->cur.pos, "out of memory");
    return NULL;
  }

  if (n->has_metadata) {
    uint32_t kv_count = abi_cur_u32(&d->cur);
    if (d->cur.failed) return NULL;
    /* each pair is at least two 4-byte length prefixes */
    if (!dec_count_guard(d, kv_count, 8, ABI_LIMIT_TREE_NODES, "metadata")) {
      return NULL;
    }
    n->metadata =
        ABI_ARENA_ARRAY(d->c->arena, AbiMetadataKV, kv_count ? kv_count : 1);
    if (!n->metadata) {
      abi_cur_fail(&d->cur, ABI_ERR_NO_MEMORY, d->cur.pos, "out of memory");
      return NULL;
    }
    n->metadata_capacity = kv_count;
    for (i = 0; i < kv_count; i++) {
      if (!abi_cur_bytes(&d->cur, d->c->arena, &n->metadata[i].key))
        return NULL;
      if (!abi_cur_bytes(&d->cur, d->c->arena, &n->metadata[i].value))
        return NULL;
    }
    n->metadata_count = kv_count;
  }

  n->flags = abi_cur_i64(&d->cur);
  n->n_children = abi_cur_u32(&d->cur);
  n->child_count = abi_cur_u32(&d->cur);
  if (d->cur.failed) return NULL;
  /* a child node is at least a length prefix, a presence byte, flags and counts
   */
  if (!dec_count_guard(d, n->child_count, 4,
                       ABI_LIMIT_TREE_NODES - d->nodes + 1, "schema child")) {
    return NULL;
  }
  if (n->child_count) {
    n->children = ABI_ARENA_ARRAY(d->c->arena, AbiSchemaNode *, n->child_count);
    if (!n->children) {
      abi_cur_fail(&d->cur, ABI_ERR_NO_MEMORY, d->cur.pos, "out of memory");
      return NULL;
    }
    n->child_capacity = n->child_count;
    for (i = 0; i < n->child_count; i++) {
      n->children[i] = dec_schema_node(d, depth + 1);
      if (!n->children[i]) return NULL;
    }
  }

  if (!abi_cur_bool(&d->cur, "schema.has_dictionary", &has_dict)) return NULL;
  if (has_dict) {
    n->dictionary = dec_schema_node(d, depth + 1);
    if (!n->dictionary) return NULL;
  }
  return n;
}

static AbiArrayNode *dec_array_node(AbiDec *d, uint32_t depth) {
  AbiArrayNode *n;
  uint8_t       has_dict;
  uint32_t      i;

  if (d->cur.failed) return NULL;
  if (depth > ABI_LIMIT_TREE_DEPTH) {
    abi_cur_fail(&d->cur, ABI_ERR_LIMIT_EXCEEDED, d->cur.pos,
                 "array depth exceeds %u", (unsigned)ABI_LIMIT_TREE_DEPTH);
    return NULL;
  }
  if (++d->nodes > ABI_LIMIT_TREE_NODES) {
    abi_cur_fail(&d->cur, ABI_ERR_LIMIT_EXCEEDED, d->cur.pos,
                 "array node count exceeds %u", (unsigned)ABI_LIMIT_TREE_NODES);
    return NULL;
  }

  n = ABI_ARENA_NEW(d->c->arena, AbiArrayNode);
  if (!n) {
    abi_cur_fail(&d->cur, ABI_ERR_NO_MEMORY, d->cur.pos, "out of memory");
    return NULL;
  }

  n->length = abi_cur_i64(&d->cur);
  n->null_count = abi_cur_i64(&d->cur);
  n->offset = abi_cur_i64(&d->cur);
  n->n_buffers = abi_cur_u32(&d->cur);
  n->buffer_count = abi_cur_u32(&d->cur);
  if (d->cur.failed) return NULL;
  /* role + present + 2 reserved is the smallest a view can be */
  if (!dec_count_guard(d, n->buffer_count, 4, ABI_LIMIT_TREE_NODES - d->buffers,
                       "buffer view")) {
    return NULL;
  }
  d->buffers += n->buffer_count;

  if (n->buffer_count) {
    n->buffers = ABI_ARENA_ARRAY(d->c->arena, AbiBufferView, n->buffer_count);
    if (!n->buffers) {
      abi_cur_fail(&d->cur, ABI_ERR_NO_MEMORY, d->cur.pos, "out of memory");
      return NULL;
    }
    n->buffer_capacity = n->buffer_count;
    for (i = 0; i < n->buffer_count; i++) {
      AbiBufferView *v = &n->buffers[i];
      size_t         at = d->cur.pos;
      uint8_t        present;

      v->role = abi_cur_u8(&d->cur);
      if (d->cur.failed) return NULL;
      if (v->role > ABI_ROLE__MAX) {
        abi_cur_fail(&d->cur, ABI_ERR_BAD_ENUM, at, "unknown buffer role %u",
                     (unsigned)v->role);
        return NULL;
      }
      if (!abi_cur_bool(&d->cur, "buffer.present", &present)) return NULL;
      v->present = present;
      if (!abi_cur_zeros(&d->cur, 2, "buffer.reserved")) return NULL;
      if (present) {
        v->allocation_id = abi_cur_u32(&d->cur);
        v->byte_offset = abi_cur_u64(&d->cur);
        v->logical_length = abi_cur_u64(&d->cur);
      } else {
        v->allocation_id = 0;
        v->byte_offset = 0;
        v->logical_length = 0;
      }
      if (d->cur.failed) return NULL;
    }
  }

  n->n_children = abi_cur_u32(&d->cur);
  n->child_count = abi_cur_u32(&d->cur);
  if (d->cur.failed) return NULL;
  if (!dec_count_guard(d, n->child_count, 4,
                       ABI_LIMIT_TREE_NODES - d->nodes + 1, "array child")) {
    return NULL;
  }
  if (n->child_count) {
    n->children = ABI_ARENA_ARRAY(d->c->arena, AbiArrayNode *, n->child_count);
    if (!n->children) {
      abi_cur_fail(&d->cur, ABI_ERR_NO_MEMORY, d->cur.pos, "out of memory");
      return NULL;
    }
    n->child_capacity = n->child_count;
    for (i = 0; i < n->child_count; i++) {
      n->children[i] = dec_array_node(d, depth + 1);
      if (!n->children[i]) return NULL;
    }
  }

  if (!abi_cur_bool(&d->cur, "array.has_dictionary", &has_dict)) return NULL;
  if (has_dict) {
    n->dictionary = dec_array_node(d, depth + 1);
    if (!n->dictionary) return NULL;
  }
  return n;
}

static int dec_callseq(AbiDec *d) {
  uint32_t count, i;
  size_t   at = d->cur.pos;

  count = abi_cur_u32(&d->cur);
  if (d->cur.failed) return 0;
  if (count == 0) {
    /* the section exists only to carry ops; empty, it should not be present */
    return abi_cur_fail(&d->cur, ABI_ERR_NOT_CANONICAL, at,
                        "empty CALLSEQ section must be omitted");
  }
  if (!dec_count_guard(d, count, 10, ABI_LIMIT_OP_COUNT, "op")) return 0;

  d->c->ops = ABI_ARENA_ARRAY(d->c->arena, AbiOp, count);
  if (!d->c->ops) {
    return abi_cur_fail(&d->cur, ABI_ERR_NO_MEMORY, d->cur.pos,
                        "out of memory");
  }
  d->c->op_capacity = count;
  for (i = 0; i < count; i++) {
    size_t op_at = d->cur.pos;
    d->c->ops[i].code = abi_cur_u16(&d->cur);
    d->c->ops[i].arg0 = abi_cur_u32(&d->cur);
    d->c->ops[i].arg1 = abi_cur_u32(&d->cur);
    if (d->cur.failed) return 0;
    if (d->c->ops[i].code > ABI_OP__MAX) {
      return abi_cur_fail(&d->cur, ABI_ERR_BAD_ENUM, op_at,
                          "unknown op code %u", (unsigned)d->c->ops[i].code);
    }
  }
  d->c->op_count = count;
  return !d->cur.failed;
}

static int dec_expected(AbiDec *d) {
  size_t at = d->cur.pos;

  d->c->expected.validation_level = abi_cur_u8(&d->cur);
  d->c->expected.expected_outcome = abi_cur_u8(&d->cur);
  if (d->cur.failed) return 0;
  if (d->c->expected.validation_level > ABI_VALIDATE_FULL) {
    return abi_cur_fail(&d->cur, ABI_ERR_BAD_ENUM, at,
                        "unknown validation level %u",
                        (unsigned)d->c->expected.validation_level);
  }
  if (d->c->expected.expected_outcome > ABI_EXPECT_UNSPECIFIED) {
    return abi_cur_fail(&d->cur, ABI_ERR_BAD_ENUM, at + 1,
                        "unknown expected outcome %u",
                        (unsigned)d->c->expected.expected_outcome);
  }
  if (!abi_cur_zeros(&d->cur, 2, "expected.reserved")) return 0;
  if (!abi_cur_bytes(&d->cur, d->c->arena, &d->c->expected.spec_clause))
    return 0;
  if (!abi_cur_bytes(&d->cur, d->c->arena, &d->c->expected.notes)) return 0;
  return !d->cur.failed;
}

/* --- header and section loop --------------------------------------------- */

static int dec_header(AbiDec *d, size_t size, uint32_t *out_sections,
                      uint32_t *out_total, uint8_t *out_class) {
  AbiCur  *cur = &d->cur;
  uint16_t ver, header_size;
  uint32_t bom, total;
  uint8_t  cls;

  if (size < ABICASE_HEADER_SIZE) {
    return abi_cur_fail(cur, ABI_ERR_TRUNCATED, 0,
                        "file of %llu bytes is shorter than the %u-byte header",
                        (unsigned long long)size,
                        (unsigned)ABICASE_HEADER_SIZE);
  }
  if (cur->data[0] != ABICASE_MAGIC0 || cur->data[1] != ABICASE_MAGIC1 ||
      cur->data[2] != ABICASE_MAGIC2 || cur->data[3] != ABICASE_MAGIC3) {
    return abi_cur_fail(cur, ABI_ERR_BAD_MAGIC, 0,
                        "bad magic %02x %02x %02x %02x, expected 'ABIC'",
                        cur->data[0], cur->data[1], cur->data[2], cur->data[3]);
  }
  cur->pos = 4;

  ver = abi_cur_u16(cur);
  if (ver != ABICASE_SCHEMA_VERSION) {
    return abi_cur_fail(cur, ABI_ERR_BAD_VERSION, 4,
                        "case_schema_ver %u, this build supports %u",
                        (unsigned)ver, (unsigned)ABICASE_SCHEMA_VERSION);
  }
  header_size = abi_cur_u16(cur);
  if (header_size != ABICASE_HEADER_SIZE) {
    return abi_cur_fail(cur, ABI_ERR_BAD_VERSION, 6,
                        "header_size %u, version 1 requires %u",
                        (unsigned)header_size, (unsigned)ABICASE_HEADER_SIZE);
  }
  bom = abi_cur_u32(cur);
  if (bom != ABICASE_BOM) {
    /*
     * A byte-swapped marker means the writer serialized native integers on a
     * big-endian host. Swapping it back would paper over exactly the class of
     * defect this project exists to surface, so it is an error, not a mode.
     */
    return abi_cur_fail(cur, ABI_ERR_BAD_BYTE_ORDER, 8,
                        "byte-order marker is 0x%08lx, expected 0x%08lx%s",
                        (unsigned long)bom, (unsigned long)ABICASE_BOM,
                        (bom == 0xFFFE0000u) ? " (file written big-endian)"
                                             : "");
  }
  cls = abi_cur_u8(cur);
  if (cls > ABI_CLASS_C) {
    return abi_cur_fail(cur, ABI_ERR_BAD_ENUM, 12, "unknown case class %u",
                        (unsigned)cls);
  }
  if (!abi_cur_zeros(cur, 3, "header.reserved")) return 0;

  *out_sections = abi_cur_u32(cur);
  total = abi_cur_u32(cur);
  if (cur->failed) return 0;

  if (total > ABI_LIMIT_TOTAL_SIZE) {
    return abi_cur_fail(cur, ABI_ERR_LIMIT_EXCEEDED, 20,
                        "total_size %lu exceeds limit %u", (unsigned long)total,
                        (unsigned)ABI_LIMIT_TOTAL_SIZE);
  }
  if (total < ABICASE_HEADER_SIZE) {
    return abi_cur_fail(cur, ABI_ERR_TRUNCATED, 20,
                        "total_size %lu is smaller than the header",
                        (unsigned long)total);
  }
  if ((uint64_t)total > (uint64_t)size) {
    return abi_cur_fail(cur, ABI_ERR_TRUNCATED, 20,
                        "total_size %lu but only %llu bytes available",
                        (unsigned long)total, (unsigned long long)size);
  }
  if ((uint64_t)total < (uint64_t)size) {
    return abi_cur_fail(
        cur, ABI_ERR_TRAILING_BYTES, total, "%llu byte(s) past total_size %lu",
        (unsigned long long)(size - total), (unsigned long)total);
  }

  /* payload id: identity and corruption check in one field */
  {
    uint8_t digest[32];
    abi_sha256(cur->data + ABICASE_HEADER_SIZE,
               (size_t)total - ABICASE_HEADER_SIZE, digest);
    if (memcmp(digest, cur->data + 24, ABICASE_ID_BYTES) != 0) {
      return abi_cur_fail(cur, ABI_ERR_DIGEST_MISMATCH, 24,
                          "payload_id does not match the payload");
    }
  }
  cur->pos = ABICASE_HEADER_SIZE;

  *out_total = total;
  *out_class = cls;
  return 1;
}

AbiStatus abi_case_decode(const uint8_t *data, size_t size, AbiCase **out,
                          AbiError *err) {
  AbiDec    d;
  uint32_t  section_count = 0, total = 0, i;
  uint8_t   cls = 0;
  uint32_t  prev_tag = 0;
  int       have_provenance = 0, have_allocations = 0, have_schema = 0;
  int       have_expected = 0;
  AbiStatus st;

  if (!data || !out) return ABI_ERR_INVALID_ARGUMENT;

  memset(&d, 0, sizeof(d));
  abi_cur_init(&d.cur, data, size);

  if (!dec_header(&d, size, &section_count, &total, &cls)) goto fail;

  d.c = abi_case_new((AbiClass)cls);
  if (!d.c) {
    abi_cur_fail(&d.cur, ABI_ERR_NO_MEMORY, 0, "out of memory");
    goto fail;
  }

  if (section_count > 6u) {
    abi_cur_fail(&d.cur, ABI_ERR_NOT_CANONICAL, 16,
                 "section_count %lu exceeds the six defined sections",
                 (unsigned long)section_count);
    goto fail;
  }

  for (i = 0; i < section_count; i++) {
    size_t   sec_at = d.cur.pos;
    uint16_t tag, sver;
    uint32_t sec_size;
    size_t   sec_end, saved_size;
    int      ok = 1;

    tag = abi_cur_u16(&d.cur);
    sver = abi_cur_u16(&d.cur);
    sec_size = abi_cur_u32(&d.cur);
    if (d.cur.failed) goto fail;

    if (sver != ABICASE_SECTION_VERSION) {
      abi_cur_fail(&d.cur, ABI_ERR_BAD_VERSION, sec_at + 2,
                   "section 0x%04x version %u, this build supports %u",
                   (unsigned)tag, (unsigned)sver,
                   (unsigned)ABICASE_SECTION_VERSION);
      goto fail;
    }
    if (tag <= prev_tag) {
      abi_cur_fail(&d.cur, ABI_ERR_NOT_CANONICAL, sec_at,
                   "section tag 0x%04x out of order or duplicated (previous "
                   "0x%04x); sections must ascend",
                   (unsigned)tag, (unsigned)prev_tag);
      goto fail;
    }
    prev_tag = tag;
    if (!abi_cur_need(&d.cur, sec_size)) goto fail;

    sec_end = d.cur.pos + sec_size;
    saved_size = d.cur.size;
    d.cur.size = sec_end; /* a section cannot read past its own payload */

    switch (tag) {
    case ABICASE_TAG_PROVENANCE:
      ok = dec_provenance(&d);
      have_provenance = 1;
      break;
    case ABICASE_TAG_ALLOCATIONS:
      ok = dec_allocations(&d);
      have_allocations = 1;
      break;
    case ABICASE_TAG_SCHEMA:
      d.nodes = 0;
      d.c->schema = dec_schema_node(&d, 0);
      ok = (d.c->schema != NULL);
      have_schema = 1;
      break;
    case ABICASE_TAG_ARRAY:
      d.nodes = 0;
      d.c->array = dec_array_node(&d, 0);
      ok = (d.c->array != NULL);
      break;
    case ABICASE_TAG_CALLSEQ: ok = dec_callseq(&d); break;
    case ABICASE_TAG_EXPECTED:
      ok = dec_expected(&d);
      have_expected = 1;
      break;
    default:
      ok = abi_cur_fail(&d.cur, ABI_ERR_BAD_ENUM, sec_at,
                        "unknown section tag 0x%04x", (unsigned)tag);
      break;
    }

    if (!ok || d.cur.failed) {
      d.cur.size = saved_size;
      goto fail;
    }
    if (d.cur.pos != sec_end) {
      d.cur.size = saved_size;
      abi_cur_fail(&d.cur, ABI_ERR_NOT_CANONICAL, d.cur.pos,
                   "section 0x%04x has %llu unread byte(s)", (unsigned)tag,
                   (unsigned long long)(sec_end - d.cur.pos));
      goto fail;
    }
    d.cur.size = saved_size;
  }

  if (d.cur.pos != (size_t)total) {
    abi_cur_fail(&d.cur, ABI_ERR_TRAILING_BYTES, d.cur.pos,
                 "%llu byte(s) after the last section",
                 (unsigned long long)((size_t)total - d.cur.pos));
    goto fail;
  }

  if (!have_provenance || !have_allocations || !have_schema || !have_expected) {
    abi_cur_fail(&d.cur, ABI_ERR_MISSING_SECTION, 0,
                 "missing required section(s):%s%s%s%s",
                 have_provenance ? "" : " PROVENANCE",
                 have_allocations ? "" : " ALLOCATIONS",
                 have_schema ? "" : " SCHEMA",
                 have_expected ? "" : " EXPECTED");
    goto fail;
  }

  /* The same rules the encoder ran before emitting. */
  st = abi_case_validate(d.c, err);
  if (st != ABI_OK) {
    abi_case_free(d.c);
    return st;
  }

  *out = d.c;
  if (err) {
    err->status = ABI_OK;
    err->offset = 0;
    err->message[0] = '\0';
  }
  return ABI_OK;

fail:
  if (d.c) abi_case_free(d.c);
  if (err) *err = d.cur.err;
  return d.cur.err.status;
}
