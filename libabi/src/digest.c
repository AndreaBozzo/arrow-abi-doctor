/*
 * Dual digest -- docs/digest.md is normative for everything here.
 *
 * One walk feeds both hashes, so the two can never be computed over different
 * trees. Every multi-byte value is fed in little-endian regardless of the host,
 * and every read out of a buffer goes through memcpy: the alignment classes of
 * the conformance model deliberately place views one and four bytes into their
 * allocation, so an aligned load here would be undefined behaviour on exactly
 * the cases the model exists to exercise.
 */
#include <stdio.h>

#include "abi_internal.h"

#include "abi/digest.h"

typedef enum {
  DIG_INT32 = 0,
  DIG_INT64,
  DIG_FLOAT64,
  DIG_UTF8,
  DIG_BOOL,
  DIG_STRUCT
} DigType;

typedef struct {
  AbiSha256 physical;
  AbiSha256 logical;
  AbiError *err;
  uint32_t  nodes;
} DigCtx;

static AbiStatus dig_fail(DigCtx *d, AbiStatus st, const char *fmt, ...) {
  va_list ap;
  if (d->err) {
    d->err->status = st;
    d->err->offset = 0;
    va_start(ap, fmt);
    vsnprintf(d->err->message, sizeof(d->err->message), fmt, ap);
    va_end(ap);
  }
  return st;
}

/* --- feeding the hashes -------------------------------------------------- */

static void put_u64(AbiSha256 *h, uint64_t v) {
  uint8_t b[8];
  int     i;
  for (i = 0; i < 8; i++) {
    b[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
  }
  abi_sha256_update(h, b, sizeof(b));
}

static void put_i64(AbiSha256 *h, int64_t v) { put_u64(h, (uint64_t)v); }

/*
 * Length-prefixed, always. Concatenating raw byte strings would let "ab" + "c"
 * and "a" + "bc" hash identically, which for a digest whose whole job is
 * telling two arrays apart is not a subtle flaw.
 */
static void put_bytes(AbiSha256 *h, const void *p, uint64_t n) {
  put_u64(h, n);
  if (n) abi_sha256_update(h, p, (size_t)n);
}

static void put_cstr(AbiSha256 *h, const char *s) {
  if (!s) {
    /* An absent name is not an empty one; Arrow distinguishes them. */
    put_u64(h, 0xFFFFFFFFFFFFFFFFull);
    return;
  }
  put_bytes(h, s, (uint64_t)strlen(s));
}

/* --- reading buffers ----------------------------------------------------- */

static int32_t load_i32(const uint8_t *p, int64_t index) {
  int32_t v;
  memcpy(&v, p + index * 4, sizeof(v));
  return v;
}

static int64_t load_i64(const uint8_t *p, int64_t index) {
  int64_t v;
  memcpy(&v, p + index * 8, sizeof(v));
  return v;
}

static uint64_t load_f64_bits(const uint8_t *p, int64_t index) {
  uint64_t bits;
  memcpy(&bits, p + index * 8, sizeof(bits));
  return bits;
}

/* Arrow bitmaps are least-significant-bit first. */
static int get_bit(const uint8_t *bitmap, int64_t i) {
  return (bitmap[i >> 3] >> (unsigned)(i & 7)) & 1u;
}

static uint64_t bitmap_bytes(int64_t elements) {
  return (uint64_t)((elements + 7) / 8);
}

/* --- the type set -------------------------------------------------------- */

static AbiStatus dig_type_of(DigCtx *d, const char *format, DigType *out,
                             int64_t *out_buffers) {
  /*
   * Written unconditionally before anything can fail. The caller returns on a
   * non-OK status and never reads these, but GCC at -O2 cannot see that across
   * the call and rejects the caller under -Werror=maybe-uninitialized -- which
   * the Linux build catches and the MinGW one does not.
   */
  *out = DIG_STRUCT;
  *out_buffers = 0;
  if (!format) {
    return dig_fail(d, ABI_ERR_INVALID_ARGUMENT, "schema has no format string");
  }
  if (strcmp(format, "i") == 0) {
    *out = DIG_INT32;
    *out_buffers = 2;
  } else if (strcmp(format, "l") == 0) {
    *out = DIG_INT64;
    *out_buffers = 2;
  } else if (strcmp(format, "g") == 0) {
    *out = DIG_FLOAT64;
    *out_buffers = 2;
  } else if (strcmp(format, "u") == 0) {
    *out = DIG_UTF8;
    *out_buffers = 3;
  } else if (strcmp(format, "b") == 0) {
    *out = DIG_BOOL;
    *out_buffers = 2;
  } else if (strcmp(format, "+s") == 0) {
    *out = DIG_STRUCT;
    *out_buffers = 1;
  } else {
    return dig_fail(d, ABI_ERR_INVALID_ARGUMENT,
                    "format \"%s\" is outside the v0 feature set (i, l, g, u, "
                    "b, +s)",
                    format);
  }
  return ABI_OK;
}

/*
 * The byte extent of buffer `index`, computed from the schema and from
 * length + offset because the C Data Interface does not transmit buffer sizes.
 * This is the same arithmetic every consumer is obliged to do, which is exactly
 * why a producer that under-sizes a buffer makes all of them read out of
 * bounds; there is no way to notice from this side.
 */
static AbiStatus dig_buffer_len(DigCtx *d, DigType type,
                                const struct ArrowArray *a, int64_t index,
                                uint64_t *out) {
  int64_t n = a->offset + a->length; /* physical elements */

  if (index == 0) { /* validity, for every type here */
    *out = bitmap_bytes(n);
    return ABI_OK;
  }
  switch (type) {
  case DIG_INT32: *out = (uint64_t)n * 4u; return ABI_OK;
  case DIG_INT64:
  case DIG_FLOAT64: *out = (uint64_t)n * 8u; return ABI_OK;
  case DIG_BOOL: *out = bitmap_bytes(n); return ABI_OK;
  case DIG_UTF8:
    if (index == 1) {
      *out = ((uint64_t)n + 1u) * 4u;
      return ABI_OK;
    }
    /* The values buffer runs to the last offset of the physical range. Note
       the offset is *not* dropped here when length is 0: that is precisely the
       arrow-rs defect this project filed as apache/arrow-rs#10910. */
    if (!a->buffers[1]) {
      *out = 0;
      return ABI_OK;
    }
    {
      int32_t last = load_i32((const uint8_t *)a->buffers[1], n);
      /*
       * The one number in this file that cannot be checked against anything.
       * The C Data Interface transmits no buffer sizes, so the length of a
       * values buffer is whatever its own last offset says -- there is no
       * second source to reconcile it against, and a wrong one makes every
       * consumer read out of bounds (apache/arrow-rs#10910 is the same
       * arithmetic getting it wrong in the other direction).
       *
       * So it is capped rather than trusted. Above the format's own allocation
       * limit the value cannot have come from a case this project produced, and
       * reading it would make the fault ours. It is reached in practice by
       * digesting a little-endian corpus on a big-endian host, where a
       * byte-swapped offset of 5 reads as 83886080: the values are meaningless
       * there by format §10.1, and failing loudly is the correct answer.
       */
      if (last < 0 || (uint64_t)last > ABI_LIMIT_ALLOC_BYTES) {
        return dig_fail(d, ABI_ERR_LIMIT_EXCEEDED,
                        "utf8 values length %ld from the last offset is "
                        "negative or above the %u byte limit; the buffer "
                        "cannot be sized",
                        (long)last, (unsigned)ABI_LIMIT_ALLOC_BYTES);
      }
      *out = (uint64_t)last;
    }
    return ABI_OK;
  case DIG_STRUCT: break;
  }
  return dig_fail(d, ABI_ERR_INVALID_ARGUMENT, "buffer %lld out of range",
                  (long long)index);
}

/* --- the walk ------------------------------------------------------------ */

static AbiStatus dig_node(DigCtx *d, const struct ArrowSchema *s,
                          const struct ArrowArray *a, int64_t extra_offset,
                          int64_t count, uint32_t depth);

/*
 * Logical values, slot by slot. Null slots contribute their validity bit and
 * nothing else: the bytes under a null are unspecified, and a producer is free
 * to leave anything there.
 */
static AbiStatus dig_values(DigCtx *d, DigType type, const struct ArrowArray *a,
                            int64_t base, int64_t count, uint64_t values_len) {
  const uint8_t *validity = (const uint8_t *)a->buffers[0];
  const uint8_t *data = (type == DIG_STRUCT) ? NULL
                        : (type == DIG_UTF8) ? (const uint8_t *)a->buffers[2]
                                             : (const uint8_t *)a->buffers[1];
  const uint8_t *offsets =
      (type == DIG_UTF8) ? (const uint8_t *)a->buffers[1] : NULL;
  int64_t i;

  for (i = 0; i < count; i++) {
    int64_t p = base + i;
    int     valid = validity ? get_bit(validity, p) : 1;
    uint8_t flag = (uint8_t)(valid ? 1 : 0);

    abi_sha256_update(&d->logical, &flag, 1);
    if (!valid || type == DIG_STRUCT) continue;

    switch (type) {
    case DIG_INT32: put_u64(&d->logical, (uint64_t)load_i32(data, p)); break;
    case DIG_INT64: put_u64(&d->logical, (uint64_t)load_i64(data, p)); break;
    case DIG_FLOAT64: {
      uint64_t bits = load_f64_bits(data, p);
      /*
       * The two normalizations float64 exists for in the model (matrix 1.1).
       * Both directions of zero are the same logical value, and every NaN is
       * the same logical value whatever its payload -- so both collapse to one
       * representative rather than being compared bit for bit.
       */
      if ((bits & 0x7FFFFFFFFFFFFFFFull) == 0) bits = 0; /* -0.0 -> +0.0 */
      if ((bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
          (bits & 0x000FFFFFFFFFFFFFull) != 0) {
        bits = 0x7FF8000000000000ull; /* any NaN -> one NaN */
      }
      put_u64(&d->logical, bits);
      break;
    }
    case DIG_BOOL: {
      uint8_t v = (uint8_t)(data ? get_bit(data, p) : 0);
      abi_sha256_update(&d->logical, &v, 1);
      break;
    }
    case DIG_UTF8: {
      int32_t from = load_i32(offsets, p);
      int32_t to = load_i32(offsets, p + 1);
      /*
       * Bounded against the values buffer, not just checked for monotonicity:
       * without the upper bound a consumer's bad offset pair would have this
       * code read past the buffer rather than report the array as invalid.
       */
      if (from < 0 || to < from || (uint64_t)to > values_len) {
        return dig_fail(d, ABI_ERR_INVALID_ARGUMENT,
                        "utf8 offsets at slot %lld are not a valid range into "
                        "%llu values bytes (%ld, %ld)",
                        (long long)p, (unsigned long long)values_len,
                        (long)from, (long)to);
      }
      /* Raw bytes, no Unicode normalization: two different encodings of the
         same text are different data, and deciding otherwise is semdiff's job.
       */
      put_bytes(&d->logical, data ? data + from : NULL, (uint64_t)(to - from));
      break;
    }
    case DIG_STRUCT: break;
    }
  }
  return ABI_OK;
}

static AbiStatus dig_node(DigCtx *d, const struct ArrowSchema *s,
                          const struct ArrowArray *a, int64_t extra_offset,
                          int64_t count, uint32_t depth) {
  DigType   type;
  int64_t   want_buffers, i, base;
  uint64_t  values_len = 0;
  AbiStatus st;

  if (!s || !a) {
    return dig_fail(d, ABI_ERR_INVALID_ARGUMENT, "null schema or array node");
  }
  if (depth > ABI_LIMIT_TREE_DEPTH) {
    return dig_fail(d, ABI_ERR_LIMIT_EXCEEDED, "tree depth exceeds %u",
                    (unsigned)ABI_LIMIT_TREE_DEPTH);
  }
  if (++d->nodes > ABI_LIMIT_TREE_NODES) {
    return dig_fail(d, ABI_ERR_LIMIT_EXCEEDED, "node count exceeds %u",
                    (unsigned)ABI_LIMIT_TREE_NODES);
  }
  if (s->dictionary || a->dictionary) {
    return dig_fail(d, ABI_ERR_INVALID_ARGUMENT,
                    "dictionary-encoded arrays are M3 and not digested");
  }

  st = dig_type_of(d, s->format, &type, &want_buffers);
  if (st != ABI_OK) return st;

  if (a->length < 0 || a->offset < 0) {
    return dig_fail(d, ABI_ERR_INVALID_ARGUMENT,
                    "negative length (%lld) or offset (%lld)",
                    (long long)a->length, (long long)a->offset);
  }
  if (a->n_buffers < want_buffers) {
    return dig_fail(d, ABI_ERR_INVALID_ARGUMENT,
                    "\"%s\" needs %lld buffers, the array declares %lld",
                    s->format, (long long)want_buffers,
                    (long long)a->n_buffers);
  }
  if (want_buffers > 0 && !a->buffers) {
    return dig_fail(d, ABI_ERR_INVALID_ARGUMENT, "buffer array is null");
  }
  base = a->offset + extra_offset;
  /*
   * Containment, for the same reason validate.c enforces it on a case: a window
   * running past its own array would make *this* code perform the out-of-bounds
   * read, and the resulting fault would be reported against whoever handed the
   * array over. A struct child shorter than its parent requires is an invalid
   * array, and saying so is the correct answer.
   */
  if (base < 0 || count < 0 || extra_offset < 0 ||
      extra_offset + count > a->length) {
    return dig_fail(d, ABI_ERR_INVALID_ARGUMENT,
                    "window [%lld,+%lld) escapes an array of length %lld at "
                    "offset %lld",
                    (long long)base, (long long)count, (long long)a->length,
                    (long long)a->offset);
  }

  /* --- physical: the schema as declared, then the bytes as laid out ------ */
  put_cstr(&d->physical, s->format);
  put_cstr(&d->physical, s->name);
  put_i64(&d->physical, s->flags);
  put_i64(&d->physical, a->length);
  put_i64(&d->physical, a->null_count);
  put_i64(&d->physical, a->offset);
  put_i64(&d->physical, a->n_buffers);
  put_i64(&d->physical, a->n_children);
  for (i = 0; i < want_buffers; i++) {
    uint64_t len = 0;
    uint8_t  present = (uint8_t)(a->buffers[i] ? 1 : 0);
    abi_sha256_update(&d->physical, &present, 1);
    if (!present) continue;
    st = dig_buffer_len(d, type, a, i, &len);
    if (st != ABI_OK) return st;
    if (type == DIG_UTF8 && i == 2) values_len = len;
    put_bytes(&d->physical, a->buffers[i], len);
  }

  /* --- logical: the type and the values, with the offset factored out ---- */
  put_cstr(&d->logical, s->format);
  put_i64(&d->logical, count);
  st = dig_values(d, type, a, base, count, values_len);
  if (st != ABI_OK) return st;

  /* --- children --------------------------------------------------------- */
  if (s->n_children != a->n_children) {
    return dig_fail(d, ABI_ERR_INVALID_ARGUMENT,
                    "schema declares %lld children, array declares %lld",
                    (long long)s->n_children, (long long)a->n_children);
  }
  put_i64(&d->logical, a->n_children);
  for (i = 0; i < a->n_children; i++) {
    if (!s->children || !a->children) {
      return dig_fail(d, ABI_ERR_INVALID_ARGUMENT, "child array is null");
    }
    /*
     * A struct's children are indexed by the parent's absolute position, so the
     * parent's window travels down rather than being applied to the parent's
     * buffers alone. A child of a struct sliced at offset 3 contributes its own
     * offset plus 3.
     */
    st = dig_node(d, s->children[i], a->children[i], base, count, depth + 1);
    if (st != ABI_OK) return st;
  }
  return ABI_OK;
}

AbiStatus abi_digest(const struct ArrowSchema *schema,
                     const struct ArrowArray *array, AbiDigest *out,
                     AbiError *err) {
  DigCtx    d;
  uint8_t   full[32];
  AbiStatus st;

  if (!schema || !array || !out) return ABI_ERR_INVALID_ARGUMENT;
  if (err) {
    err->status = ABI_OK;
    err->offset = 0;
    err->message[0] = '\0';
  }
  d.err = err;
  d.nodes = 0;
  abi_sha256_init(&d.physical);
  abi_sha256_init(&d.logical);

  /*
   * Domain separation and the version, in both. Two digests of different kinds
   * must never collide, and a digest computed under an older rule set must
   * never compare equal to one computed under the current rules.
   */
  put_bytes(&d.physical, "abi-digest/physical", 19);
  put_u64(&d.physical, ABI_DIGEST_VER);
  put_bytes(&d.logical, "abi-digest/logical", 18);
  put_u64(&d.logical, ABI_DIGEST_VER);

  st = dig_node(&d, schema, array, 0, array->length, 0);
  if (st != ABI_OK) return st;

  abi_sha256_final(&d.physical, full);
  memcpy(out->physical, full, ABI_DIGEST_BYTES);
  abi_sha256_final(&d.logical, full);
  memcpy(out->logical, full, ABI_DIGEST_BYTES);
  return ABI_OK;
}

void abi_digest_hex(const uint8_t digest[ABI_DIGEST_BYTES], char *out) {
  abi_hex(digest, ABI_DIGEST_BYTES, out);
}
