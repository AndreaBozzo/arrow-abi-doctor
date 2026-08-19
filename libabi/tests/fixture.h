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

#endif /* ABI_FIXTURE_H */
