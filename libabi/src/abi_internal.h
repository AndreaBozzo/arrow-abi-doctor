/*
 * libabi internals: arena, byte primitives, growable buffer, parse cursor.
 * Not installed; not part of the public API.
 */
#ifndef ABI_INTERNAL_H
#define ABI_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "abi/abicase.h"

/* --- section tags (docs/abicase-format.md 3) ----------------------------- */

#define ABICASE_SECTION_VERSION 1u

#define ABICASE_TAG_PROVENANCE  0x0001u
#define ABICASE_TAG_ALLOCATIONS 0x0002u
#define ABICASE_TAG_SCHEMA      0x0003u
#define ABICASE_TAG_ARRAY       0x0004u
#define ABICASE_TAG_CALLSEQ     0x0005u
#define ABICASE_TAG_EXPECTED    0x0006u

/* --- arena --------------------------------------------------------------- */

/*
 * Bump allocator with a chunk list. Everything reachable from an AbiCase lives
 * here, so releasing a case is one free and cannot leak a partially-built tree
 * -- which matters more than usual, because this tool's own memory behaviour is
 * what it reports about other people's.
 *
 * There is no individual free and no realloc: growable vectors reallocate by
 * taking a larger block and copying, wasting at most the previous block. For
 * objects bounded by ABI_LIMIT_* that is a fixed, small cost.
 */
struct AbiArena;
typedef struct AbiArena AbiArena;

AbiArena *abi_arena_new(void);
void      abi_arena_free(AbiArena *a);
void     *abi_arena_alloc(AbiArena *a, size_t size, size_t align);
void     *abi_arena_calloc(AbiArena *a, size_t size, size_t align);
void     *abi_arena_dup(AbiArena *a, const void *src, size_t size, size_t align);
size_t    abi_arena_bytes_used(const AbiArena *a);

#define ABI_ARENA_NEW(a, T) ((T *)abi_arena_calloc((a), sizeof(T), _Alignof(T)))
#define ABI_ARENA_ARRAY(a, T, n) \
  ((T *)abi_arena_calloc((a), sizeof(T) * (size_t)(n), _Alignof(T)))

/* Copies into the arena; NULL src with size 0 yields an empty AbiBytes. */
int abi_arena_bytes(AbiArena *a, AbiBytes *out, const void *src, uint32_t size);
int abi_arena_cstr(AbiArena *a, AbiBytes *out, const char *s);

/* --- endian-explicit primitives ------------------------------------------ */

/*
 * Little-endian, on every host, by construction: these are written with shifts
 * over unsigned char, so there is no #ifdef and nothing to get wrong on a
 * big-endian target. Every integer that reaches a .abicase file goes through
 * here. A memcpy of a native integer into the output buffer would compile and
 * pass every little-endian test, and is the exact defect the s390x round-trip
 * test exists to catch.
 */

static inline void abi_store_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void abi_store_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void abi_store_u64(uint8_t *p, uint64_t v) {
  int i;
  for (i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
}

static inline uint16_t abi_load_u16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t abi_load_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline uint64_t abi_load_u64(const uint8_t *p) {
  uint64_t v = 0;
  int      i;
  for (i = 7; i >= 0; i--) v = (v << 8) | (uint64_t)p[i];
  return v;
}

/*
 * Two's-complement conversions written out rather than cast, so that the
 * mapping is defined by this file and not by the implementation. Signed
 * overflow on conversion is implementation-defined before C23; being casual
 * about it in the one library whose job is byte-level fidelity would be a poor
 * look.
 */
static inline uint64_t abi_i64_to_u64(int64_t v) {
  return (v < 0) ? (~(uint64_t)(-(v + 1))) : (uint64_t)v;
}

static inline int64_t abi_u64_to_i64(uint64_t u) {
  return (u <= (uint64_t)INT64_MAX) ? (int64_t)u
                                    : -(int64_t)(UINT64_MAX - u) - 1;
}

/* --- growable output buffer ---------------------------------------------- */

typedef struct {
  uint8_t *data;
  size_t   size;
  size_t   capacity;
  int      failed; /* sticky: allocation failure */
} AbiBuf;

void abi_buf_init(AbiBuf *b);
void abi_buf_reset(AbiBuf *b);
int  abi_buf_put(AbiBuf *b, const void *src, size_t n);
int  abi_buf_u8(AbiBuf *b, uint8_t v);
int  abi_buf_u16(AbiBuf *b, uint16_t v);
int  abi_buf_u32(AbiBuf *b, uint32_t v);
int  abi_buf_u64(AbiBuf *b, uint64_t v);
int  abi_buf_i64(AbiBuf *b, int64_t v);
int  abi_buf_zeros(AbiBuf *b, size_t n);
int  abi_buf_bytes(AbiBuf *b, AbiBytes s); /* u32 length + payload */

/* --- parse cursor -------------------------------------------------------- */

/*
 * Sticky-error cursor: the first failure latches and every later read becomes a
 * no-op returning zero. Callers check `failed` at section boundaries instead of
 * after each field, which keeps the decoder readable without the usual risk of
 * an unchecked read -- there is no path that reads out of bounds, because the
 * bounds check happens inside every accessor.
 */
typedef struct {
  const uint8_t *data;
  size_t         size;
  size_t         pos;
  int            failed;
  AbiError       err;
} AbiCur;

void abi_cur_init(AbiCur *c, const uint8_t *data, size_t size);

/* Always returns 0, so callers can `return abi_cur_fail(...)`. */
int abi_cur_fail(AbiCur *c, AbiStatus status, size_t offset, const char *fmt, ...);

int      abi_cur_need(AbiCur *c, uint64_t n);
uint8_t  abi_cur_u8(AbiCur *c);
uint16_t abi_cur_u16(AbiCur *c);
uint32_t abi_cur_u32(AbiCur *c);
uint64_t abi_cur_u64(AbiCur *c);
int64_t  abi_cur_i64(AbiCur *c);
/* Borrows into the input buffer; copy before the input goes away. */
int      abi_cur_raw(AbiCur *c, uint64_t n, const uint8_t **out);
/* u32 length + payload, length-limited, copied into `arena`. */
int      abi_cur_bytes(AbiCur *c, AbiArena *arena, AbiBytes *out);
int      abi_cur_zeros(AbiCur *c, size_t n, const char *field);
int      abi_cur_bool(AbiCur *c, const char *field, uint8_t *out);

/* --- allocation payload encoding (fill.c, format 5.2) -------------------- */

int abi_fill_write(AbiBuf *out, const uint8_t *bytes, uint64_t size,
                   AbiFill fill, uint32_t period, uint32_t run_count);
/* Materializes and fully validates one allocation payload, canonicality
   included. Borrowed bytes live in `arena`. */
int abi_fill_read(AbiCur *c, AbiArena *arena, uint64_t size, uint8_t fill,
                  const uint8_t **out_bytes);

/* --- semantic validation (validate.c) ------------------------------------ */

/*
 * The structural and class rules, in one place, called by BOTH the encoder
 * (before emitting) and the decoder (after parsing). Sharing it is what makes
 * "the encoder never emits a file its own decoder rejects" a property of the
 * code rather than a habit -- and it is the reason a B1 case cannot smuggle an
 * out-of-bounds view past the writer.
 */
AbiStatus abi_case_validate(const AbiCase *c, AbiError *err);

/* --- sha-256 ------------------------------------------------------------- */

typedef struct {
  uint64_t length;
  uint32_t state[8];
  uint8_t  block[64];
  size_t   block_used;
} AbiSha256;

void abi_sha256_init(AbiSha256 *s);
void abi_sha256_update(AbiSha256 *s, const void *data, size_t size);
void abi_sha256_final(AbiSha256 *s, uint8_t out[32]);
void abi_sha256(const void *data, size_t size, uint8_t out[32]);
void abi_hex(const uint8_t *bytes, size_t n, char *out_hex);

#endif /* ABI_INTERNAL_H */
