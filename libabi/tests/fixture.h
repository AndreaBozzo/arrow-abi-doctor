/*
 * Shared M0 fixtures.
 *
 * Built by both the test suite and the abicase CLI so that the cross-
 * architecture check compares the same case, defined once. If this file and the
 * golden fixture ever disagree, the golden test says so.
 */
#ifndef ABI_FIXTURE_H
#define ABI_FIXTURE_H

#include "abi/abicase.h"

/*
 * Exercises every feature the format claims to carry: buffer aliasing, an
 * intentionally misaligned view, NULL buffers, a dictionary, absent vs empty
 * names, metadata with an embedded NUL, and all four allocation fill encodings.
 */
AbiCase *abi_fixture_rich(void);

/* Schema-only case: no ARRAY section. */
AbiCase *abi_fixture_minimal(void);

/*
 * A plainly valid struct<a: int32, b: utf8> with four rows and one null.
 *
 * Deliberately boring, and separate from the rich fixture: the rich one carries
 * dictionary indices that are out of range for its dictionary, which is fine
 * for exercising the format but wrong for a smoke test, where anything a
 * consumer rejects has to be our bug rather than the case's.
 *
 * Buffer contents are little-endian int32, matching the host this was written
 * for. See docs/abicase-format.md 10 on why scalar values, unlike structure and
 * topology, are only meaningful on hosts of the same endianness.
 */
AbiCase *abi_fixture_smoke(void);

/*
 * Class B1: struct<d: dictionary<int8, utf8>> whose index 99 is out of range
 * for a two-value dictionary.
 *
 * Structurally well-formed, semantically invalid -- exactly the "trusted but
 * defective producer" shape. A conforming consumer must refuse cleanly; reading
 * the dictionary at an unchecked index is a bug regardless of the input being
 * invalid, because "we do not support that" cannot cover an out-of-bounds read.
 */
AbiCase *abi_fixture_bad_dict_index(void);

#endif /* ABI_FIXTURE_H */
