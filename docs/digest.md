# Dual digest

`digest_ver` = 1
Status: **normative.** `libabi/src/digest.c` is the executable form; the C tests
in `libabi/tests/test_digest.c` assert every property stated here.

A single checksum over a delivered array produces false positives. A consumer
that preserves `offset = 7` and one that materializes the slice at `offset = 0`
represent the same values in different layouts, and one digest cannot tell "the
data changed" from "the layout changed". Two are computed instead:

| | Covers |
|---|---|
| `physical_digest` | the schema as declared, the declared fields, and the buffer bytes within their declared extent |
| `logical_digest` | the type, the length, the validity, and the logical values |

Both are computed over `ArrowSchema` + `ArrowArray`, not over an `AbiCase`,
because the question is asked of both sides of the interface: what the harness
handed over, and what the consumer handed back. A digest that could only be
computed from a `.abicase` could only ever describe our half.

---

## 1. The boundary, which is the important part

`logical_digest` answers **"did the data survive transport?"** It does not answer
"do the engines agree after an operation."

No timezone conversion, decimal rescaling, ordering, or dictionary decoding
belongs here. That is `semdiff`'s equivalence specification (design spec §8), and
`semdiff` does not start until after M2. The risk this section exists to name is
concrete: if this digest starts *computing* — normalizing anything that requires
knowing what a value **means** rather than what it **is** — then the project has
absorbed semdiff without anyone deciding to, and the M2 milestone quietly
changed shape.

The test for whether a proposed rule belongs here: it must be expressible as
"these two bit patterns are the same logical value" without reference to a type
parameter, a locale, a calendar, or a sort order.

---

## 2. What `logical_digest` normalizes

The list is short on purpose, and it is the whole list.

| Rule | Why it is a transport question, not a semantic one |
|---|---|
| **Bytes of null slots are ignored** | A null slot has no value. Arrow leaves the bytes under it unspecified, so a producer may write anything there and a consumer may write anything back. Comparing them would report a difference that no data has. |
| **`-0.0` and `+0.0` are the same value** | Two bit patterns, one number. IEEE 754 defines them as equal under `==`. |
| **All NaN payloads are one value** | Every quiet NaN means "not a number". The payload is not data a transport is obliged to preserve, and Arrow does not require it. |
| **The physical offset is irrelevant** | `ArrowArray.offset` is a window into buffers, not content. Preserving a slice and materializing it are both correct. |
| **The declared `null_count` is not read** | Validity comes from the bitmap. `null_count = -1` means "not computed" and is legal, so reading it would make a legal producer choice look like a data difference. |
| **Field names and metadata are not covered** | They are not values. They are covered by `physical_digest`, so a rename shows up as a layout change with the data intact — which is exactly the discrimination the pair exists for. |

And what it explicitly does **not** normalize:

| Not normalized | Why |
|---|---|
| **UTF-8 bytes** | Compared raw. Two Unicode normalization forms of the same text are different bytes, and deciding they are the same text is a semantic judgement — semdiff's, not this. |
| **Integer width** | An `int32` 1 and an `int64` 1 have different types, and the type is part of the logical content. |
| **Null vs. a zero value** | A null is the absence of a value, not the value zero. |

Multi-byte values are fed to the hash **little-endian regardless of the host**,
so the digest of a given logical value is the same on every architecture. What
does not survive an endianness change is what a given `int32` *means* when read
from native-endian buffers (format §10.1) — that is a property of the data, and
the digest correctly reports the two hosts as holding different data.

---

## 3. What `physical_digest` covers

Per node, in tree order: the format string, the name (an absent name and an
empty one are distinguished), the flags, `length`, `null_count`, `offset`,
`n_buffers`, `n_children`, then for each buffer a presence flag and, if present,
its bytes.

It is deliberately strict. `null_count = -1` against `null_count = 0` is a
physical difference even though it is not a data difference; so is a rename.
Anything that moves `physical_digest` while `logical_digest` holds still is a
representation difference, which is a compatibility-matrix entry rather than a
defect.

Buffer extents are computed from the schema and from `length + offset`, because
**the C Data Interface does not transmit buffer sizes**. Every consumer is
obliged to do this same arithmetic, which is why a producer that under-sizes a
buffer makes all of them read out of bounds and none of them able to notice. The
values buffer of a variable-length type runs to the last offset of the physical
range, and the array offset is *not* dropped when the length is zero — that
particular mistake is [apache/arrow-rs#10910](https://github.com/apache/arrow-rs/issues/10910),
found by this project.

### What `physical_digest` cannot cover

It hashes **bytes**, and the C Data Interface hands a consumer **pointers**. So
anything that is a property of an address rather than of the bytes at it is
invisible to this or to any other byte-wise digest. Measured over the 1880
generated Corpus A cases, which give 375 distinct physical digests:

| Invisible | Measurement | Why |
|---|---|---|
| `alignment-class` | 0 of 630 groups move the digest when only the alignment varies | Realized as a byte offset of the view within its allocation (matrix §1.6). A view one byte in holds the same bytes. |
| `aliasing` | `normal` and `aliased` share a digest at every length | Whether three buffers sit in one allocation or three is an address relationship. The same bytes either way. |
| `empty` vs `normal` at length 0 | they share a digest | Both have a declared extent of zero bytes, and the size of the allocation behind a buffer is not transmitted. **No consumer can distinguish them either**, which is worth knowing about the class. |

This is a fact about the interface, not a gap to close, and the project already
has the right instrument for each: aliasing is verified by **pointer comparison**
(format §5.1), and alignment is verified by construction in the reconstructor.
Neither is a digest question.

The consequence for a differential run: agreement on both digests means the
values and the declared layout survived, not that the memory layout was
reproduced. That is the right strength of claim to make from two numbers.

---

## 4. Scope

The v0 feature set only: `i`, `l`, `g`, `u`, `b`, and `+s`. Any other format
string is an error naming the format, not a digest computed over bytes whose
layout this code does not know. Dictionaries are M3 and are refused for the same
reason.

A struct's window travels to its children: a child of a struct sliced at
offset 3 contributes its own offset plus 3, which is how Arrow indexes struct
children.

---

## 5. Versioning

`digest_ver` is mixed into both digests, so a digest computed under one version
can never compare equal to one computed under another. Any change to §2 or §3
bumps `ABI_DIGEST_VER` in `libabi/include/abi/digest.h` and the number at the top
of this file, in the same commit.

Digests are **not** part of the `.abicase` container format and do not affect
`case_schema_ver`. They are computed over a reconstruction, so they describe a
run, not a file.

### Change log

| Version | Date | Change |
|---|---|---|
| 1 | 2026-08-29 | Initial. Rules as above, for the v0 feature set. |
