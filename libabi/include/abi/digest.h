/*
 * Dual digest: physical and logical, with the boundary written down.
 *
 * A single checksum produces false positives. A consumer that preserves
 * `offset = 7` and one that materializes the slice at `offset = 0` represent
 * the same values with different layouts, and one digest cannot tell "the data
 * changed" from "the layout changed".
 *
 *   physical   schema, declared fields, and the buffer bytes as laid out
 *   logical    type, length, validity, logical values
 *
 * Both are computed over `ArrowSchema` + `ArrowArray` rather than over an
 * `AbiCase`, because the question they answer is asked of both sides: what this
 * harness handed over, and what a consumer handed back. An `AbiCase`-only
 * digest could only ever describe our half.
 *
 * **What `logical` answers is "did the data survive transport?"** -- not "do
 * the engines agree after an operation". No timezone, decimal, ordering or
 * dictionary-decoding normalization belongs here. That is semdiff's equivalence
 * spec, and semdiff does not start until after M2; a digest that starts
 * *computing* has absorbed semdiff without anyone deciding to. The
 * normalization list is deliberately short, is written down in
 * docs/digest.md, and is versioned by ABI_DIGEST_VER.
 *
 * Digests computed under different ABI_DIGEST_VER values are not comparable.
 * The version is mixed into both, so a stale digest cannot silently compare
 * equal to a fresh one.
 */
#ifndef ABI_DIGEST_H
#define ABI_DIGEST_H

#include "abi/abicase.h"
#include "abi/arrow_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on any change to what either digest covers. docs/digest.md 5. */
#define ABI_DIGEST_VER 1u

/*
 * 128 bits, the same width as the payload id in the container format, and for
 * the same reason: it identifies and detects accidental divergence. It is not
 * an authentication tag.
 */
#define ABI_DIGEST_BYTES    16u
#define ABI_DIGEST_HEX_SIZE 33u /* 32 hex digits + NUL */

typedef struct {
  uint8_t physical[ABI_DIGEST_BYTES];
  uint8_t logical[ABI_DIGEST_BYTES];
} AbiDigest;

/*
 * Digests the array `array` described by `schema`. Neither is modified and
 * neither is released.
 *
 * The v0 feature set only: `i`, `l`, `g`, `u`, `b` and `+s`. Anything else is
 * ABI_ERR_INVALID_ARGUMENT with the format string in the message rather than a
 * digest over bytes this code does not understand -- a digest that guesses at a
 * layout would compare equal or unequal for reasons nobody could explain.
 * Dictionaries are M3 and are rejected here for the same reason.
 *
 * Buffer sizes are computed from the schema and from `length + offset`, because
 * the C Data Interface does not transmit them. That is the same computation
 * every consumer must do, and it is why a producer that under-sizes a buffer
 * makes every consumer read out of bounds.
 */
AbiStatus abi_digest(const struct ArrowSchema *schema,
                     const struct ArrowArray *array, AbiDigest *out,
                     AbiError *err);

/* Lowercase hex of one digest half; `out` is ABI_DIGEST_HEX_SIZE bytes. */
void abi_digest_hex(const uint8_t digest[ABI_DIGEST_BYTES], char *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ABI_DIGEST_H */
