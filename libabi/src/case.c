/*
 * In-memory case model and its builders.
 *
 * Every builder copies what it is given, so a caller may pass stack buffers and
 * string literals freely; nothing here retains a caller pointer.
 */
#include <stdio.h>
#include <stdlib.h>

#include "abi_internal.h"

const char *abi_status_str(AbiStatus status) {
  switch (status) {
    case ABI_OK:                   return "ok";
    case ABI_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ABI_ERR_NO_MEMORY:        return "out of memory";
    case ABI_ERR_TRUNCATED:        return "truncated input";
    case ABI_ERR_BAD_MAGIC:        return "bad magic";
    case ABI_ERR_BAD_VERSION:      return "unsupported version";
    case ABI_ERR_BAD_BYTE_ORDER:   return "bad byte order";
    case ABI_ERR_NOT_CANONICAL:    return "not canonical";
    case ABI_ERR_LIMIT_EXCEEDED:   return "limit exceeded";
    case ABI_ERR_BAD_REFERENCE:    return "bad reference";
    case ABI_ERR_BAD_ENUM:         return "bad enum value";
    case ABI_ERR_TRAILING_BYTES:   return "trailing bytes";
    case ABI_ERR_DIGEST_MISMATCH:  return "payload id mismatch";
    case ABI_ERR_MISSING_SECTION:  return "missing required section";
    case ABI_ERR_CLASS_RULE:       return "class rule violated";
    case ABI_ERR_IO:               return "i/o error";
  }
  return "unknown status";
}

void abi_free(void *p) { free(p); }

/* --- growable vectors in the arena --------------------------------------- */

static int abi_vec_grow(AbiArena *a, void **items, uint32_t *capacity,
                        uint32_t count, size_t elem, size_t align,
                        uint32_t limit) {
  uint32_t newcap;
  void    *p;

  if (count < *capacity) return 1;
  if (count >= limit) return 0;
  newcap = (*capacity == 0) ? 4u : (*capacity * 2u);
  if (newcap > limit) newcap = limit;
  p = abi_arena_calloc(a, elem * (size_t)newcap, align);
  if (!p) return 0;
  if (count) memcpy(p, *items, elem * (size_t)count);
  *items = p;
  *capacity = newcap;
  return 1;
}

/* --- lifecycle ----------------------------------------------------------- */

AbiCase *abi_case_new(AbiClass cls) {
  AbiArena *arena;
  AbiCase  *c;

  if (cls > ABI_CLASS_C) return NULL;
  arena = abi_arena_new();
  if (!arena) return NULL;
  c = ABI_ARENA_NEW(arena, AbiCase);
  if (!c) {
    abi_arena_free(arena);
    return NULL;
  }
  c->arena = arena;
  c->cls = cls;

  /*
   * Every AbiBytes in a case is non-NULL from birth. An empty string and an
   * absent one are different things in this format, and the difference is
   * carried by explicit presence flags -- never by a NULL pointer, which would
   * turn a modelling distinction into a crash.
   */
  if (!abi_arena_cstr(arena, &c->provenance.rng_algorithm, "") ||
      !abi_arena_cstr(arena, &c->provenance.generator_version, "") ||
      !abi_arena_cstr(arena, &c->provenance.abi_doctor_version,
                      ABI_DOCTOR_VERSION) ||
      !abi_arena_cstr(arena, &c->provenance.spec_revision, "") ||
      !abi_arena_cstr(arena, &c->expected.spec_clause, "") ||
      !abi_arena_cstr(arena, &c->expected.notes, "")) {
    abi_arena_free(arena);
    return NULL;
  }
  c->expected.validation_level = ABI_VALIDATE_DEFAULT;
  c->expected.expected_outcome = ABI_EXPECT_UNSPECIFIED;
  return c;
}

void abi_case_free(AbiCase *c) {
  if (c) abi_arena_free(c->arena); /* the case itself lives in the arena */
}

/* --- provenance and expectation ------------------------------------------ */

AbiStatus abi_case_set_provenance(AbiCase *c, uint64_t seed,
                                  const char *rng_algorithm,
                                  uint32_t rng_version,
                                  const char *generator_version,
                                  const char *abi_doctor_version,
                                  const char *spec_revision) {
  if (!c) return ABI_ERR_INVALID_ARGUMENT;
  c->provenance.seed = seed;
  c->provenance.rng_version = rng_version;
  if (!abi_arena_cstr(c->arena, &c->provenance.rng_algorithm, rng_algorithm) ||
      !abi_arena_cstr(c->arena, &c->provenance.generator_version,
                      generator_version) ||
      !abi_arena_cstr(c->arena, &c->provenance.abi_doctor_version,
                      abi_doctor_version ? abi_doctor_version
                                         : ABI_DOCTOR_VERSION) ||
      !abi_arena_cstr(c->arena, &c->provenance.spec_revision, spec_revision)) {
    return ABI_ERR_NO_MEMORY;
  }
  return ABI_OK;
}

AbiStatus abi_case_set_expected(AbiCase *c, AbiValidationLevel level,
                                AbiExpectedOutcome outcome,
                                const char *spec_clause, const char *notes) {
  if (!c) return ABI_ERR_INVALID_ARGUMENT;
  if (level > ABI_VALIDATE_FULL || outcome > ABI_EXPECT_UNSPECIFIED) {
    return ABI_ERR_BAD_ENUM;
  }
  c->expected.validation_level = (uint8_t)level;
  c->expected.expected_outcome = (uint8_t)outcome;
  if (!abi_arena_cstr(c->arena, &c->expected.spec_clause, spec_clause) ||
      !abi_arena_cstr(c->arena, &c->expected.notes, notes)) {
    return ABI_ERR_NO_MEMORY;
  }
  return ABI_OK;
}

/* --- allocations --------------------------------------------------------- */

static int abi_is_pow2_u32(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

AbiStatus abi_case_add_allocation(AbiCase *c, const void *bytes, uint64_t size,
                                  uint32_t alignment, uint32_t *out_id) {
  AbiAllocation *a;

  if (!c || (!bytes && size)) return ABI_ERR_INVALID_ARGUMENT;
  if (!abi_is_pow2_u32(alignment) || alignment > ABI_LIMIT_ALIGNMENT) {
    return ABI_ERR_INVALID_ARGUMENT;
  }
  if (size > ABI_LIMIT_ALLOC_BYTES) return ABI_ERR_LIMIT_EXCEEDED;
  if (!abi_vec_grow(c->arena, (void **)&c->allocations, &c->alloc_capacity,
                    c->alloc_count, sizeof(AbiAllocation), _Alignof(AbiAllocation),
                    ABI_LIMIT_ALLOC_COUNT)) {
    return (c->alloc_count >= ABI_LIMIT_ALLOC_COUNT) ? ABI_ERR_LIMIT_EXCEEDED
                                                     : ABI_ERR_NO_MEMORY;
  }
  a = &c->allocations[c->alloc_count];
  a->size_bytes = size;
  a->alignment = alignment;
  if (size) {
    a->bytes = (const uint8_t *)abi_arena_dup(c->arena, bytes, (size_t)size, 1);
    if (!a->bytes) return ABI_ERR_NO_MEMORY;
  } else {
    a->bytes = NULL;
  }
  if (out_id) *out_id = c->alloc_count;
  c->alloc_count++;
  return ABI_OK;
}

/* --- schema -------------------------------------------------------------- */

AbiSchemaNode *abi_schema_new(AbiCase *c, const char *format) {
  AbiSchemaNode *n;
  if (!c) return NULL;
  n = ABI_ARENA_NEW(c->arena, AbiSchemaNode);
  if (!n) return NULL;
  if (!abi_arena_cstr(c->arena, &n->format, format) ||
      !abi_arena_cstr(c->arena, &n->name, "")) {
    return NULL;
  }
  return n;
}

AbiStatus abi_schema_set_format(AbiCase *c, AbiSchemaNode *n, const void *format,
                                uint32_t len) {
  if (!c || !n || (!format && len)) return ABI_ERR_INVALID_ARGUMENT;
  if (len > ABI_LIMIT_BYTES_LEN) return ABI_ERR_LIMIT_EXCEEDED;
  if (!abi_arena_bytes(c->arena, &n->format, format, len)) return ABI_ERR_NO_MEMORY;
  return ABI_OK;
}

AbiStatus abi_schema_set_name(AbiCase *c, AbiSchemaNode *n, const char *name) {
  if (!c || !n) return ABI_ERR_INVALID_ARGUMENT;
  if (!name) { /* ArrowSchema.name == NULL, which is not the empty name */
    n->has_name = 0;
    return ABI_OK;
  }
  if (!abi_arena_cstr(c->arena, &n->name, name)) return ABI_ERR_NO_MEMORY;
  n->has_name = 1;
  return ABI_OK;
}

AbiStatus abi_schema_set_metadata_present(AbiCase *c, AbiSchemaNode *n) {
  if (!c || !n) return ABI_ERR_INVALID_ARGUMENT;
  n->has_metadata = 1;
  return ABI_OK;
}

AbiStatus abi_schema_add_metadata(AbiCase *c, AbiSchemaNode *n, const void *key,
                                  uint32_t key_len, const void *value,
                                  uint32_t value_len) {
  AbiMetadataKV *kv;
  if (!c || !n || (!key && key_len) || (!value && value_len)) {
    return ABI_ERR_INVALID_ARGUMENT;
  }
  if (key_len > ABI_LIMIT_BYTES_LEN || value_len > ABI_LIMIT_BYTES_LEN) {
    return ABI_ERR_LIMIT_EXCEEDED;
  }
  if (!abi_vec_grow(c->arena, (void **)&n->metadata, &n->metadata_capacity,
                    n->metadata_count, sizeof(AbiMetadataKV),
                    _Alignof(AbiMetadataKV), ABI_LIMIT_TREE_NODES)) {
    return ABI_ERR_NO_MEMORY;
  }
  kv = &n->metadata[n->metadata_count];
  if (!abi_arena_bytes(c->arena, &kv->key, key, key_len) ||
      !abi_arena_bytes(c->arena, &kv->value, value, value_len)) {
    return ABI_ERR_NO_MEMORY;
  }
  n->metadata_count++;
  n->has_metadata = 1;
  return ABI_OK;
}

AbiStatus abi_schema_add_child(AbiCase *c, AbiSchemaNode *parent,
                               AbiSchemaNode *child) {
  if (!c || !parent || !child) return ABI_ERR_INVALID_ARGUMENT;
  if (!abi_vec_grow(c->arena, (void **)&parent->children, &parent->child_capacity,
                    parent->child_count, sizeof(AbiSchemaNode *),
                    _Alignof(AbiSchemaNode *), ABI_LIMIT_TREE_NODES)) {
    return ABI_ERR_NO_MEMORY;
  }
  parent->children[parent->child_count] = child;
  parent->child_count++;
  parent->n_children = parent->child_count; /* declared follows provided */
  return ABI_OK;
}

void abi_schema_set_dictionary(AbiSchemaNode *n, AbiSchemaNode *dict) {
  if (n) n->dictionary = dict;
}

void abi_schema_set_declared_children(AbiSchemaNode *n, uint32_t n_children) {
  if (n) n->n_children = n_children;
}

/* --- array --------------------------------------------------------------- */

AbiArrayNode *abi_array_new(AbiCase *c) {
  if (!c) return NULL;
  return ABI_ARENA_NEW(c->arena, AbiArrayNode);
}

static AbiStatus abi_array_push_buffer(AbiCase *c, AbiArrayNode *n,
                                       const AbiBufferView *view) {
  if (!abi_vec_grow(c->arena, (void **)&n->buffers, &n->buffer_capacity,
                    n->buffer_count, sizeof(AbiBufferView),
                    _Alignof(AbiBufferView), ABI_LIMIT_TREE_NODES)) {
    return ABI_ERR_NO_MEMORY;
  }
  n->buffers[n->buffer_count] = *view;
  n->buffer_count++;
  n->n_buffers = n->buffer_count; /* declared follows provided */
  return ABI_OK;
}

AbiStatus abi_array_add_buffer(AbiCase *c, AbiArrayNode *n, AbiBufferRole role,
                               uint32_t allocation_id, uint64_t byte_offset,
                               uint64_t logical_length) {
  AbiBufferView v;
  if (!c || !n) return ABI_ERR_INVALID_ARGUMENT;
  if (role > ABI_ROLE__MAX) return ABI_ERR_BAD_ENUM;
  v.role = (uint8_t)role;
  v.present = 1;
  v.allocation_id = allocation_id;
  v.byte_offset = byte_offset;
  v.logical_length = logical_length;
  return abi_array_push_buffer(c, n, &v);
}

AbiStatus abi_array_add_null_buffer(AbiCase *c, AbiArrayNode *n,
                                    AbiBufferRole role) {
  AbiBufferView v;
  if (!c || !n) return ABI_ERR_INVALID_ARGUMENT;
  if (role > ABI_ROLE__MAX) return ABI_ERR_BAD_ENUM;
  v.role = (uint8_t)role;
  v.present = 0;
  v.allocation_id = 0;
  v.byte_offset = 0;
  v.logical_length = 0;
  return abi_array_push_buffer(c, n, &v);
}

AbiStatus abi_array_add_child(AbiCase *c, AbiArrayNode *parent,
                              AbiArrayNode *child) {
  if (!c || !parent || !child) return ABI_ERR_INVALID_ARGUMENT;
  if (!abi_vec_grow(c->arena, (void **)&parent->children, &parent->child_capacity,
                    parent->child_count, sizeof(AbiArrayNode *),
                    _Alignof(AbiArrayNode *), ABI_LIMIT_TREE_NODES)) {
    return ABI_ERR_NO_MEMORY;
  }
  parent->children[parent->child_count] = child;
  parent->child_count++;
  parent->n_children = parent->child_count;
  return ABI_OK;
}

void abi_array_set_dictionary(AbiArrayNode *n, AbiArrayNode *dict) {
  if (n) n->dictionary = dict;
}

void abi_array_set_declared_buffers(AbiArrayNode *n, uint32_t n_buffers) {
  if (n) n->n_buffers = n_buffers;
}

void abi_array_set_declared_children(AbiArrayNode *n, uint32_t n_children) {
  if (n) n->n_children = n_children;
}

void abi_case_set_schema(AbiCase *c, AbiSchemaNode *schema) {
  if (c) c->schema = schema;
}

void abi_case_set_array(AbiCase *c, AbiArrayNode *array) {
  if (c) c->array = array;
}

AbiStatus abi_case_add_op(AbiCase *c, AbiOpCode code, uint32_t arg0,
                          uint32_t arg1) {
  AbiOp *op;
  if (!c) return ABI_ERR_INVALID_ARGUMENT;
  if ((int)code < 0 || code > ABI_OP__MAX) return ABI_ERR_BAD_ENUM;
  if (!abi_vec_grow(c->arena, (void **)&c->ops, &c->op_capacity, c->op_count,
                    sizeof(AbiOp), _Alignof(AbiOp), ABI_LIMIT_OP_COUNT)) {
    return (c->op_count >= ABI_LIMIT_OP_COUNT) ? ABI_ERR_LIMIT_EXCEEDED
                                               : ABI_ERR_NO_MEMORY;
  }
  op = &c->ops[c->op_count];
  op->code = (uint16_t)code;
  op->arg0 = arg0;
  op->arg1 = arg1;
  c->op_count++;
  return ABI_OK;
}

/* --- file i/o ------------------------------------------------------------ */

/*
 * "rb" / "wb" are load-bearing, not decoration. In text mode on Windows the C
 * runtime rewrites 0x0A on write and 0x0D 0x0A on read, which silently corrupts
 * a binary container in a way that survives casual inspection and shows up
 * later as a truncation error on a file that was written correctly.
 */
AbiStatus abi_read_file(const char *path, uint8_t **out_data, size_t *out_size) {
  FILE    *f;
  uint8_t *buf = NULL;
  size_t   cap = 0, len = 0;

  if (!path || !out_data || !out_size) return ABI_ERR_INVALID_ARGUMENT;
  f = fopen(path, "rb");
  if (!f) return ABI_ERR_IO;

  for (;;) {
    size_t got;
    if (len == cap) {
      size_t   ncap = cap ? cap * 2 : 8192;
      uint8_t *np;
      if (ncap > ABI_LIMIT_TOTAL_SIZE + 1u) ncap = ABI_LIMIT_TOTAL_SIZE + 1u;
      if (ncap == cap) { /* would exceed the format limit: stop reading */
        free(buf);
        fclose(f);
        return ABI_ERR_LIMIT_EXCEEDED;
      }
      np = (uint8_t *)realloc(buf, ncap);
      if (!np) {
        free(buf);
        fclose(f);
        return ABI_ERR_NO_MEMORY;
      }
      buf = np;
      cap = ncap;
    }
    got = fread(buf + len, 1, cap - len, f);
    len += got;
    if (got == 0) break;
  }
  if (ferror(f)) {
    free(buf);
    fclose(f);
    return ABI_ERR_IO;
  }
  fclose(f);
  *out_data = buf;
  *out_size = len;
  return ABI_OK;
}

AbiStatus abi_case_write_file(const AbiCase *c, const char *path) {
  uint8_t  *data = NULL;
  size_t    size = 0;
  AbiStatus st;
  FILE     *f;

  if (!c || !path) return ABI_ERR_INVALID_ARGUMENT;
  st = abi_case_encode(c, &data, &size);
  if (st != ABI_OK) return st;

  f = fopen(path, "wb");
  if (!f) {
    free(data);
    return ABI_ERR_IO;
  }
  if (size && fwrite(data, 1, size, f) != size) {
    fclose(f);
    free(data);
    return ABI_ERR_IO;
  }
  free(data);
  return (fclose(f) == 0) ? ABI_OK : ABI_ERR_IO;
}

AbiStatus abi_case_read_file(const char *path, AbiCase **out, AbiError *err) {
  uint8_t  *data = NULL;
  size_t    size = 0;
  AbiStatus st = abi_read_file(path, &data, &size);
  if (st != ABI_OK) {
    if (err) {
      err->status = st;
      err->offset = 0;
      snprintf(err->message, sizeof(err->message), "cannot read '%s': %s", path,
               abi_status_str(st));
    }
    return st;
  }
  st = abi_case_decode(data, size, out, err);
  free(data);
  return st;
}
