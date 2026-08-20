/*
 * libabi test suite -- M0.
 *
 * The properties under test are the ones M0 promises:
 *   - byte-identical round trip on one host
 *   - aliasing and topology preserved across encode/decode
 *   - a strictly canonical decoder: no input has two encodings
 *   - no crash, no out-of-bounds read on truncated or corrupted input
 *
 * The last one is only meaningful under a sanitizer. Run the ASan/UBSan build
 * (cmake -DABI_SANITIZERS=ON) before believing it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi_internal.h"
#include "fixture.h"

static int         g_checks = 0;
static int         g_fails = 0;
static const char *g_test = "?";

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_fails++;                                                               \
      fprintf(stderr, "FAIL [%s] %s:%d: %s\n", g_test, __FILE__, __LINE__,     \
              (msg));                                                          \
    }                                                                          \
  } while (0)

#define CHECKF(cond, fmt, ...)                                                 \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_fails++;                                                               \
      fprintf(stderr, "FAIL [%s] %s:%d: " fmt "\n", g_test, __FILE__,          \
              __LINE__, __VA_ARGS__);                                          \
    }                                                                          \
  } while (0)

#define RUN(fn)                                                                \
  do {                                                                         \
    g_test = #fn;                                                              \
    fn();                                                                      \
  } while (0)

/* --- primitives ---------------------------------------------------------- */

static void test_endian_primitives(void) {
  uint8_t b[8];
  int     i;

  abi_store_u16(b, 0x1234u);
  CHECK(b[0] == 0x34 && b[1] == 0x12, "u16 must serialize little-endian");

  abi_store_u32(b, 0x0000FEFFu);
  CHECK(b[0] == 0xFF && b[1] == 0xFE && b[2] == 0x00 && b[3] == 0x00,
        "the byte-order marker must be FF FE 00 00 on the wire");

  abi_store_u64(b, 0x0102030405060708ull);
  for (i = 0; i < 8; i++) {
    CHECKF(b[i] == (uint8_t)(8 - i), "u64 byte %d", i);
  }
  CHECK(abi_load_u64(b) == 0x0102030405060708ull, "u64 load must invert store");
  CHECK(abi_load_u32(b) == 0x05060708u, "u32 load");
  CHECK(abi_load_u16(b) == 0x0708u, "u16 load");

  /* two's-complement mapping is defined by us, not by the compiler */
  CHECK(abi_i64_to_u64(-1) == 0xFFFFFFFFFFFFFFFFull, "-1 maps to all ones");
  CHECK(abi_u64_to_i64(0xFFFFFFFFFFFFFFFFull) == -1, "all ones maps to -1");
  CHECK(abi_i64_to_u64(INT64_MIN) == 0x8000000000000000ull, "INT64_MIN maps");
  CHECK(abi_u64_to_i64(0x8000000000000000ull) == INT64_MIN,
        "INT64_MIN inverts");
  CHECK(abi_u64_to_i64(abi_i64_to_u64(-1234567890123LL)) == -1234567890123LL,
        "negative round trip");
}

static void test_sha256_vectors(void) {
  uint8_t d[32];
  char    hex[65];

  abi_sha256("", 0, d);
  abi_hex(d, 32, hex);
  CHECKF(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca4959"
                     "91b7852b855") == 0,
         "SHA-256(\"\") = %s", hex);

  abi_sha256("abc", 3, d);
  abi_hex(d, 32, hex);
  CHECKF(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410f"
                     "f61f20015ad") == 0,
         "SHA-256(\"abc\") = %s", hex);

  /* crosses the 56-byte padding boundary and a whole block */
  {
    uint8_t big[200];
    size_t  i;
    for (i = 0; i < sizeof(big); i++)
      big[i] = (uint8_t)(i & 0xFF);
    abi_sha256(big, sizeof(big), d);
    abi_hex(d, 32, hex);
    /* self-consistency: streaming in odd chunks must equal the one-shot */
    {
      AbiSha256 s;
      uint8_t   d2[32];
      char      hex2[65];
      size_t    off = 0, step = 7;
      abi_sha256_init(&s);
      while (off < sizeof(big)) {
        size_t n = (sizeof(big) - off < step) ? (sizeof(big) - off) : step;
        abi_sha256_update(&s, big + off, n);
        off += n;
      }
      abi_sha256_final(&s, d2);
      abi_hex(d2, 32, hex2);
      CHECKF(strcmp(hex, hex2) == 0, "streamed %s != one-shot %s", hex2, hex);
    }
  }
}

/* --- canonical fill selection -------------------------------------------- */

static void check_fill(const uint8_t *b, uint64_t n, AbiFill want,
                       const char *what) {
  uint64_t payload = 0;
  uint32_t period = 0, runs = 0;
  AbiFill  got = abi_fill_choose(b, n, &payload, &period, &runs);
  CHECKF(got == want, "%s: chose fill %d, expected %d", what, (int)got,
         (int)want);
}

static void test_fill_choose(void) {
  uint8_t  zeros[64];
  uint8_t  ones[16];
  uint8_t  alt[16];
  uint8_t  raw[8];
  uint8_t  runs[100];
  uint64_t payload = 0;
  uint32_t period = 0, run_count = 0;
  size_t   i;

  memset(zeros, 0, sizeof(zeros));
  memset(ones, 0xFF, sizeof(ones));
  for (i = 0; i < sizeof(alt); i++)
    alt[i] = (uint8_t)((i % 2) ? 0xCD : 0xAB);
  for (i = 0; i < sizeof(raw); i++)
    raw[i] = (uint8_t)(0x10 + i * 37);
  for (i = 0; i < sizeof(runs); i++)
    runs[i] = (uint8_t)(i < 50 ? 0x01 : 0x02);

  check_fill(NULL, 0, ABI_FILL_ZERO, "empty allocation");
  check_fill(zeros, sizeof(zeros), ABI_FILL_ZERO, "all zero");

  CHECK(abi_fill_choose(ones, sizeof(ones), &payload, &period, &run_count) ==
            ABI_FILL_PATTERN,
        "0xFF bitmap should be PATTERN");
  CHECKF(period == 1, "0xFF bitmap period = %lu, expected 1",
         (unsigned long)period);
  CHECKF(payload == 5, "0xFF bitmap payload = %llu, expected 5",
         (unsigned long long)payload);

  CHECK(abi_fill_choose(alt, sizeof(alt), &payload, &period, &run_count) ==
            ABI_FILL_PATTERN,
        "alternating bytes should be PATTERN");
  CHECKF(period == 2, "alternating period = %lu, expected 2",
         (unsigned long)period);

  check_fill(raw, sizeof(raw), ABI_FILL_RAW, "incompressible short buffer");

  CHECK(abi_fill_choose(runs, sizeof(runs), &payload, &period, &run_count) ==
            ABI_FILL_RLE,
        "two long runs should be RLE");
  CHECKF(run_count == 2, "run_count = %lu, expected 2",
         (unsigned long)run_count);
  CHECKF(payload == 14, "RLE payload = %llu, expected 14",
         (unsigned long long)payload);
}

/* --- case construction --------------------------------------------------- */

/*
 * One case exercising every feature the format claims to carry: aliasing, an
 * intentionally misaligned view, NULL buffers, a dictionary, absent vs empty
 * names, metadata with an embedded NUL, and all four fill encodings.
 */
/* --- round trip ---------------------------------------------------------- */

static void roundtrip_case(AbiCase *c, const char *what) {
  uint8_t  *b1 = NULL, *b2 = NULL;
  size_t    n1 = 0, n2 = 0;
  AbiCase  *back = NULL;
  AbiError  err;
  AbiStatus st;

  st = abi_case_encode(c, &b1, &n1);
  CHECKF(st == ABI_OK, "%s: encode failed: %s", what, abi_status_str(st));
  if (st != ABI_OK) return;

  st = abi_case_decode(b1, n1, &back, &err);
  CHECKF(st == ABI_OK, "%s: decode failed: %s (%s at +%llu)", what,
         abi_status_str(st), err.message, (unsigned long long)err.offset);
  if (st != ABI_OK) {
    abi_free(b1);
    return;
  }

  st = abi_case_encode(back, &b2, &n2);
  CHECKF(st == ABI_OK, "%s: re-encode failed: %s", what, abi_status_str(st));
  if (st == ABI_OK) {
    CHECKF(n1 == n2, "%s: size %llu -> %llu", what, (unsigned long long)n1,
           (unsigned long long)n2);
    CHECKF(n1 == n2 && memcmp(b1, b2, n1) == 0,
           "%s: round trip is not byte-identical", what);
    abi_free(b2);
  }
  abi_case_free(back);
  abi_free(b1);
}

static void test_roundtrip(void) {
  AbiCase *min = abi_fixture_minimal();
  AbiCase *rich = abi_fixture_rich();

  CHECK(min != NULL, "minimal case built");
  CHECK(rich != NULL, "rich case built");
  if (min) {
    roundtrip_case(min, "minimal");
    abi_case_free(min);
  }
  if (rich) {
    roundtrip_case(rich, "rich");
    abi_case_free(rich);
  }
}

static void test_size_budget(void) {
  AbiCase *c = abi_fixture_rich();
  uint8_t *b = NULL;
  size_t   n = 0;
  if (!c) return;
  if (abi_case_encode(c, &b, &n) == ABI_OK) {
    /* format 4.5: a case must be small enough to attach to an issue */
    CHECKF(n < 10240, "rich case is %llu bytes, budget is 10 KB",
           (unsigned long long)n);
    printf("  rich case encodes to %llu bytes\n", (unsigned long long)n);
    abi_free(b);
  }
  abi_case_free(c);
}

/* --- topology ------------------------------------------------------------ */

static void test_alias_preserved(void) {
  AbiCase *c = abi_fixture_rich();
  AbiCase *back = NULL;
  uint8_t *b = NULL;
  size_t   n = 0;
  AbiError err;

  if (!c) return;
  if (abi_case_encode(c, &b, &n) != ABI_OK) {
    abi_case_free(c);
    return;
  }
  if (abi_case_decode(b, n, &back, &err) != ABI_OK) {
    CHECKF(0, "decode failed: %s", err.message);
    abi_free(b);
    abi_case_free(c);
    return;
  }

  CHECK(back->array != NULL && back->array->child_count == 2,
        "rich case has two children");
  if (back->array && back->array->child_count == 2) {
    const AbiBufferView *v0 = &back->array->children[0]->buffers[0];
    const AbiBufferView *v1 = &back->array->children[1]->buffers[0];
    /*
     * The point of the allocation/view model: these two buffers shared one
     * allocation before encoding and must still share one after. Serializing
     * buffers as independent blocks would silently turn this into two
     * allocations with equal contents, and every ownership test built on it
     * would stop testing anything.
     */
    CHECK(v0->allocation_id == v1->allocation_id,
          "aliased buffers must reference the same allocation after replay");
    CHECK(v0->byte_offset == v1->byte_offset,
          "aliased buffers must keep the same offset");

    /* the misaligned view must survive exactly, not be normalized away */
    CHECK(back->array->children[1]->buffers[1].byte_offset == 1,
          "intentional misalignment must be preserved");
    CHECKF(back->allocations[back->array->children[1]->buffers[1].allocation_id]
                   .alignment == 64,
           "allocation alignment must survive, got %lu",
           (unsigned long)back
               ->allocations[back->array->children[1]->buffers[1].allocation_id]
               .alignment);

    /* NULL buffer stays NULL, and is not a zero-length allocation */
    CHECK(back->array->buffers[0].present == 0,
          "NULL buffer must decode as absent");

    /* dictionary survives on both sides */
    CHECK(back->array->children[1]->dictionary != NULL,
          "array dictionary preserved");
    CHECK(back->schema->children[1]->dictionary != NULL,
          "schema dictionary preserved");
  }

  /* absent name vs empty name must remain distinguishable */
  CHECK(back->schema->has_name == 0, "absent schema name stays absent");
  CHECK(back->schema->children[1]->dictionary->has_name == 1 &&
            back->schema->children[1]->dictionary->name.size == 0,
        "empty schema name stays present-and-empty");

  /* metadata with an embedded NUL survives as bytes */
  CHECK(back->schema->metadata_count == 2, "metadata pair count");
  if (back->schema->metadata_count == 2) {
    CHECK(back->schema->metadata[1].value.size == 3 &&
              memcmp(back->schema->metadata[1].value.data, "a\0b", 3) == 0,
          "metadata value with embedded NUL survives");
  }

  abi_case_free(back);
  abi_free(b);
  abi_case_free(c);
}

static void test_case_id(void) {
  AbiCase *a = abi_fixture_rich();
  AbiCase *b = abi_fixture_rich();
  AbiCase *m = abi_fixture_minimal();
  char     ida[ABICASE_ID_HEX_SIZE], idb[ABICASE_ID_HEX_SIZE];
  char     idm[ABICASE_ID_HEX_SIZE];

  if (!a || !b || !m) return;
  CHECK(abi_case_id(a, ida) == ABI_OK, "case id computed");
  CHECK(abi_case_id(b, idb) == ABI_OK, "case id computed");
  CHECK(abi_case_id(m, idm) == ABI_OK, "case id computed");
  CHECK(strcmp(ida, idb) == 0, "identical cases share an id");
  CHECK(strcmp(ida, idm) != 0, "different cases have different ids");
  CHECK(strlen(ida) == 32, "case id is 32 hex digits");
  printf("  rich case id: %s\n", ida);

  /* the id must be readable from the bytes without decoding */
  {
    uint8_t *bytes = NULL;
    size_t   n = 0;
    char     idf[ABICASE_ID_HEX_SIZE];
    if (abi_case_encode(a, &bytes, &n) == ABI_OK) {
      CHECK(abi_case_id_of_bytes(bytes, n, idf) == ABI_OK, "id from bytes");
      CHECK(strcmp(ida, idf) == 0, "id from bytes matches id from case");
      abi_free(bytes);
    }
  }
  abi_case_free(a);
  abi_case_free(b);
  abi_case_free(m);
}

/* --- rejection ----------------------------------------------------------- */

typedef void (*MutateFn)(uint8_t *b, size_t n);

static void mut_magic(uint8_t *b, size_t n) {
  (void)n;
  b[0] = 'X';
}
static void mut_version(uint8_t *b, size_t n) {
  (void)n;
  abi_store_u16(b + 4, 2);
}
static void mut_header_size(uint8_t *b, size_t n) {
  (void)n;
  abi_store_u16(b + 6, 48);
}
static void mut_bom(uint8_t *b, size_t n) {
  (void)n;
  abi_store_u32(b + 8, 0xFFFE0000u);
}
static void mut_class(uint8_t *b, size_t n) {
  (void)n;
  b[12] = 9;
}
static void mut_reserved(uint8_t *b, size_t n) {
  (void)n;
  b[13] = 1;
}
static void mut_total_size(uint8_t *b, size_t n) {
  abi_store_u32(b + 20, (uint32_t)n - 1);
}
static void mut_payload_id(uint8_t *b, size_t n) {
  (void)n;
  b[24] ^= 0x01u;
}
static void mut_payload(uint8_t *b, size_t n) { b[n - 1] ^= 0x80u; }
static void mut_section_count(uint8_t *b, size_t n) {
  (void)n;
  abi_store_u32(b + 16, 99);
}

static void expect_reject(MutateFn mutate, AbiStatus want, const char *what) {
  AbiCase  *c = abi_fixture_rich();
  uint8_t  *b = NULL;
  size_t    n = 0;
  AbiCase  *back = NULL;
  AbiError  err;
  AbiStatus st;

  if (!c) return;
  if (abi_case_encode(c, &b, &n) != ABI_OK) {
    abi_case_free(c);
    return;
  }
  mutate(b, n);
  memset(&err, 0, sizeof(err));
  st = abi_case_decode(b, n, &back, &err);
  CHECKF(st != ABI_OK, "%s: mutation was accepted", what);
  CHECKF(st == want, "%s: got %s, expected %s", what, abi_status_str(st),
         abi_status_str(want));
  if (st == ABI_OK) abi_case_free(back);
  abi_free(b);
  abi_case_free(c);
}

static void test_rejects_malformed_header(void) {
  expect_reject(mut_magic, ABI_ERR_BAD_MAGIC, "bad magic");
  expect_reject(mut_version, ABI_ERR_BAD_VERSION, "unsupported version");
  expect_reject(mut_header_size, ABI_ERR_BAD_VERSION, "wrong header size");
  expect_reject(mut_bom, ABI_ERR_BAD_BYTE_ORDER,
                "big-endian byte order marker");
  expect_reject(mut_class, ABI_ERR_BAD_ENUM, "unknown class");
  expect_reject(mut_reserved, ABI_ERR_NOT_CANONICAL, "non-zero reserved byte");
  expect_reject(mut_total_size, ABI_ERR_TRAILING_BYTES, "short total_size");
  expect_reject(mut_payload_id, ABI_ERR_DIGEST_MISMATCH, "corrupt payload id");
  expect_reject(mut_payload, ABI_ERR_DIGEST_MISMATCH, "corrupt payload");
  expect_reject(mut_section_count, ABI_ERR_NOT_CANONICAL,
                "absurd section count");
}

/*
 * A file whose allocation payload is encoded correctly but not minimally must
 * be refused: accepting it would give one case two valid encodings, and the
 * round-trip guarantee would quietly become false.
 */
static void test_rejects_non_minimal_fill(void) {
  AbiCase  *c = abi_case_new(ABI_CLASS_A);
  uint8_t  *b = NULL;
  size_t    n = 0;
  uint8_t   ones[16];
  size_t    pos, sec_at = 0, fill_at = 0;
  AbiCase  *back = NULL;
  AbiError  err;
  AbiStatus st;

  if (!c) return;
  memset(ones, 0xFF, sizeof(ones));
  abi_case_add_allocation(c, ones, sizeof(ones), 8, NULL);
  abi_case_set_schema(c, abi_schema_new(c, "i"));
  if (abi_case_encode(c, &b, &n) != ABI_OK) {
    abi_case_free(c);
    return;
  }

  /* walk the section table rather than scanning for a byte pattern */
  pos = ABICASE_HEADER_SIZE;
  while (pos + ABICASE_SECTION_HEADER_SIZE <= n) {
    uint16_t tag = abi_load_u16(b + pos);
    uint32_t sec_size = abi_load_u32(b + pos + 4);
    if (tag == ABICASE_TAG_ALLOCATIONS) {
      sec_at = pos;
      /* payload: alloc_count u32, then size_bytes u64, alignment u32, fill u8
       */
      fill_at = pos + ABICASE_SECTION_HEADER_SIZE + 4 + 8 + 4;
      break;
    }
    pos += ABICASE_SECTION_HEADER_SIZE + sec_size;
  }
  CHECK(fill_at != 0, "located the allocation fill byte");
  if (fill_at == 0 || fill_at >= n) {
    abi_free(b);
    abi_case_free(c);
    return;
  }

  CHECKF(b[fill_at] == (uint8_t)ABI_FILL_PATTERN,
         "expected PATTERN at the fill byte, found %u", (unsigned)b[fill_at]);

  {
    /*
     * Splice the allocation payload from PATTERN(period 1, 5 bytes) to RAW
     * (16 literal bytes), leaving every later section in place, then repair
     * section size, total_size and the payload digest. Everything a strict
     * decoder checks before canonicality is therefore correct, and the only
     * remaining defect is that the encoding is not the minimal one.
     */
    const size_t old_payload = 4u + 1u; /* period u32 + one pattern byte */
    const size_t new_payload = sizeof(ones);
    const size_t tail_at = fill_at + 4 + old_payload;
    size_t       flen = n - old_payload + new_payload;
    uint8_t     *forged = (uint8_t *)malloc(flen);
    uint8_t      digest[32];

    CHECK(forged != NULL, "allocated forged file");
    if (forged) {
      memcpy(forged, b, fill_at);
      forged[fill_at] = (uint8_t)ABI_FILL_RAW;
      forged[fill_at + 1] = 0;
      forged[fill_at + 2] = 0;
      forged[fill_at + 3] = 0;
      memset(forged + fill_at + 4, 0xFF, new_payload);
      memcpy(forged + fill_at + 4 + new_payload, b + tail_at, n - tail_at);

      abi_store_u32(forged + sec_at + 4,
                    abi_load_u32(b + sec_at + 4) +
                        (uint32_t)(new_payload - old_payload));
      abi_store_u32(forged + 20, (uint32_t)flen);
      abi_sha256(forged + ABICASE_HEADER_SIZE, flen - ABICASE_HEADER_SIZE,
                 digest);
      memcpy(forged + 24, digest, ABICASE_ID_BYTES);

      memset(&err, 0, sizeof(err));
      st = abi_case_decode(forged, flen, &back, &err);
      CHECKF(st == ABI_ERR_NOT_CANONICAL,
             "non-minimal RAW fill: got %s (%s), expected not canonical",
             abi_status_str(st), err.message);
      if (st == ABI_OK) abi_case_free(back);
      free(forged);
    }
  }
  abi_free(b);
  abi_case_free(c);
}

/* --- robustness ---------------------------------------------------------- */

/*
 * Every proper prefix of a valid file must be refused without reading out of
 * bounds. Meaningful only under ASan, which is why CI runs this build.
 */
static void test_truncation_sweep(void) {
  AbiCase *c = abi_fixture_rich();
  uint8_t *b = NULL;
  size_t   n = 0, len;
  int      accepted = 0;

  if (!c) return;
  if (abi_case_encode(c, &b, &n) != ABI_OK) {
    abi_case_free(c);
    return;
  }
  for (len = 0; len < n; len++) {
    AbiCase  *back = NULL;
    AbiError  err;
    AbiStatus st;
    memset(&err, 0, sizeof(err));
    st = abi_case_decode(b, len, &back, &err);
    if (st == ABI_OK) {
      accepted++;
      abi_case_free(back);
    }
  }
  CHECKF(accepted == 0, "%d truncated prefix(es) were accepted", accepted);
  printf("  truncation sweep: %llu prefixes rejected\n", (unsigned long long)n);
  abi_free(b);
  abi_case_free(c);
}

/*
 * Single-bit corruption must either be refused or, where the flipped bit is a
 * legitimately different value, decode to a case that re-encodes to exactly the
 * corrupted bytes. There is no third outcome: a file that decodes but
 * re-encodes differently would mean the format is not canonical.
 */
static void test_corruption_sweep(void) {
  AbiCase *c = abi_fixture_rich();
  uint8_t *b = NULL;
  size_t   n = 0, i;
  int      bit;
  int      accepted = 0, unstable = 0;

  if (!c) return;
  if (abi_case_encode(c, &b, &n) != ABI_OK) {
    abi_case_free(c);
    return;
  }
  for (i = 0; i < n; i++) {
    for (bit = 0; bit < 8; bit++) {
      AbiCase  *back = NULL;
      AbiError  err;
      AbiStatus st;

      b[i] ^= (uint8_t)(1u << bit);
      memset(&err, 0, sizeof(err));
      st = abi_case_decode(b, n, &back, &err);
      if (st == ABI_OK) {
        uint8_t *again = NULL;
        size_t   n2 = 0;
        accepted++;
        if (abi_case_encode(back, &again, &n2) == ABI_OK) {
          if (n2 != n || memcmp(again, b, n) != 0) unstable++;
          abi_free(again);
        } else {
          unstable++;
        }
        abi_case_free(back);
      }
      b[i] ^= (uint8_t)(1u << bit);
    }
  }
  CHECKF(unstable == 0,
         "%d corrupted file(s) decoded but did not re-encode identically",
         unstable);
  printf("  corruption sweep: %llu flips, %d accepted as valid variants\n",
         (unsigned long long)(n * 8), accepted);
  abi_free(b);
  abi_case_free(c);
}

/* --- class rules and invariants ------------------------------------------ */

static void test_class_rules(void) {
  AbiStatus st;

  /* B1 may declare n_buffers inconsistent with the views it provides */
  {
    AbiCase      *c = abi_case_new(ABI_CLASS_B1);
    AbiArrayNode *a;
    if (c) {
      abi_case_set_schema(c, abi_schema_new(c, "i"));
      a = abi_array_new(c);
      a->length = 4;
      abi_array_add_null_buffer(c, a, ABI_ROLE_VALIDITY);
      abi_array_set_declared_buffers(a, 7); /* the lie under test */
      abi_case_set_array(c, a);
      st = abi_case_validate(c, NULL);
      CHECKF(st == ABI_OK, "B1 must permit n_buffers != buffer_count, got %s",
             abi_status_str(st));
      roundtrip_case(c, "b1-inconsistent-n_buffers");
      abi_case_free(c);
    }
  }

  /* the same case in class A is a generator bug, not a test case */
  {
    AbiCase      *c = abi_case_new(ABI_CLASS_A);
    AbiArrayNode *a;
    if (c) {
      abi_case_set_schema(c, abi_schema_new(c, "i"));
      a = abi_array_new(c);
      a->length = 4;
      abi_array_add_null_buffer(c, a, ABI_ROLE_VALIDITY);
      abi_array_set_declared_buffers(a, 7);
      abi_case_set_array(c, a);
      st = abi_case_validate(c, NULL);
      CHECKF(st == ABI_ERR_CLASS_RULE,
             "class A must reject n_buffers != buffer_count, got %s",
             abi_status_str(st));
      abi_case_free(c);
    }
  }

  /* consumer-misuse ops require class C */
  {
    AbiCase *c = abi_case_new(ABI_CLASS_A);
    if (c) {
      abi_case_set_schema(c, abi_schema_new(c, "i"));
      abi_case_add_op(c, ABI_OP_RELEASE_CHILD, 0, 0);
      st = abi_case_validate(c, NULL);
      CHECKF(st == ABI_ERR_CLASS_RULE,
             "RELEASE_CHILD outside class C must be refused, got %s",
             abi_status_str(st));
      abi_case_free(c);
    }
  }
  {
    AbiCase *c = abi_case_new(ABI_CLASS_C);
    if (c) {
      abi_case_set_schema(c, abi_schema_new(c, "i"));
      abi_case_add_op(c, ABI_OP_RELEASE_CHILD, 0, 0);
      st = abi_case_validate(c, NULL);
      CHECKF(st == ABI_OK, "RELEASE_CHILD in class C must be allowed, got %s",
             abi_status_str(st));
      roundtrip_case(c, "class-c-misuse-op");
      abi_case_free(c);
    }
  }
}

/*
 * A view may never escape its own allocation, in any class. "Buffer too short"
 * is modelled as a small allocation with a large ArrowArray.length, so that the
 * out-of-bounds read happens in the consumer under test rather than in this
 * tool.
 */
static void test_containment_invariant(void) {
  AbiCase      *c = abi_case_new(ABI_CLASS_B1);
  AbiArrayNode *a;
  uint8_t       data[16];
  uint32_t      id = 0;
  AbiStatus     st;

  if (!c) return;
  memset(data, 0x5A, sizeof(data));
  abi_case_add_allocation(c, data, sizeof(data), 8, &id);
  abi_case_set_schema(c, abi_schema_new(c, "i"));

  a = abi_array_new(c);
  a->length = 4;
  abi_array_add_buffer(c, a, ABI_ROLE_DATA, id, 8, 16); /* 8 + 16 > 16 */
  abi_case_set_array(c, a);

  st = abi_case_validate(c, NULL);
  CHECKF(st == ABI_ERR_BAD_REFERENCE,
         "a view escaping its allocation must be refused even in B1, got %s",
         abi_status_str(st));

  /* and the encoder must refuse to emit it, not just the validator */
  {
    uint8_t *b = NULL;
    size_t   n = 0;
    st = abi_case_encode(c, &b, &n);
    CHECKF(st == ABI_ERR_BAD_REFERENCE,
           "the encoder must refuse an escaping view, got %s",
           abi_status_str(st));
    if (st == ABI_OK) abi_free(b);
  }

  /* the legitimate form of the same test: small allocation, large length */
  {
    AbiCase      *ok = abi_case_new(ABI_CLASS_B1);
    AbiArrayNode *oa;
    uint32_t      oid = 0;
    if (ok) {
      abi_case_add_allocation(ok, data, 4, 8, &oid);
      abi_case_set_schema(ok, abi_schema_new(ok, "i"));
      oa = abi_array_new(ok);
      oa->length = 1000000; /* claims far more than 4 bytes hold */
      abi_array_add_null_buffer(ok, oa, ABI_ROLE_VALIDITY);
      abi_array_add_buffer(ok, oa, ABI_ROLE_DATA, oid, 0, 4);
      abi_case_set_array(ok, oa);
      st = abi_case_validate(ok, NULL);
      CHECKF(
          st == ABI_OK,
          "a short buffer with an overlong length is a valid B1 case, got %s",
          abi_status_str(st));
      roundtrip_case(ok, "b1-short-buffer");
      abi_case_free(ok);
    }
  }
  abi_case_free(c);
}

static void test_schema_only_case(void) {
  /* format-string bugs are reached before any data exists: ARRAY is optional */
  AbiCase  *c = abi_case_new(ABI_CLASS_B1);
  AbiStatus st;
  if (!c) return;
  abi_case_set_schema(c, abi_schema_new(c, "w:")); /* truncated parameter */
  abi_case_set_expected(c, ABI_VALIDATE_MINIMAL, ABI_EXPECT_REJECT,
                        "C Data Interface: format strings",
                        "fixed-size binary with no width");
  st = abi_case_validate(c, NULL);
  CHECKF(st == ABI_OK, "a schema-only case must be valid, got %s",
         abi_status_str(st));
  CHECK(c->array == NULL, "no array section");
  roundtrip_case(c, "schema-only");
  abi_case_free(c);
}

/* --- golden fixture ------------------------------------------------------ */

/*
 * Guards against silent format drift. Regenerate deliberately, with
 * ABI_UPDATE_GOLDEN=1, and only together with a case_schema_ver bump.
 */
static void test_golden_fixture(void) {
  const char *path = ABI_FIXTURE_DIR "/rich-v1.abicase";
  AbiCase    *c = abi_fixture_rich();
  uint8_t    *mine = NULL, *disk = NULL;
  size_t      n_mine = 0, n_disk = 0;

  if (!c) return;
  if (abi_case_encode(c, &mine, &n_mine) != ABI_OK) {
    abi_case_free(c);
    return;
  }

  if (getenv("ABI_UPDATE_GOLDEN")) {
    CHECK(abi_case_write_file(c, path) == ABI_OK, "golden fixture written");
    printf("  golden fixture regenerated: %s\n", path);
  } else if (abi_read_file(path, &disk, &n_disk) == ABI_OK) {
    CHECKF(n_disk == n_mine,
           "golden fixture is %llu bytes, encoder produced %llu",
           (unsigned long long)n_disk, (unsigned long long)n_mine);
    CHECK(n_disk == n_mine && memcmp(disk, mine, n_mine) == 0,
          "encoder output drifted from the golden fixture");
    free(disk);

    /* and it must still decode, through the public file path */
    {
      AbiCase *back = NULL;
      AbiError err;
      memset(&err, 0, sizeof(err));
      CHECKF(abi_case_read_file(path, &back, &err) == ABI_OK,
             "golden fixture decodes: %s", err.message);
      if (back) abi_case_free(back);
    }
  } else {
    CHECK(0,
          "golden fixture missing; run with ABI_UPDATE_GOLDEN=1 to create it");
  }

  abi_free(mine);
  abi_case_free(c);
}

int main(void) {
  RUN(test_endian_primitives);
  RUN(test_sha256_vectors);
  RUN(test_fill_choose);
  RUN(test_roundtrip);
  RUN(test_size_budget);
  RUN(test_alias_preserved);
  RUN(test_case_id);
  RUN(test_rejects_malformed_header);
  RUN(test_rejects_non_minimal_fill);
  RUN(test_truncation_sweep);
  RUN(test_corruption_sweep);
  RUN(test_class_rules);
  RUN(test_containment_invariant);
  RUN(test_schema_only_case);
  RUN(test_golden_fixture);

  printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
