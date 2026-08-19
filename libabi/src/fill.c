/*
 * Canonical allocation payload encoding -- docs/abicase-format.md 5.2.
 *
 * Encoder and decoder both call abi_fill_choose(). The encoder uses it to pick
 * the encoding; the decoder re-runs it over the bytes it just decoded and
 * rejects the file if a different encoding would have been chosen. That is what
 * lets the format be strictly canonical without trusting the writer, and it is
 * what makes encode(decode(b)) == b provable rather than hoped for.
 */
#include "abi_internal.h"

/*
 * s is periodic with period p exactly when s[0 .. size-p) == s[p .. size).
 * Phrasing it as one memcmp keeps the divisor scan cheap: the comparison is
 * vectorized and bails at the first mismatch, which is the common case.
 */
static int abi_is_periodic(const uint8_t *b, uint64_t size, uint64_t p) {
  return memcmp(b, b + p, (size_t)(size - p)) == 0;
}

static uint64_t abi_rle_run_count(const uint8_t *b, uint64_t size) {
  uint64_t runs = 0, i = 0;
  while (i < size) {
    uint64_t j = i + 1;
    while (j < size && b[j] == b[i]) j++;
    runs++;
    i = j;
  }
  return runs;
}

/* Smallest period in [1, min(256, size/2)] that divides size, or 0. */
static uint32_t abi_smallest_period(const uint8_t *b, uint64_t size) {
  uint64_t max_p = size / 2;
  uint64_t p;
  if (max_p > ABI_LIMIT_PATTERN_PERIOD) max_p = ABI_LIMIT_PATTERN_PERIOD;
  for (p = 1; p <= max_p; p++) {
    if (size % p != 0) continue;
    if (abi_is_periodic(b, size, p)) return (uint32_t)p;
  }
  return 0;
}

AbiFill abi_fill_choose(const uint8_t *bytes, uint64_t size,
                        uint64_t *out_payload_size, uint32_t *out_period,
                        uint32_t *out_run_count) {
  uint64_t best_size, runs, rle_size, pattern_size;
  AbiFill  best;
  uint32_t period;
  uint64_t i;
  int      all_zero = 1;

  if (out_period) *out_period = 0;
  if (out_run_count) *out_run_count = 0;

  /* ZERO carries no payload, so nothing can beat it and nothing ties it. */
  for (i = 0; i < size; i++) {
    if (bytes[i] != 0) {
      all_zero = 0;
      break;
    }
  }
  if (all_zero) {
    if (out_payload_size) *out_payload_size = 0;
    return ABI_FILL_ZERO;
  }

  /* RAW is code 0, so it wins every tie. */
  best = ABI_FILL_RAW;
  best_size = size;

  runs = abi_rle_run_count(bytes, size);
  rle_size = 4u + runs * 5u;
  if (rle_size < best_size) {
    best = ABI_FILL_RLE;
    best_size = rle_size;
  }

  period = abi_smallest_period(bytes, size);
  if (period) {
    pattern_size = 4u + (uint64_t)period;
    if (pattern_size < best_size) {
      best = ABI_FILL_PATTERN;
      best_size = pattern_size;
    }
  }

  if (out_payload_size) *out_payload_size = best_size;
  if (out_period) *out_period = (best == ABI_FILL_PATTERN) ? period : 0;
  if (out_run_count) {
    *out_run_count = (best == ABI_FILL_RLE) ? (uint32_t)runs : 0;
  }
  return best;
}

int abi_fill_write(AbiBuf *out, const uint8_t *bytes, uint64_t size,
                   AbiFill fill, uint32_t period, uint32_t run_count) {
  uint64_t i;

  switch (fill) {
    case ABI_FILL_ZERO:
      return 1;

    case ABI_FILL_RAW:
      return abi_buf_put(out, bytes, (size_t)size);

    case ABI_FILL_PATTERN:
      if (!abi_buf_u32(out, period)) return 0;
      return abi_buf_put(out, bytes, period);

    case ABI_FILL_RLE: {
      if (!abi_buf_u32(out, run_count)) return 0;
      i = 0;
      while (i < size) {
        uint64_t j = i + 1;
        while (j < size && bytes[j] == bytes[i]) j++;
        if (!abi_buf_u32(out, (uint32_t)(j - i))) return 0;
        if (!abi_buf_u8(out, bytes[i])) return 0;
        i = j;
      }
      return 1;
    }

    default:
      return 0;
  }
}

/*
 * Materializes an allocation payload into `arena` and validates it. Structural
 * rules first (they bound the work), canonicality last.
 */
int abi_fill_read(AbiCur *c, AbiArena *arena, uint64_t size, uint8_t fill,
                  const uint8_t **out_bytes) {
  size_t   at = c->pos;
  uint8_t *dst;
  uint64_t canon_payload = 0;
  uint32_t canon_period = 0, canon_runs = 0;
  uint32_t declared_period = 0, declared_runs = 0;
  AbiFill  canon;

  if (fill > ABI_FILL_PATTERN) {
    return abi_cur_fail(c, ABI_ERR_BAD_ENUM, at, "unknown fill code %u",
                        (unsigned)fill);
  }

  dst = (uint8_t *)abi_arena_alloc(arena, (size_t)size, 1);
  if (!dst) return abi_cur_fail(c, ABI_ERR_NO_MEMORY, at, "out of memory");
  if (size) memset(dst, 0, (size_t)size);

  switch (fill) {
    case ABI_FILL_ZERO:
      break; /* already zeroed */

    case ABI_FILL_RAW: {
      const uint8_t *raw = NULL;
      if (!abi_cur_raw(c, size, &raw)) return 0;
      if (size) memcpy(dst, raw, (size_t)size);
      break;
    }

    case ABI_FILL_PATTERN: {
      size_t         pat_at = c->pos;
      uint32_t       period = abi_cur_u32(c);
      const uint8_t *pat = NULL;
      uint64_t       max_p = size / 2;
      uint64_t       off;

      if (c->failed) return 0;
      if (max_p > ABI_LIMIT_PATTERN_PERIOD) max_p = ABI_LIMIT_PATTERN_PERIOD;
      if (period == 0 || (uint64_t)period > max_p || size % period != 0) {
        return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, pat_at,
                            "PATTERN period %lu invalid for size %llu",
                            (unsigned long)period, (unsigned long long)size);
      }
      if (!abi_cur_raw(c, period, &pat)) return 0;
      for (off = 0; off < size; off += period) memcpy(dst + off, pat, period);
      declared_period = period;
      break;
    }

    case ABI_FILL_RLE: {
      size_t   rc_at = c->pos;
      uint32_t run_count = abi_cur_u32(c);
      uint32_t r;
      uint64_t written = 0;
      int      have_prev = 0;
      uint8_t  prev = 0;

      if (c->failed) return 0;
      /* Each run costs 5 bytes and covers >= 1 byte: both bound run_count. */
      if ((uint64_t)run_count > size) {
        return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, rc_at,
                            "RLE run_count %lu exceeds size %llu",
                            (unsigned long)run_count, (unsigned long long)size);
      }
      for (r = 0; r < run_count; r++) {
        size_t   run_at = c->pos;
        uint32_t run_len = abi_cur_u32(c);
        uint8_t  value = abi_cur_u8(c);
        if (c->failed) return 0;
        if (run_len == 0) {
          return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, run_at,
                              "RLE run length must be non-zero");
        }
        if ((uint64_t)run_len > size - written) {
          return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, run_at,
                              "RLE runs overrun size %llu",
                              (unsigned long long)size);
        }
        if (have_prev && value == prev) {
          /* Two encodings for one payload; the maximal-run form is the only
             canonical one. */
          return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, run_at,
                              "adjacent RLE runs share value 0x%02x",
                              (unsigned)value);
        }
        memset(dst + written, value, run_len);
        written += run_len;
        prev = value;
        have_prev = 1;
      }
      if (written != size) {
        return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, at,
                            "RLE runs cover %llu of %llu bytes",
                            (unsigned long long)written,
                            (unsigned long long)size);
      }
      declared_runs = run_count;
      break;
    }

    default:
      return abi_cur_fail(c, ABI_ERR_BAD_ENUM, at, "unknown fill code %u",
                          (unsigned)fill);
  }

  canon = abi_fill_choose(dst, size, &canon_payload, &canon_period, &canon_runs);
  if ((uint8_t)canon != fill) {
    return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, at,
                        "allocation uses fill %u but canonical encoding is %u",
                        (unsigned)fill, (unsigned)canon);
  }
  /*
   * The fill code alone does not pin the bytes down. "abababab" is periodic
   * with period 2 and with period 4, and both pass the structural rules above,
   * so a file could declare the non-minimal one, decode correctly, and still
   * re-encode to different bytes. Comparing the parameters closes that.
   */
  if (canon_period != declared_period) {
    return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, at,
                        "PATTERN period %lu is not the smallest period %lu",
                        (unsigned long)declared_period,
                        (unsigned long)canon_period);
  }
  if (canon_runs != declared_runs) {
    return abi_cur_fail(c, ABI_ERR_NOT_CANONICAL, at,
                        "RLE run_count %lu is not the maximal-run count %lu",
                        (unsigned long)declared_runs, (unsigned long)canon_runs);
  }
  (void)canon_payload;

  *out_bytes = size ? dst : NULL;
  return 1;
}
