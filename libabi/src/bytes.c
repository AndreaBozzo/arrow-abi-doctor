#include <stdio.h>
#include <stdlib.h>

#include "abi_internal.h"

/* --- growable output buffer ---------------------------------------------- */

void abi_buf_init(AbiBuf *b) {
  b->data = NULL;
  b->size = 0;
  b->capacity = 0;
  b->failed = 0;
}

void abi_buf_reset(AbiBuf *b) {
  free(b->data);
  abi_buf_init(b);
}

static int abi_buf_reserve(AbiBuf *b, size_t extra) {
  size_t   need, cap;
  uint8_t *p;

  if (b->failed) return 0;
  if (extra > SIZE_MAX - b->size) {
    b->failed = 1;
    return 0;
  }
  need = b->size + extra;
  if (need <= b->capacity) return 1;

  cap = b->capacity ? b->capacity : 256;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) {
      b->failed = 1;
      return 0;
    }
    cap *= 2;
  }
  p = (uint8_t *)realloc(b->data, cap);
  if (!p) {
    b->failed = 1;
    return 0;
  }
  b->data = p;
  b->capacity = cap;
  return 1;
}

int abi_buf_put(AbiBuf *b, const void *src, size_t n) {
  if (!abi_buf_reserve(b, n)) return 0;
  if (n) memcpy(b->data + b->size, src, n);
  b->size += n;
  return 1;
}

int abi_buf_u8(AbiBuf *b, uint8_t v) {
  if (!abi_buf_reserve(b, 1)) return 0;
  b->data[b->size++] = v;
  return 1;
}

int abi_buf_u16(AbiBuf *b, uint16_t v) {
  if (!abi_buf_reserve(b, 2)) return 0;
  abi_store_u16(b->data + b->size, v);
  b->size += 2;
  return 1;
}

int abi_buf_u32(AbiBuf *b, uint32_t v) {
  if (!abi_buf_reserve(b, 4)) return 0;
  abi_store_u32(b->data + b->size, v);
  b->size += 4;
  return 1;
}

int abi_buf_u64(AbiBuf *b, uint64_t v) {
  if (!abi_buf_reserve(b, 8)) return 0;
  abi_store_u64(b->data + b->size, v);
  b->size += 8;
  return 1;
}

int abi_buf_i64(AbiBuf *b, int64_t v) {
  return abi_buf_u64(b, abi_i64_to_u64(v));
}

int abi_buf_zeros(AbiBuf *b, size_t n) {
  if (!abi_buf_reserve(b, n)) return 0;
  if (n) memset(b->data + b->size, 0, n);
  b->size += n;
  return 1;
}

int abi_buf_bytes(AbiBuf *b, AbiBytes s) {
  if (!abi_buf_u32(b, s.size)) return 0;
  return abi_buf_put(b, s.data, s.size);
}

/* --- parse cursor -------------------------------------------------------- */

void abi_cur_init(AbiCur *c, const uint8_t *data, size_t size) {
  c->data = data;
  c->size = size;
  c->pos = 0;
  c->failed = 0;
  c->err.status = ABI_OK;
  c->err.offset = 0;
  c->err.message[0] = '\0';
}

int abi_cur_fail(AbiCur *c, AbiStatus status, size_t offset, const char *fmt,
                 ...) {
  va_list ap;
  if (!c->failed) { /* keep the first failure: it is the informative one */
    c->failed = 1;
    c->err.status = status;
    c->err.offset = offset;
    va_start(ap, fmt);
    vsnprintf(c->err.message, sizeof(c->err.message), fmt, ap);
    va_end(ap);
  }
  return 0;
}

int abi_cur_need(AbiCur *c, uint64_t n) {
  if (c->failed) return 0;
  if (n > (uint64_t)(c->size - c->pos)) {
    return abi_cur_fail(
        c, ABI_ERR_TRUNCATED, c->pos, "need %llu byte(s), %llu remaining",
        (unsigned long long)n, (unsigned long long)(c->size - c->pos));
  }
  return 1;
}

uint8_t abi_cur_u8(AbiCur *c) {
  if (!abi_cur_need(c, 1)) return 0;
  return c->data[c->pos++];
}

uint16_t abi_cur_u16(AbiCur *c) {
  uint16_t v;
  if (!abi_cur_need(c, 2)) return 0;
  v = abi_load_u16(c->data + c->pos);
  c->pos += 2;
  return v;
}

uint32_t abi_cur_u32(AbiCur *c) {
  uint32_t v;
  if (!abi_cur_need(c, 4)) return 0;
  v = abi_load_u32(c->data + c->pos);
  c->pos += 4;
  return v;
}

uint64_t abi_cur_u64(AbiCur *c) {
  uint64_t v;
  if (!abi_cur_need(c, 8)) return 0;
  v = abi_load_u64(c->data + c->pos);
  c->pos += 8;
  return v;
}

int64_t abi_cur_i64(AbiCur *c) { return abi_u64_to_i64(abi_cur_u64(c)); }

int abi_cur_raw(AbiCur *c, uint64_t n, const uint8_t **out) {
  if (!abi_cur_need(c, n)) return 0;
  *out = c->data + c->pos;
  c->pos += (size_t)n;
  return 1;
}

int abi_cur_bytes(AbiCur *c, AbiArena *arena, AbiBytes *out) {
  size_t         at = c->pos;
  uint32_t       len;
  const uint8_t *raw = NULL;

  len = abi_cur_u32(c);
  if (c->failed) return 0;
  if (len > ABI_LIMIT_BYTES_LEN) {
    return abi_cur_fail(c, ABI_ERR_LIMIT_EXCEEDED, at,
                        "byte string of %lu exceeds limit %lu",
                        (unsigned long)len, (unsigned long)ABI_LIMIT_BYTES_LEN);
  }
  if (!abi_cur_raw(c, len, &raw)) return 0;
  if (!abi_arena_bytes(arena, out, raw, len)) {
    return abi_cur_fail(c, ABI_ERR_NO_MEMORY, at, "out of memory");
  }
  return 1;
}

int abi_cur_zeros(AbiCur *c, size_t n, const char *field) {
  size_t         at = c->pos;
  const uint8_t *raw = NULL;
  size_t         i;

  if (!abi_cur_raw(c, n, &raw)) return 0;
  for (i = 0; i < n; i++) {
    if (raw[i] != 0) {
      /*
       * Reserved bytes must be zero on read, not merely on write. Accepting a
       * non-zero value here would give one case two valid encodings and quietly
       * break the round-trip guarantee.
       */
      return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, at + i,
                          "reserved field '%s' must be zero, found 0x%02x",
                          field, (unsigned)raw[i]);
    }
  }
  return 1;
}

int abi_cur_bool(AbiCur *c, const char *field, uint8_t *out) {
  size_t  at = c->pos;
  uint8_t v = abi_cur_u8(c);
  if (c->failed) return 0;
  if (v > 1) {
    return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, at,
                        "boolean field '%s' must be 0 or 1, found %u", field,
                        (unsigned)v);
  }
  *out = v;
  return 1;
}
