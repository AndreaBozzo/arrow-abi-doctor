# The `.abicase` container format

Version: `case_schema_ver = 1`
Status: normative for M0. Changes to the byte layout require bumping
`case_schema_ver` and regenerating the golden fixtures.

A `.abicase` file is a **portable, canonical description of an Arrow C Data /
C Stream Interface test case**. It is not a memory image.

`ArrowSchema` and `ArrowArray` contain raw pointers, and Arrow's on-wire schema
metadata encoding uses **native-endian `int32`**. There is therefore no memory
image of an Arrow C structure that is portable between hosts of differing
endianness. This format defines a canonical encoding independent of the native
representation, from which the structures are *reconstructed* on the target
host.

Design requirements, from the project spec:

| Requirement | How it is met |
|---|---|
| Portable between architectures | all integers little-endian, fixed explicit widths, metadata stored decoded |
| Aliasing survives replay | allocations and views are modelled separately (§5) |
| Typical size < 10 KB | allocation payloads use a canonical minimal encoding (§5.2) |
| Attachable to an issue, no tool dependency | single self-contained file, documented here |
| Reconstructible without the generator | the payload is the case; the seed is only provenance |

---

## 1. Conventions

* All multi-byte integers are **little-endian**, always, on every host. There is
  no byte-order variant of this format and no byte-order flag. Encoders and
  decoders must use explicit shift-based serialization, never a `memcpy` of a
  native integer.
* Integer widths are fixed and explicit: `u8`, `u16`, `u32`, `u64`, `i64`.
  There are no variable-length integers.
* `str` is `u32 length` followed by exactly `length` bytes, **not**
  NUL-terminated. Arrow format strings are ASCII; names and metadata keys are
  UTF-8 by Arrow convention but are stored and compared as raw bytes.
* `bytes` has the same encoding as `str` and carries no character semantics.
* `bool` is `u8`, restricted to `0` or `1`. Any other value is invalid.
* Reserved fields are written as zero and **must** be zero on read. This keeps
  the encoding canonical: a decoder that ignored them would accept two distinct
  byte strings for the same case.
* Sizes are byte counts unless the field name says otherwise.

### 1.1 Canonicality

The format is **strictly canonical**: for a given case there is exactly one
valid byte string. A decoder must reject any input that a conforming encoder
would not have produced — non-ascending section order, duplicate sections,
non-zero reserved fields, a non-minimal allocation payload encoding (§5.2),
trailing bytes after the last section.

This is what makes the M0 round-trip guarantee cheap and total:

```
decode(b) = c  and  c valid  =>  encode(c) = b     byte for byte
```

Canonicality is a property of the *file*, not of the *case*. A `.abicase` may
describe a deliberately malformed Arrow structure (Corpus B1/B2) while itself
being a perfectly canonical file. The two validity notions are independent and
must not be conflated.

---

## 2. File layout

```
+--------------------------------------------------+
| header            40 bytes, fixed                |
+--------------------------------------------------+
| section 0         8-byte section header + payload|
| section 1                                        |
| ...                                              |  <- the "payload"
| section n-1                                      |
+--------------------------------------------------+
```

The **payload** is every byte from `header_size` to `total_size`.

### 2.1 Header

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `magic` | `"ABIC"` (`0x41 0x42 0x49 0x43`) |
| 4 | 2 | `case_schema_ver` | `u16`, currently `1` |
| 6 | 2 | `header_size` | `u16`, currently `40` |
| 8 | 4 | `bom` | `u32` = `0x0000FEFF` |
| 12 | 1 | `class` | `u8`, see §2.2 |
| 13 | 1 | `reserved8` | `0` |
| 14 | 2 | `reserved16` | `0` |
| 16 | 4 | `section_count` | `u32` |
| 20 | 4 | `total_size` | `u32`, size of the whole file |
| 24 | 16 | `payload_id` | `u8[16]`, see §2.3 |

`bom` is a byte-order self-check, not a byte-order *switch*. A reader that
loads it as a little-endian `u32` must obtain `0x0000FEFF`; obtaining
`0xFFFE0000` means the writer serialized native integers on a big-endian host
and the file is invalid, not byte-swapped. Rejecting is correct: silently
byte-swapping would hide exactly the bug this project exists to find.

`header_size` is present so that a future revision can extend the header while
leaving the payload boundary discoverable. A decoder for version 1 requires it
to be exactly 40.

### 2.2 Case class

| Value | Class | Meaning (project spec §3) |
|---|---|---|
| 0 | `A` | conforming producer, valid but rare input |
| 1 | `B1` | producer invalid in structure or data |
| 2 | `B2` | producer invalid in protocol or lifecycle |
| 3 | `C` | consumer misuse |

The class is a claim about *intent*, and it selects which reconstruction rules
apply (§7.3). It is deliberately in the header so that a triage tool can
classify a file without parsing the payload.

### 2.3 Payload identity

`payload_id` is the first 16 bytes of `SHA-256(payload)`, where `payload` is
bytes `[header_size, total_size)`.

The payload is the canonical identity of the test case. The seed in the
provenance block is *not* an identity: a generator modified six months from now
will produce something different from the same seed. Two files with the same
`payload_id` describe the same case; corpus deduplication and issue references
use it. Its lowercase hex form is the **case id**.

It is a truncated digest over the payload only, so it is an identifier and an
accidental-corruption check, not an authentication tag.

---

## 3. Sections

Each section is:

| Size | Field |
|---|---|
| 2 | `tag` (`u16`) |
| 2 | `version` (`u16`), per-section, currently `1` |
| 4 | `size` (`u32`), payload bytes, excluding this 8-byte header |

Sections appear in **strictly ascending `tag` order** and at most once each.
Section payloads are not padded and carry no alignment guarantee: an
allocation's alignment is a property of the reconstructed memory (§5.1), never
of its position in the file.

| Tag | Name | Required | Contents |
|---|---|---|---|
| `0x0001` | `PROVENANCE` | yes | §4 |
| `0x0002` | `ALLOCATIONS` | yes (may be empty) | §5 |
| `0x0003` | `SCHEMA` | yes | §6 |
| `0x0004` | `ARRAY` | no | §7 |
| `0x0005` | `CALLSEQ` | no | §8 |
| `0x0006` | `EXPECTED` | yes | §9 |

`ARRAY` is optional because a schema-only case is a real and valuable case: the
format-string parser is a consumer surface of its own, reached before any data
exists. DuckDB #21691 — a `std::stoi` crash parsing the format string for
fixed-size binary/list — is a schema-only bug.

An unknown tag is rejected. Version 1 has no forward-compatible skip rule; a
decoder that skipped unknown sections could not guarantee canonical round-trip,
and silent partial understanding of a test case is worse than a clean refusal.

---

## 4. `PROVENANCE` (tag `0x0001`)

| Field | Type |
|---|---|
| `seed` | `u64` |
| `rng_algorithm` | `str` |
| `rng_version` | `u32` |
| `generator_version` | `str` |
| `abi_doctor_version` | `str` |
| `spec_revision` | `str` |

`spec_revision` identifies the revision of the Arrow C Data Interface
documentation the case was written against, so that a case which becomes
invalid due to a specification change can be found.

Provenance describes *where the case came from*. It deliberately does **not**
describe where it ran: consumer name, version, build id, compiler,
architecture, OS and active sanitizers belong to the run report, not to the
case. A reproducer without the consumer version is not reproducible upstream,
but that information is a property of the observation, not of the input.

Hand-written cases use `seed = 0`, `rng_algorithm = ""`, `rng_version = 0`.

---

## 5. `ALLOCATIONS` (tag `0x0002`)

```
alloc_count   u32
allocation[alloc_count]
```

Allocation ids are implicit: an allocation's id is its index, dense from 0.

Serializing buffers as independent blocks would destroy aliasing. Two children
sharing one allocation would be reconstructed as two distinct allocations with
identical contents, and every test of ownership and lifetime built on them
would silently stop testing anything. The format therefore models **backing
allocations** and, separately, the **views** onto them (§7.2).

### 5.1 Allocation record

| Field | Type | Notes |
|---|---|---|
| `size_bytes` | `u64` | length of the backing allocation |
| `alignment` | `u32` | power of two, `1..=4096` |
| `fill` | `u8` | payload encoding, §5.2 |
| `reserved` | `u8[3]` | zero |
| payload | varies | per `fill` |

`alignment` is a property of the **allocation**, not of any buffer viewing it.
This is what lets a case say "this buffer is misaligned" precisely: allocate
with natural alignment and take a view at `byte_offset = 1` (§7.2). Encoding
alignment per buffer could not express that, because the misalignment would be
indistinguishable from a weaker allocator.

`size_bytes = 0` is legal, and is distinct from a buffer whose pointer is
`NULL`. A zero-length allocation reconstructs to a non-`NULL`, uniquely-owned,
zero-length allocation. Arrow's "length 0 with non-NULL but empty buffers" case
depends on that distinction.

### 5.2 Allocation payload encoding

Buffers are highly regular — zeroed, all-`0xFF` validity bitmaps, repeated
fixed-width values — and the format has a 10 KB budget. Each allocation payload
is therefore stored under one of four encodings:

| `fill` | Name | Payload | Encodes |
|---|---|---|---|
| 0 | `RAW` | `size_bytes` literal bytes | anything |
| 1 | `ZERO` | *(empty)* | all bytes zero |
| 2 | `RLE` | `u32 run_count`, then `run_count` × (`u32 run_len`, `u8 byte`) | byte runs |
| 3 | `PATTERN` | `u32 period`, then `period` bytes | a repeating byte pattern |

`PATTERN` repeats its `period` bytes exactly `size_bytes / period` times;
`period` must divide `size_bytes`, and `1 <= period <= min(256, size_bytes/2)`.
It is the encoding that matters most in practice: a buffer of one repeated
`int64` value is a 4-run-per-element disaster under `RLE` and 8 bytes under
`PATTERN`.

`RLE` run lengths must be non-zero, must sum to exactly `size_bytes`, and
**adjacent runs must not share a byte value** — otherwise two encodings would
exist for one payload.

**Canonical selection rule.** The encoder must choose the encoding with the
smallest total payload size; ties are broken by the lowest `fill` code. The
decoder re-runs the same selection over the decoded bytes and rejects the file
if a different encoding would have been chosen. This is what keeps §1.1 true
without trusting the writer, and it costs one linear pass.

Consequently `ZERO` always wins for an all-zero allocation (0 payload bytes),
and `size_bytes = 0` is always `ZERO`.

The `PATTERN` search is defined as: the smallest `period` in
`[1, min(256, size_bytes/2)]` that divides `size_bytes` and for which the
payload is periodic. Bounding it at 256 keeps the search cheap and the rule
stateable; a longer repeat falls back to `RAW`, which is correct, just larger.

---

## 6. `SCHEMA` (tag `0x0003`)

One `schema_node`, recursively:

| Field | Type | Notes |
|---|---|---|
| `format` | `str` | Arrow format string, e.g. `i`, `l`, `g`, `u`, `b`, `+s`, `w:16`, `d:38,10` |
| `presence` | `u8` | bit 0: `name` present; bit 1: `metadata` present; other bits zero |
| `name` | `str` | present iff bit 0 |
| `metadata` | §6.1 | present iff bit 1 |
| `flags` | `i64` | Arrow `ARROW_FLAG_*` bitmask |
| `n_children` | `u32` | value written into `ArrowSchema.n_children` |
| `child_count` | `u32` | number of `schema_node` records that follow |
| `children` | `schema_node[child_count]` | |
| `has_dictionary` | `u8` | |
| `dictionary` | `schema_node` | present iff `has_dictionary` |

`presence` exists because Arrow distinguishes a `NULL` `name` from an empty
one, and a `NULL` `metadata` pointer from metadata encoding zero pairs. A
length prefix alone cannot carry that distinction, and collapsing it would make
a set of real cases unrepresentable.

`n_children` and `child_count` are separate for the reason given in §7.1.

### 6.1 Metadata

```
kv_count  u32
kv[kv_count]:  key   bytes
               value bytes
```

Metadata is stored **decoded**, as a key/value list, and re-encoded into
Arrow's wire form by the reconstructor on the target host. Arrow's wire form
uses native-endian `int32` lengths; storing it verbatim would make the file
non-portable in exactly the way this format exists to avoid.

Keys and values are `bytes`, not `str`: Arrow's metadata encoding is
length-prefixed and permits arbitrary payloads, embedded NUL included.

---

## 7. `ARRAY` (tag `0x0004`)

One `array_node`, recursively:

| Field | Type | Notes |
|---|---|---|
| `length` | `i64` | |
| `null_count` | `i64` | `-1` means "not computed" |
| `offset` | `i64` | |
| `n_buffers` | `u32` | value written into `ArrowArray.n_buffers` |
| `buffer_count` | `u32` | number of `buffer_view` records that follow |
| `buffers` | `buffer_view[buffer_count]` | §7.2 |
| `n_children` | `u32` | value written into `ArrowArray.n_children` |
| `child_count` | `u32` | number of `array_node` records that follow |
| `children` | `array_node[child_count]` | |
| `has_dictionary` | `u8` | |
| `dictionary` | `array_node` | present iff `has_dictionary` |

### 7.1 Declared counts versus provided records

`n_buffers`/`buffer_count` and `n_children`/`child_count` are separate fields
because Corpus B1 contains "`n_buffers` inconsistent with the type" and
"`n_children` inconsistent with the schema". Those cases exist precisely to
make the declared count disagree with reality, so the format must be able to
state both numbers independently. A single field could not express the case at
all.

For class `A` and `B2` the decoder requires them to be equal — in those classes
a mismatch is a bug in the generator, not a case. For `B1` and `C` a mismatch
is permitted and is the point. See §7.3.

### 7.2 Buffer view

| Field | Type | Notes |
|---|---|---|
| `role` | `u8` | §7.4, descriptive only |
| `present` | `u8` | `0` = the pointer is `NULL` |
| `reserved` | `u16` | zero |
| `allocation_id` | `u32` | present iff `present` |
| `byte_offset` | `u64` | present iff `present` |
| `logical_length` | `u64` | present iff `present` |

A view names a backing allocation and a window into it. This expresses
directly:

```
buffer A -> allocation 3, offset 0
buffer B -> allocation 3, offset 16
buffer C -> allocation 3, offset 16      exact alias of B
buffer D -> allocation 4, offset 1       intentional misalignment
```

`present = 0` is a `NULL` buffer pointer, which is a legal and interesting
input (length 0 with `NULL` buffers; a validity buffer omitted when
`null_count == 0`), and is not the same as a view onto a zero-length
allocation.

**Containment invariant.** `byte_offset + logical_length <= size_bytes` of the
referenced allocation, and `allocation_id < alloc_count`. This is enforced for
every class, including B1, and it is a rule about *this tool*, not about Arrow.

The malformed-input case "buffer too short for `length + offset`" is expressed
by making the *allocation* small while the `ArrowArray.length` claims more
elements — the consumer then reads past a real, correctly-sized allocation,
which is the bug being hunted, and the fault lands in the consumer where it can
be observed. Letting a view run past its own allocation would instead make
`abi-doctor` construct an out-of-bounds pointer itself: the crash would be
ours, it would be attributed to the consumer, and under a sanitizer it would
abort the harness before the consumer was ever called. `abi-doctor` must be the
trustworthy party on both ends of every case it reports.

`logical_length` is what the case claims the buffer spans. It is descriptive:
the reconstructed `ArrowArray` carries only a pointer. It exists so the
observer can bound-check a consumer's reads and so the containment invariant
can be stated at all.

### 7.3 Class-dependent decode rules

Only these rules vary by class. Everything else in this document holds for
every file.

| Rule | `A` | `B1` | `B2` | `C` |
|---|---|---|---|---|
| `buffer_count == n_buffers` | required | optional | required | optional |
| `child_count == n_children` | required | optional | required | optional |
| containment invariant (§7.2) | required | required | required | required |
| schema and array tree shapes agree | required | optional | required | optional |

### 7.4 Buffer roles

| Value | Role |
|---|---|
| 0 | unspecified |
| 1 | validity |
| 2 | data |
| 3 | offsets |
| 4 | type ids |
| 5 | union offsets |
| 6 | sizes |
| 7 | views |
| 8 | variadic data |

The role is documentation for reports and for the observer. Arrow buffer
semantics come from the format string, and a case may deliberately mislabel a
buffer; the decoder does not cross-check the role against the format string.

---

## 8. `CALLSEQ` (tag `0x0005`)

```
op_count  u32
op[op_count]:  code  u16
               arg0  u32
               arg1  u32
```

The call sequence drives Corpus B2 and C: it is the script the harness follows
when exercising the case, rather than the default import/consume/release path.
Encoded and validated in M0; executed from M1 by `libabi/src/callseq.c`
against a consumer, with the lifecycle judged by the state machine in
`libabi/src/lifecycle.c`. An op a consumer cannot perform is refused by name,
never replaced by the default path. `USE_AFTER_RELEASE` is performed only in a
build configured with `-DABI_ENABLE_USE_AFTER_RELEASE=ON`, since it is undefined
behaviour in the worker's own process.

| Code | Op | `arg0` | `arg1` |
|---|---|---|---|
| 0 | `NOP` | — | — |
| 1 | `IMPORT_SCHEMA` | — | — |
| 2 | `IMPORT_ARRAY` | — | — |
| 3 | `STREAM_GET_SCHEMA` | — | — |
| 4 | `STREAM_GET_NEXT` | — | — |
| 5 | `STREAM_GET_LAST_ERROR` | — | — |
| 6 | `RELEASE_BASE` | — | — |
| 7 | `RELEASE_CHILD` | child index | — |
| 8 | `RELEASE_DICTIONARY` | — | — |
| 9 | `MOVE_STRUCT` | — | — |
| 10 | `USE_AFTER_RELEASE` | — | — |
| 11 | `EXPECT_EOF` | — | — |

Codes 7, 8 and 10 encode consumer behaviour the specification forbids. They are
only legal in a class `C` case; a class `A`, `B1` or `B2` file containing them
is rejected. Unused arguments must be zero.

---

## 9. `EXPECTED` (tag `0x0006`)

| Field | Type |
|---|---|
| `validation_level` | `u8` |
| `expected_outcome` | `u8` |
| `reserved` | `u16`, zero |
| `spec_clause` | `str` |
| `notes` | `str` |

`validation_level`: `0` none, `1` minimal, `2` default, `3` full. The levels
mirror nanoarrow's `ArrowArrayViewValidate()` so that reference-validator
results are directly comparable.

`expected_outcome`:

| Value | Name | Meaning |
|---|---|---|
| 0 | `ACCEPT` | a conforming consumer must accept and round-trip the data |
| 1 | `REJECT` | a conforming consumer must refuse, cleanly |
| 2 | `EITHER` | acceptance or clean refusal are both conforming |
| 3 | `UNSPECIFIED` | the specification does not say |

`EITHER` is not indecision. The specification permits a consumer not to support
every data type, non-zero offsets, or unaligned memory, provided it documents
the limitation; a clean refusal is then conforming and belongs in the
compatibility matrix, not in a bug report. `UNSPECIFIED` marks the cases whose
answer has to come from a maintainer.

Neither value excuses a crash, a leak, or silently divergent values. Those are
bugs under every outcome in this table.

`spec_clause` cites the clause the case rests on, quoted textually in
`docs/spec-citations.md`. A case whose expected outcome cannot be attributed to
a clause is a case whose expectation is an opinion.

---

## 10. Replay guarantees

```
same host:       .abicase -> case -> .abicase     byte identical
different host:  .abicase -> case                 same structural and
                                                  topological case, aliasing
                                                  preserved
```

"Topological" is the load-bearing word: the reconstructed graph of allocations
and views must have the same sharing structure on every host. Two views that
alias on one host alias on every host; two that do not, never do.

Byte-identical round-trip is verified per host. Cross-host equality is verified
by comparing the `payload_id` of a file re-encoded on each architecture, which
is exactly the check that catches a native integer serialized by accident.

### 10.1 What is not carried across endianness

**Scalar interpretation.** The guarantee above deliberately does not say
"logical". `.abicase` replays buffer bytes exactly, and Arrow buffer data is
native-endian, so a buffer holding `int32` values written on a little-endian
host describes *different scalars* when replayed on a big-endian one. The
encoding is portable, the structure is portable, the aliasing is portable; the
values a consumer reads out of the buffers are not.

That is a property of Arrow rather than of this format, and there is nothing to
fix in either. It is stated here because this is the document someone reads
before trusting a reproducer, and a reproducer attached to an issue may well be
opened on a host unlike the one that made it. What survives the trip is what the
cross-architecture check verifies: the encoded bytes, the structure, the
topology, the aliasing. What does not survive is what any given `int32` means.

A case whose buffers hold only single-byte or all-zero data is unaffected, and
so is a schema-only case. Whether a particular case is affected is decidable
from its schema — any buffer of a type wider than one byte — and belongs in the
run report, alongside the architecture the run happened on.

### 10.2 Why the generating host's endianness is not recorded

It would be natural to add the generating host's endianness to `PROVENANCE` so a
case could be flagged when replayed somewhere it will not mean the same thing.
It is not recorded, and must not be.

`PROVENANCE` is inside the payload, and the payload is the case identity (§2.3).
A field holding the generating host's endianness would make the same case encode
to *different bytes*, and therefore to a different `payload_id`, on a big-endian
host than on a little-endian one. That directly contradicts the cross-host
guarantee above and would break `tools/cross-arch-check.sh`, which compares the
two encodings byte for byte. Excluding the field from the digest instead would
give one case two valid encodings, which §1.1 forbids for the same reason.

The information is real and worth having; it is a property of the *observation*,
not of the case, which is the same reason §4 keeps consumer version, compiler,
architecture and sanitizers out of `PROVENANCE`. It belongs in the run report.

---

## 11. Limits

Fixed limits, enforced by the decoder, so that a corrupt or hostile file cannot
make the decoder allocate unboundedly before it fails.

| Limit | Value |
|---|---|
| `total_size` | 64 MiB |
| `alloc_count` | 4096 |
| allocation `size_bytes` | 16 MiB |
| `alignment` | power of two, `1..=4096` |
| tree depth (schema or array) | 64 |
| node count per tree | 4096 |
| `str` / `bytes` length | 1 MiB |
| `op_count` | 4096 |
| `run_count`, `period` | bounded by `size_bytes` |

These bound the decoder, not the Arrow structures a case may describe: an
`ArrowArray.length` of `INT64_MAX` is perfectly encodable, and is a case.
