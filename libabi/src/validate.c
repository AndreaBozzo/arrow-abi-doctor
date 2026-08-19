/*
 * Semantic and class validation -- docs/abicase-format.md 5.1, 7.2, 7.3, 8, 11.
 *
 * Called from the encoder before it emits and from the decoder after it parses.
 * One implementation, two directions: any rule enforced on read is enforced on
 * write, so the library cannot produce a file it would then refuse.
 *
 * Syntactic canonicality (section order, reserved bytes, minimal fill) is not
 * here -- it lives in the decoder, where byte offsets exist to report.
 */
#include <stdio.h>

#include "abi_internal.h"

typedef struct {
  const AbiCase *c;
  AbiError      *err;
  uint32_t       nodes;
} AbiVal;

static AbiStatus val_fail(AbiVal *v, AbiStatus st, const char *fmt, ...) {
  va_list ap;
  if (v->err) {
    v->err->status = st;
    v->err->offset = 0; /* structural rules are not tied to a byte position */
    va_start(ap, fmt);
    vsnprintf(v->err->message, sizeof(v->err->message), fmt, ap);
    va_end(ap);
  }
  return st;
}

static int abi_is_pow2_u32(uint32_t x) { return x != 0 && (x & (x - 1)) == 0; }

/* Class A and B2 are meant to be well-formed containers; B1 and C are not. */
static int class_requires_consistent_counts(AbiClass cls) {
  return cls == ABI_CLASS_A || cls == ABI_CLASS_B2;
}

static AbiStatus val_bytes(AbiVal *v, AbiBytes b, const char *what) {
  if (!b.data) return val_fail(v, ABI_ERR_INVALID_ARGUMENT, "%s is unset", what);
  if (b.size > ABI_LIMIT_BYTES_LEN) {
    return val_fail(v, ABI_ERR_LIMIT_EXCEEDED, "%s of %lu exceeds limit %lu",
                    what, (unsigned long)b.size,
                    (unsigned long)ABI_LIMIT_BYTES_LEN);
  }
  return ABI_OK;
}

static AbiStatus val_schema(AbiVal *v, const AbiSchemaNode *n, uint32_t depth) {
  AbiStatus st;
  uint32_t  i;

  if (!n) return val_fail(v, ABI_ERR_INVALID_ARGUMENT, "null schema node");
  if (depth > ABI_LIMIT_TREE_DEPTH) {
    return val_fail(v, ABI_ERR_LIMIT_EXCEEDED, "schema depth exceeds %u",
                    (unsigned)ABI_LIMIT_TREE_DEPTH);
  }
  if (++v->nodes > ABI_LIMIT_TREE_NODES) {
    return val_fail(v, ABI_ERR_LIMIT_EXCEEDED, "schema node count exceeds %u",
                    (unsigned)ABI_LIMIT_TREE_NODES);
  }

  st = val_bytes(v, n->format, "schema format");
  if (st != ABI_OK) return st;
  st = val_bytes(v, n->name, "schema name");
  if (st != ABI_OK) return st;

  if (n->metadata_count > 0 && !n->has_metadata) {
    return val_fail(v, ABI_ERR_INVALID_ARGUMENT,
                    "schema carries %lu metadata pairs but is marked absent",
                    (unsigned long)n->metadata_count);
  }
  for (i = 0; i < n->metadata_count; i++) {
    st = val_bytes(v, n->metadata[i].key, "metadata key");
    if (st != ABI_OK) return st;
    st = val_bytes(v, n->metadata[i].value, "metadata value");
    if (st != ABI_OK) return st;
  }

  if (n->child_count > 0 && !n->children) {
    return val_fail(v, ABI_ERR_INVALID_ARGUMENT, "schema children missing");
  }
  if (class_requires_consistent_counts(v->c->cls) &&
      n->child_count != n->n_children) {
    return val_fail(v, ABI_ERR_CLASS_RULE,
                    "class %d requires schema child_count (%lu) == n_children "
                    "(%lu); declare the mismatch in a B1 or C case",
                    (int)v->c->cls, (unsigned long)n->child_count,
                    (unsigned long)n->n_children);
  }
  for (i = 0; i < n->child_count; i++) {
    st = val_schema(v, n->children[i], depth + 1);
    if (st != ABI_OK) return st;
  }
  if (n->dictionary) {
    st = val_schema(v, n->dictionary, depth + 1);
    if (st != ABI_OK) return st;
  }
  return ABI_OK;
}

static AbiStatus val_array(AbiVal *v, const AbiArrayNode *n, uint32_t depth) {
  AbiStatus st;
  uint32_t  i;

  if (!n) return val_fail(v, ABI_ERR_INVALID_ARGUMENT, "null array node");
  if (depth > ABI_LIMIT_TREE_DEPTH) {
    return val_fail(v, ABI_ERR_LIMIT_EXCEEDED, "array depth exceeds %u",
                    (unsigned)ABI_LIMIT_TREE_DEPTH);
  }
  if (++v->nodes > ABI_LIMIT_TREE_NODES) {
    return val_fail(v, ABI_ERR_LIMIT_EXCEEDED, "array node count exceeds %u",
                    (unsigned)ABI_LIMIT_TREE_NODES);
  }

  if (n->buffer_count > 0 && !n->buffers) {
    return val_fail(v, ABI_ERR_INVALID_ARGUMENT, "array buffers missing");
  }
  for (i = 0; i < n->buffer_count; i++) {
    const AbiBufferView *b = &n->buffers[i];
    const AbiAllocation *a;

    if (b->role > ABI_ROLE__MAX) {
      return val_fail(v, ABI_ERR_BAD_ENUM, "buffer %lu has unknown role %u",
                      (unsigned long)i, (unsigned)b->role);
    }
    if (b->present > 1) {
      return val_fail(v, ABI_ERR_NOT_CANONICAL,
                      "buffer %lu presence flag must be 0 or 1",
                      (unsigned long)i);
    }
    if (!b->present) {
      if (b->allocation_id || b->byte_offset || b->logical_length) {
        return val_fail(v, ABI_ERR_NOT_CANONICAL,
                        "buffer %lu is absent but carries a view",
                        (unsigned long)i);
      }
      continue;
    }
    if (b->allocation_id >= v->c->alloc_count) {
      return val_fail(v, ABI_ERR_BAD_REFERENCE,
                      "buffer %lu references allocation %lu of %lu",
                      (unsigned long)i, (unsigned long)b->allocation_id,
                      (unsigned long)v->c->alloc_count);
    }
    a = &v->c->allocations[b->allocation_id];
    /*
     * Containment, enforced for every class including B1. A view that ran past
     * its own allocation would make this tool construct an out-of-bounds
     * pointer; the resulting fault would be ours and would be reported as the
     * consumer's. "Buffer too short" is expressed as a small allocation with a
     * large ArrowArray.length instead -- see format 7.2.
     */
    if (b->byte_offset > a->size_bytes ||
        b->logical_length > a->size_bytes - b->byte_offset) {
      return val_fail(v, ABI_ERR_BAD_REFERENCE,
                      "buffer %lu view [%llu,+%llu) escapes allocation %lu of "
                      "%llu bytes",
                      (unsigned long)i, (unsigned long long)b->byte_offset,
                      (unsigned long long)b->logical_length,
                      (unsigned long)b->allocation_id,
                      (unsigned long long)a->size_bytes);
    }
  }

  if (class_requires_consistent_counts(v->c->cls)) {
    if (n->buffer_count != n->n_buffers) {
      return val_fail(v, ABI_ERR_CLASS_RULE,
                      "class %d requires buffer_count (%lu) == n_buffers (%lu)",
                      (int)v->c->cls, (unsigned long)n->buffer_count,
                      (unsigned long)n->n_buffers);
    }
    if (n->child_count != n->n_children) {
      return val_fail(v, ABI_ERR_CLASS_RULE,
                      "class %d requires array child_count (%lu) == n_children "
                      "(%lu)",
                      (int)v->c->cls, (unsigned long)n->child_count,
                      (unsigned long)n->n_children);
    }
  }

  if (n->child_count > 0 && !n->children) {
    return val_fail(v, ABI_ERR_INVALID_ARGUMENT, "array children missing");
  }
  for (i = 0; i < n->child_count; i++) {
    st = val_array(v, n->children[i], depth + 1);
    if (st != ABI_OK) return st;
  }
  if (n->dictionary) {
    st = val_array(v, n->dictionary, depth + 1);
    if (st != ABI_OK) return st;
  }
  return ABI_OK;
}

/* Class A/B2 only: the array tree must mirror the schema tree. */
static AbiStatus val_shapes_agree(AbiVal *v, const AbiSchemaNode *s,
                                  const AbiArrayNode *a, uint32_t depth) {
  uint32_t i;

  if (depth > ABI_LIMIT_TREE_DEPTH) return ABI_OK; /* already reported */
  if (s->child_count != a->child_count) {
    return val_fail(v, ABI_ERR_CLASS_RULE,
                    "class %d requires matching tree shapes: schema has %lu "
                    "children, array has %lu",
                    (int)v->c->cls, (unsigned long)s->child_count,
                    (unsigned long)a->child_count);
  }
  if ((s->dictionary != NULL) != (a->dictionary != NULL)) {
    return val_fail(v, ABI_ERR_CLASS_RULE,
                    "class %d requires schema and array to agree on the "
                    "presence of a dictionary",
                    (int)v->c->cls);
  }
  for (i = 0; i < s->child_count; i++) {
    AbiStatus st = val_shapes_agree(v, s->children[i], a->children[i], depth + 1);
    if (st != ABI_OK) return st;
  }
  if (s->dictionary) {
    return val_shapes_agree(v, s->dictionary, a->dictionary, depth + 1);
  }
  return ABI_OK;
}

static AbiStatus val_ops(AbiVal *v) {
  uint32_t i;

  if (v->c->op_count > ABI_LIMIT_OP_COUNT) {
    return val_fail(v, ABI_ERR_LIMIT_EXCEEDED, "op_count %lu exceeds limit %u",
                    (unsigned long)v->c->op_count, (unsigned)ABI_LIMIT_OP_COUNT);
  }
  if (v->c->op_count && !v->c->ops) {
    return val_fail(v, ABI_ERR_INVALID_ARGUMENT, "call sequence missing");
  }
  for (i = 0; i < v->c->op_count; i++) {
    const AbiOp *op = &v->c->ops[i];
    if (op->code > ABI_OP__MAX) {
      return val_fail(v, ABI_ERR_BAD_ENUM, "op %lu has unknown code %u",
                      (unsigned long)i, (unsigned)op->code);
    }
    /*
     * These three encode behaviour the C Data Interface forbids a consumer to
     * perform. Outside a class C case they would not be a test, they would be a
     * mislabelled one.
     */
    if ((op->code == ABI_OP_RELEASE_CHILD || op->code == ABI_OP_RELEASE_DICTIONARY ||
         op->code == ABI_OP_USE_AFTER_RELEASE) &&
        v->c->cls != ABI_CLASS_C) {
      return val_fail(v, ABI_ERR_CLASS_RULE,
                      "op %lu (code %u) is consumer misuse and requires class C",
                      (unsigned long)i, (unsigned)op->code);
    }
    if (op->code != ABI_OP_RELEASE_CHILD && op->arg0 != 0) {
      return val_fail(v, ABI_ERR_NOT_CANONICAL,
                      "op %lu (code %u) does not use arg0, which must be zero",
                      (unsigned long)i, (unsigned)op->code);
    }
    if (op->arg1 != 0) {
      return val_fail(v, ABI_ERR_NOT_CANONICAL,
                      "op %lu does not use arg1, which must be zero",
                      (unsigned long)i);
    }
  }
  return ABI_OK;
}

AbiStatus abi_case_validate(const AbiCase *c, AbiError *err) {
  AbiVal    v;
  AbiStatus st;
  uint32_t  i;

  if (!c) return ABI_ERR_INVALID_ARGUMENT;
  v.c = c;
  v.err = err;
  v.nodes = 0;
  if (err) {
    err->status = ABI_OK;
    err->offset = 0;
    err->message[0] = '\0';
  }

  if (c->cls > ABI_CLASS_C) {
    return val_fail(&v, ABI_ERR_BAD_ENUM, "unknown case class %d", (int)c->cls);
  }
  if (!c->schema) {
    return val_fail(&v, ABI_ERR_MISSING_SECTION, "a case requires a schema");
  }

  if (c->alloc_count > ABI_LIMIT_ALLOC_COUNT) {
    return val_fail(&v, ABI_ERR_LIMIT_EXCEEDED,
                    "alloc_count %lu exceeds limit %u",
                    (unsigned long)c->alloc_count,
                    (unsigned)ABI_LIMIT_ALLOC_COUNT);
  }
  if (c->alloc_count && !c->allocations) {
    return val_fail(&v, ABI_ERR_INVALID_ARGUMENT, "allocation table missing");
  }
  for (i = 0; i < c->alloc_count; i++) {
    const AbiAllocation *a = &c->allocations[i];
    if (a->size_bytes > ABI_LIMIT_ALLOC_BYTES) {
      return val_fail(&v, ABI_ERR_LIMIT_EXCEEDED,
                      "allocation %lu of %llu bytes exceeds limit %u",
                      (unsigned long)i, (unsigned long long)a->size_bytes,
                      (unsigned)ABI_LIMIT_ALLOC_BYTES);
    }
    if (!abi_is_pow2_u32(a->alignment) || a->alignment > ABI_LIMIT_ALIGNMENT) {
      return val_fail(&v, ABI_ERR_INVALID_ARGUMENT,
                      "allocation %lu alignment %lu must be a power of two in "
                      "[1,%u]",
                      (unsigned long)i, (unsigned long)a->alignment,
                      (unsigned)ABI_LIMIT_ALIGNMENT);
    }
    if ((a->size_bytes == 0) != (a->bytes == NULL)) {
      return val_fail(&v, ABI_ERR_INVALID_ARGUMENT,
                      "allocation %lu: bytes must be NULL exactly when "
                      "size_bytes is zero",
                      (unsigned long)i);
    }
  }

  if (c->expected.validation_level > ABI_VALIDATE_FULL) {
    return val_fail(&v, ABI_ERR_BAD_ENUM, "unknown validation level %u",
                    (unsigned)c->expected.validation_level);
  }
  if (c->expected.expected_outcome > ABI_EXPECT_UNSPECIFIED) {
    return val_fail(&v, ABI_ERR_BAD_ENUM, "unknown expected outcome %u",
                    (unsigned)c->expected.expected_outcome);
  }
  st = val_bytes(&v, c->expected.spec_clause, "spec_clause");
  if (st != ABI_OK) return st;
  st = val_bytes(&v, c->expected.notes, "notes");
  if (st != ABI_OK) return st;
  st = val_bytes(&v, c->provenance.rng_algorithm, "rng_algorithm");
  if (st != ABI_OK) return st;
  st = val_bytes(&v, c->provenance.generator_version, "generator_version");
  if (st != ABI_OK) return st;
  st = val_bytes(&v, c->provenance.abi_doctor_version, "abi_doctor_version");
  if (st != ABI_OK) return st;
  st = val_bytes(&v, c->provenance.spec_revision, "spec_revision");
  if (st != ABI_OK) return st;

  v.nodes = 0;
  st = val_schema(&v, c->schema, 0);
  if (st != ABI_OK) return st;

  if (c->array) {
    v.nodes = 0;
    st = val_array(&v, c->array, 0);
    if (st != ABI_OK) return st;
    if (class_requires_consistent_counts(c->cls)) {
      st = val_shapes_agree(&v, c->schema, c->array, 0);
      if (st != ABI_OK) return st;
    }
  }

  return val_ops(&v);
}
