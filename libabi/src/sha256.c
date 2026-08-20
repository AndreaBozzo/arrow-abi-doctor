/*
 * SHA-256 (FIPS 180-4), used only to derive the canonical payload id.
 *
 * Vendored rather than depended upon: libabi must build with nothing but a C11
 * compiler on every host a case has to replay on, including the big-endian
 * cross target. Written against the byte stream throughout, so the digest is
 * identical on hosts of either endianness -- which is the whole point of having
 * it.
 */
#include "abi_internal.h"

static const uint32_t abi_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t abi_rotr(uint32_t v, int n) {
  return (v >> n) | (v << (32 - n));
}

static void abi_sha256_block(AbiSha256 *s, const uint8_t *p) {
  uint32_t w[64];
  uint32_t a, b, c, d, e, f, g, h;
  int      i;

  for (i = 0; i < 16; i++) {
    /* big-endian word load, spelled out: the digest is defined on bytes */
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
           ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
  }
  for (i = 16; i < 64; i++) {
    uint32_t s0 =
        abi_rotr(w[i - 15], 7) ^ abi_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 =
        abi_rotr(w[i - 2], 17) ^ abi_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  a = s->state[0];
  b = s->state[1];
  c = s->state[2];
  d = s->state[3];
  e = s->state[4];
  f = s->state[5];
  g = s->state[6];
  h = s->state[7];

  for (i = 0; i < 64; i++) {
    uint32_t S1 = abi_rotr(e, 6) ^ abi_rotr(e, 11) ^ abi_rotr(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + abi_sha256_k[i] + w[i];
    uint32_t S0 = abi_rotr(a, 2) ^ abi_rotr(a, 13) ^ abi_rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  s->state[0] += a;
  s->state[1] += b;
  s->state[2] += c;
  s->state[3] += d;
  s->state[4] += e;
  s->state[5] += f;
  s->state[6] += g;
  s->state[7] += h;
}

void abi_sha256_init(AbiSha256 *s) {
  s->length = 0;
  s->block_used = 0;
  s->state[0] = 0x6a09e667u;
  s->state[1] = 0xbb67ae85u;
  s->state[2] = 0x3c6ef372u;
  s->state[3] = 0xa54ff53au;
  s->state[4] = 0x510e527fu;
  s->state[5] = 0x9b05688cu;
  s->state[6] = 0x1f83d9abu;
  s->state[7] = 0x5be0cd19u;
}

void abi_sha256_update(AbiSha256 *s, const void *data, size_t size) {
  const uint8_t *p = (const uint8_t *)data;
  s->length += (uint64_t)size;

  if (s->block_used) {
    size_t want = 64 - s->block_used;
    size_t take = (size < want) ? size : want;
    memcpy(s->block + s->block_used, p, take);
    s->block_used += take;
    p += take;
    size -= take;
    if (s->block_used == 64) {
      abi_sha256_block(s, s->block);
      s->block_used = 0;
    }
  }
  while (size >= 64) {
    abi_sha256_block(s, p);
    p += 64;
    size -= 64;
  }
  if (size) {
    memcpy(s->block, p, size);
    s->block_used = size;
  }
}

void abi_sha256_final(AbiSha256 *s, uint8_t out[32]) {
  uint64_t bits = s->length * 8u;
  uint8_t  pad[72];
  size_t   padlen;
  int      i;

  /* 0x80, then zeros to 56 mod 64, then the length as a big-endian u64 */
  padlen = (s->block_used < 56) ? (56 - s->block_used) : (120 - s->block_used);
  memset(pad, 0, sizeof(pad));
  pad[0] = 0x80u;
  for (i = 0; i < 8; i++) {
    pad[padlen + i] = (uint8_t)((bits >> (56 - 8 * i)) & 0xFFu);
  }
  abi_sha256_update(s, pad, padlen + 8);

  for (i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)((s->state[i] >> 24) & 0xFFu);
    out[4 * i + 1] = (uint8_t)((s->state[i] >> 16) & 0xFFu);
    out[4 * i + 2] = (uint8_t)((s->state[i] >> 8) & 0xFFu);
    out[4 * i + 3] = (uint8_t)(s->state[i] & 0xFFu);
  }
}

void abi_sha256(const void *data, size_t size, uint8_t out[32]) {
  AbiSha256 s;
  abi_sha256_init(&s);
  abi_sha256_update(&s, data, size);
  abi_sha256_final(&s, out);
}

void abi_hex(const uint8_t *bytes, size_t n, char *out_hex) {
  static const char digits[] = "0123456789abcdef";
  size_t            i;
  for (i = 0; i < n; i++) {
    out_hex[2 * i] = digits[(bytes[i] >> 4) & 0xFu];
    out_hex[2 * i + 1] = digits[bytes[i] & 0xFu];
  }
  out_hex[2 * n] = '\0';
}
