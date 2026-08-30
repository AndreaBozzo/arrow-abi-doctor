# Bounded conformance model

`coverage_model_ver` = 2
Status: **normative.** Frozen before the first corpus run that claims coverage
over it. Changes are governed by §8.

The real Arrow input space is not enumerable. What can be made exhaustive is a
finite model of equivalence classes, declared **before** the runs that claim
coverage over it. Declaring it afterwards would make M2-B unfalsifiable, which
is why the milestone treats publication timing as a blocking constraint rather
than a documentation task.

The model has seven dimensions (design spec §7). §1 freezes the exact
enumeration of each and the reason for every class boundary; §2 gives the
constraints that carve the invalid combinations out of the raw product; §3
computes N. `tools/coverage_matrix.py` is the executable form of §1–§3, and
`--check` fails if it and this file have drifted apart — so N below is a number
this repository computes, not one someone asserted.

---

## 0. What this model covers

**Corpus A only** — conforming producers, valid but rare input. Every
combination in the product describes an Arrow array a correct producer is
permitted to hand over.

Corpus B1 (invalid structure or data), B2 (invalid protocol or lifecycle) and C
(consumer misuse) are **not** part of this model and are not counted in N. They
are not a product of equivalence classes; they are an enumerated list of
specific defects, each carrying a `spec_clause`. Reporting them as coverage over
a bounded model would be a category error — "exhaustive over the defects we
thought of" is not a measurement.

Consequently every case in the model has an `expected_outcome` of `ACCEPT` or
`EITHER`, never `REJECT`. `EITHER` is the correct outcome for the classes the
specification lets a consumer decline provided it documents the limitation —
non-zero offsets and non-natural alignment, principally. §5 says which.

---

## 1. The dimensions

### 1.1 `type` — 5 classes

| Class | Format string | Buffers |
|---|---|---|
| `int32` | `i` | validity, data (4 B/elem) |
| `int64` | `l` | validity, data (8 B/elem) |
| `float64` | `g` | validity, data (8 B/elem) |
| `utf8` | `u` | validity, offsets (`int32`), data (bytes) |
| `bool` | `b` | validity, data (1 bit/elem) |

This is the v0 feature set from `README.md`, unchanged. The boundaries:

- **`int32` and `int64` both present** because they differ in element width, and
  width confusion in offset arithmetic is a defect a single integer width cannot
  expose. Their presence is also what makes the `+4` alignment class mean
  something (§1.6).
- **`float64`** is the only type whose values have more than one bit pattern per
  logical value (`-0.0`, NaN payloads). It carries no extra layout, so it costs
  nothing structurally, and it is the type the `logical_digest` normalization
  rules (issue #7) exist for.
- **`utf8`** is the only type with three buffers and the only one with an offsets
  buffer. Offset-buffer validation for sliced arrays is a known live defect area
  (arrow-nanoarrow #626), and it is unreachable without a variable-length type.
- **`bool`** is the only type whose *data* buffer is bit-packed, so it is the
  only type for which `ArrowArray.offset` is a **bit** offset into the data
  rather than a byte offset. A consumer that handles offsets by byte arithmetic
  is correct for the other four and wrong for this one.

`float32`, the other integer widths, decimal, date/time, binary and
fixed-size-binary are omitted: each adds a row without adding a *layout* the five
above do not already reach. Dictionary is M3; nested, run-end, `string_view` and
`list_view` are out of scope (§7).

### 1.2 `length-class` — 4 classes

| Class | Value | Why this boundary |
|---|---|---|
| `zero` | 0 | The degenerate array. Validity and `null_count` conventions collapse here, and "length 0 with non-`NULL` but empty buffers" is a documented Arrow case that exists only at this length. |
| `one` | 1 | The smallest array in which a value and a validity bit both exist. Separates "handles the empty case" from "handles the singular case", which are different off-by-one bugs. |
| `small` | 9 | Deliberately **not** a multiple of 8: a validity bitmap spans two bytes with a partial trailing byte, so trailing-bit handling is exercised. Chosen as 9 rather than any other non-multiple for how it lands against the offset-classes of §1.3; the table there gives the five windows. |
| `medium` | 1024 | A multiple of 8 and of 64, larger than a cache line and than any plausible SIMD block, so a vectorized consumer takes its wide path rather than its scalar tail. Bounded at 1024 rather than larger so every case stays inside the format's ~10 KB budget under the canonical fill encodings (format §5.2): a regular buffer of 1024 elements encodes to a few bytes under `PATTERN` or `ZERO`, and the worst case in the model — a `utf8` offsets buffer, monotonic and therefore incompressible — is (9 + 1024 + 1) × 4 = 4136 bytes of `RAW`, at the largest offset-class. |

There is no "large" class. A length exceeding `INT32_MAX` elements is a real and
interesting case, but it is one that cannot be *allocated* on the hosts this runs
on, so it belongs to Corpus B1 as a declared-but-unbacked length, not here.

### 1.3 `offset-class` — 5 classes

`ArrowArray.offset`, in elements: **0, 1, 7, 8, 9**.

This freezes the sketch's "0, 1, 7, byte-boundary ±1" — 8 is the byte boundary in
the validity bitmap, 7 and 9 are one below and one above it.

- **`0`** — the nominal path. A consumer that ignores `offset` entirely passes
  this class and only this class, which is what makes the other four
  load-bearing.
- **`1`** — the smallest slice. The failure mode here is silent: a consumer that
  drops the offset returns the wrong values rather than crashing, which is the
  divergence class the dual digest (issue #7) exists to catch.
- **`7`** — one bit below a byte boundary in the validity bitmap, and for `bool`
  in the data bitmap as well. Every bitmap read spans a byte boundary.
- **`8`** — exactly one byte. This is the class that discriminates *byte-aligned
  offset support* from *arbitrary offset support*: a consumer that shifts bitmaps
  by whole bytes only is correct at 0 and 8 and wrong at 1, 7 and 9. Without it,
  that consumer is indistinguishable from one that is simply broken.
- **`9`** — one bit above the byte boundary, so the first byte of the window is
  partial at the head rather than at the tail.

Every buffer is allocated for `offset + length` elements, so a non-zero offset
never makes a case invalid; it makes it a slice.

Against `length-class` `small` (9), the five classes give five distinct validity
windows — the reason 9 was picked for that class:

| `offset` | Window, in bits | First byte | Trailing byte |
|---|---|---|---|
| 0 | [0, 9) | whole | partial, 1 bit |
| 1 | [1, 10) | partial | partial, 2 bits |
| 7 | [7, 16) | partial, 1 bit | **none** — ends exactly on a boundary |
| 8 | [8, 17) | whole, second byte | partial, 1 bit |
| 9 | [9, 18) | partial, second byte | partial, 2 bits |

Within a byte, `offset = 8` repeats 0 and `offset = 9` repeats 1; they differ
only in which byte of the bitmap the window starts in, which is exactly what
separates a consumer that shifts by whole bytes from one that does not. And
`offset = 7` is the only class here that ends flush with a byte boundary, so it
is the one that does **not** exercise the trailing-bit path — worth having for
the same reason as its opposite.

### 1.4 `null-pattern` — 4 classes

Defined over **logical** indices, that is relative to `offset`, so the physical
bit positions move with `offset-class`. That interaction is the point of having
both dimensions.

| Class | Definition | Why |
|---|---|---|
| `none` | no null slots; `null_count = 0` | The common path, and the only pattern under which the validity buffer may legally be omitted (§1.5). |
| `all` | every slot null; `null_count = length` | The all-null axis. Consumers frequently have a short-circuit path here, and a short-circuit path is a path that skips validation. |
| `alternating` | null at even logical index | Dense and regular, so it is the pattern a bit-parallel path handles; it also guarantees both a null and a non-null in every byte of the bitmap. |
| `sparse` | exactly one null, at logical index `length - 1` | The single null in the last slot, which under a non-zero offset and a non-multiple-of-8 length lands in the partial trailing byte — the bit most often mishandled. |

`null_count` always carries the true count. `null_count = -1` ("not computed") is
legal and is **not** in the model; see §7.

The patterns are deterministic. No dimension of this model is random.

### 1.5 `buffer-state` — 4 classes

| Class | Definition | Why |
|---|---|---|
| `normal` | every buffer of the type is present, in its own backing allocation, sized for `offset + length` elements with a floor of one byte | The baseline. The floor keeps the allocation non-degenerate at `length = 0`, which is what makes it distinguishable from `empty`. |
| `empty` | every buffer pointer is non-`NULL` over a backing allocation of `size_bytes = 0`, **except an offsets buffer, which keeps its one mandatory entry** | Arrow distinguishes a `NULL` buffer from a zero-length one, and the format does too (format §5.1). This is the "length 0 with non-`NULL` but empty buffers" case; a consumer that conflates the two fails exactly here. The offsets exception is not a softening: the columnar format says an offsets buffer "contains `length + 1` signed integers", and the C Data Interface places the obligation on the producer — "The producer MUST ensure that each contiguous buffer is large enough to represent `length + offset` values encoded according to the Columnar format specification". A zero-byte offsets buffer would therefore describe an **invalid** array, which belongs to Corpus B1 and not to a model whose every case is valid Arrow (§0). For `utf8` it is the *values* buffer that is empty here. |
| `omitted-validity` | the validity buffer pointer is `NULL`; the others are `normal` | Legal when there are no nulls. It is adjacent to Arrow #40898 — release callback never invoked on import when a buffer pointer was `NULL` — but not the same shape: that report concerned a `NULL` **non**-validity buffer, which this model does not construct (§7). Note the asymmetry this makes visible: `none` nulls has *two* legal representations — omitted validity, and a present all-ones bitmap — and they are separate cells here. |
| `aliased` | all buffers of the array are views into **one** backing allocation, at distinct correctly-placed offsets | What a producer with an arena actually does. Aliasing is preserved by the format by construction (allocations and views are modelled separately) and is verified by pointer comparison, so a consumer that frees per buffer, or that assumes buffers are disjoint, is caught here. |

The four are mutually exclusive by construction; a case cannot be both `aliased`
and `omitted-validity`.

### 1.6 `alignment-class` — 3 classes

Realized as the byte offset of the *view* within a 64-byte-aligned allocation,
which is the only way to state misalignment precisely (format §5.1): allocate
with natural alignment, then take the view one or four bytes in.

| Class | View starts at | What it breaks |
|---|---|---|
| `natural` | +0 from a 64-byte-aligned allocation | nothing; Arrow's recommended allocation alignment |
| `+1` | +1 byte | every element type wider than a byte, and every word-wise bitmap read |
| `+4` | +4 bytes | `int64` and `float64` only; `int32` and both bitmaps stay aligned |

Two classes of breakage rather than one is the whole reason `+4` exists: it
separates a consumer that requires natural alignment *for the element width it is
reading* from one that requires 8-byte alignment unconditionally, and those are
different documented limitations.

For `bool` and for the `utf8` data buffer the element granularity is a bit or a
byte, so a shifted view cannot produce a *type* misalignment. The class is kept
for them anyway: those buffers are read word-wise in practice, so it tests the
word-wise path rather than the element type.

### 1.7 `lifecycle` — 5 classes

| Class | Sequence | Requires |
|---|---|---|
| `direct` | import schema, import array, consume, release | — |
| `moved` | the struct is moved to a new location and the source's `release` nulled, per Arrow move semantics, then consumed and released from the new location | — |
| `streamed` | `get_schema`, `get_next` → one batch, `get_next` → EOF, release | C Stream Interface (issue #4) |
| `early-release` | the stream is released without `get_next` ever being called | issue #4 |
| `EOF` | the first `get_next` returns the already-released array that is the EOF sentinel; no batch is ever delivered | issue #4 |

`early-release` and `EOF` deliver a **schema but no array**. The five
data-bearing dimensions are therefore inapplicable to them, which §2 handles
explicitly rather than by letting one case be counted 1880 times.

`streamed` is not redundant with `direct`: the array arrives by a different
ownership path, with the stream owning the batch until `get_next` hands it over.

---

## 2. How the product is generated

The raw product of §1 is 5 × 4 × 5 × 4 × 4 × 3 × 5 = **24000** tuples. Most are
not descriptions of a valid Arrow array, or are duplicates of one another. Eight
named constraints carve them out. Each is a predicate that must hold; a tuple is
rejected by the **first** constraint it fails, which is how the per-constraint
counts in §3 are attributed.

| # | Name | Predicate | Why |
|---|---|---|---|
| 1 | `data-free-lifecycle` | `lifecycle ∈ {early-release, EOF}` ⟹ the five data dimensions sit at their sentinel (`zero`, `0`, `none`, `normal`, `natural`) | No array is delivered, so those dimensions describe nothing. Pinning them keeps the tuple seven-wide while counting the case once. |
| 2 | `null-pattern-needs-elements` | `length = zero` ⟹ `nulls = none` | An array with no slots has no null slots. |
| 3 | `single-element-null-collapse` | `length = one` ⟹ `nulls ∈ {none, all}` | At one element, `all`, `alternating` and `sparse` all describe the same array — a single null slot. Three names for one array would inflate N without adding a case. |
| 4 | `empty-buffers-need-zero-length` | `buffers = empty` ⟹ `length = zero` | A zero-byte allocation cannot back an array with elements in it. A non-zero length over zero-length buffers is a genuine case, but an *invalid* one: Corpus B1, not this model. |
| 5 | `empty-buffers-need-zero-offset` | `buffers = empty` ⟹ `offset = 0` | The format's containment invariant is `byte_offset + logical_length ≤ size_bytes`, and `size_bytes` is 0 here. A view past its own allocation would make `abi-doctor` the party constructing an out-of-bounds pointer. |
| 6 | `empty-buffers-have-no-alignment` | `buffers = empty` ⟹ `alignment = natural` | For four of the five types there are no bytes to place, so the three alignment classes reconstruct identically. For `utf8` the class retains the one mandatory offsets entry (§1.5), whose placement at +0, +1 and +4 is already exercised by every `normal` `utf8` case, so the three classes still add nothing here. |
| 7 | `omitted-validity-needs-no-nulls` | `buffers = omitted-validity` ⟹ `nulls = none` | Arrow permits the validity buffer to be absent only when there are no nulls. Absent-with-nulls is Corpus B1. |
| 8 | `aliasing-needs-elements` | `buffers = aliased` ⟹ `length ≠ zero` | With no elements every buffer view is a zero-length window, and sharing a backing allocation has no observable consequence — the reconstruction is indistinguishable from `normal`. |

Constraints 3, 6 and 8 merge equivalence classes; 2, 4, 5 and 7 exclude invalid
arrays; 1 is bookkeeping. A constraint that removes nothing is either wrong or
redundant, so `tools/coverage_matrix.py --check` fails if any of the eight has a
count of zero.

---

## 3. N

N = **5650**

Reproduce with `python tools/coverage_matrix.py`:

```
raw product        = 24000

removed by constraint (first one to reject wins):
  data-free-lifecycle                9590
  null-pattern-needs-elements        2700
  single-element-null-collapse       1800
  empty-buffers-need-zero-length     2250
  empty-buffers-need-zero-offset      180
  empty-buffers-have-no-alignment      30
  omitted-validity-needs-no-nulls    1575
  aliasing-needs-elements             225

N = 5650
```

By lifecycle:

| Lifecycle | Cases |
|---|---|
| `direct` | 1880 |
| `moved` | 1880 |
| `streamed` | 1880 |
| `early-release` | 5 |
| `EOF` | 5 |

1880 is 5 types × 376 valid combinations of the five data dimensions. The two
data-free lifecycles contribute one case per type, which is the whole of their
contribution and is why they are worth five cases each rather than 1880.

By buffer-state and by length-class:

| Buffer-state | Cases | Length-class | Cases |
|---|---|---|---|
| `normal` | 2485 | `zero` | 475 |
| `empty` | 15 | `one` | 1125 |
| `omitted-validity` | 900 | `small` | 2025 |
| `aliased` | 2250 | `medium` | 2025 |

Size, measured rather than estimated, over the `direct` third of the model that
`tools/gen_corpus.py` generates today: 1880 cases, 4.7 MB, mean 2489 bytes,
largest 8891 — an `int64` or `float64` `medium` case at a non-zero
offset, whose data buffer is incompressible under every canonical fill
encoding. Every case is inside the 10 KB reproducer budget, but the full
corpus is on the order of 14 MB rather than the "few megabytes" a 1 KB
average would suggest. It is
**generated, not committed**: the repository holds the generator, this model, and
the minimized reproducers for the cases that actually found something.

---

## 4. What a coverage claim means

Each of the N cells, per consumer, is in exactly one state:

| State | Meaning |
|---|---|
| `agree` | the case ran, and the consumer's outcome matched the other voices |
| `disagree` | the case ran, and the outcome differed — a signal to investigate, not yet a verdict |
| `not-run` | the case was not executed, for any reason, including a crash that ended the worker |
| `inexpressible` | the case could not be constructed at all |

**N/N is claimable only when `not-run` and `inexpressible` are both zero.** A
cell that was never executed is a hole in the measurement, not a pass; a report
that counts it as one is the failure this whole document exists to prevent.

The only claim this model licenses:

> exhaustive over the finite equivalence-class matrix defined in
> `docs/coverage-matrix.md`, `coverage_model_ver` 2

Always with the version. N is a function of the model version, so a coverage
figure quoted without one is not comparable to anything.

---

## 5. Expected outcomes within the model

Every case here is valid Arrow, so no case in the model may crash, leak, exhibit
undefined behaviour, or return divergent values. Those are bugs under every
outcome, and the "documented limitation" defence does not reach them.

A **clean rejection** is conforming for the classes the specification lets a
consumer decline, provided the limitation is documented:

| Class | Outcome | Destination if rejected |
|---|---|---|
| `offset-class` ≠ 0 | `EITHER` | compatibility matrix, or the undocumented-limits map |
| `alignment-class` ≠ `natural` | `EITHER` | as above |
| any `type` | `EITHER` | as above — a consumer need not support every data type |
| everything else | `ACCEPT` | a rejection here is a defect |

A rejection recorded against an `EITHER` cell counts as `agree` for coverage and
becomes an entry in the compatibility matrix; it is not a disagreement. Whether
the limitation is documented is what routes it between the two maps, and that is
a judgement made per consumer, once, not per case.

---

## 6. Random fuzzing

Reported **separately**, and never presented as coverage. Fuzzing explores a
space that is not enumerable, so it produces findings, not a denominator. A
campaign's output belongs in the report as "additional evidence, N executions, M
findings" — never as a percentage, and never merged into the N/N figure above.

The same applies to buffer *contents*. Values are not a dimension of this model:
the generator fills buffers deterministically from the tuple itself — from
the case id would be circular, since the id is a digest over the payload the
buffers are part of (format 2.3) — and the model makes no claim about value
coverage. Value-space exploration is fuzzing's job.

---

## 7. Explicitly out

Named here so a report can say which surface was explored and which was not.
Everything in this list is a surface about which the model makes **no claim** —
not a surface believed to be correct.

| Out | Reason |
|---|---|
| Corpus B1, B2, C | not equivalence-class products (§0) |
| dictionary-encoded types | M3; enabled in Corpus A only then |
| nested types — list, struct, union, map | out of scope until after M2 |
| run-end, `string_view`, `list_view` | out of scope until after M2 |
| `null_count = -1` ("not computed") | legal and interesting, and a candidate for a later `coverage_model_ver`. Doubling the model for it is not worth the run time before there is a differential pair to run it against. |
| schema-surface variation — `flags`, metadata presence, `NULL` versus empty `name` | a real and separate defect class: DuckDB #21691 is a format-string crash reached before any data exists. It needs its own enumeration, over the schema rather than over the array, and this matrix is not it. |
| a `NULL` **non**-validity buffer | the shape of Arrow #40898. Legal only at `length = 0`; at any other length the array is invalid and it is Corpus B1. Reaching the legal half needs a fifth `buffer-state` that NULLs a non-validity buffer, which is a candidate for a later `coverage_model_ver`. |
| element widths other than the five types in §1.1 | §1.1 |
| lengths beyond `INT32_MAX` | not allocatable on the hosts this runs on; belongs to B1 as a declared-but-unbacked length |
| alignment classes other than +1 and +4 | those two break different sets of the five types; a third would break the same set as one of them |
| `validation_level` as a dimension | it is a property of how a case is *checked*, not of the case; it is recorded per case in `EXPECTED` and compared against the reference validator |
| concurrency, several consumers over one case at once | the C Data Interface offers no ordering guarantees to test against |

---

## 8. Changing the model

The model is frozen. It may still change — but visibly, because the value of a
pre-declared model lies entirely in the fact that it was declared first.

- Any change to §1, §2 or §3 bumps `coverage_model_ver` and updates
  `tools/coverage_matrix.py` in the same commit. CI runs `--check`, so the two
  cannot drift.
- **A class may not be removed because it produced a disagreement.** That is the
  one change this document exists to make impossible to do quietly. A class that
  is genuinely inexpressible or genuinely redundant may be retired, in the log
  below, naming the run that revealed it and the reason.
- A coverage figure computed under one version is not comparable to one computed
  under another. Re-run; do not re-scale.

### Change log

| Version | Date | Change |
|---|---|---|
| 1 | 2026-08-24 | Initial freeze. Enumeration, constraints and N as above. |
| 1 | 2026-08-29 | Editorial, no version bump: §3's size estimate replaced by measured figures from the first generator run, and §6's buffer-fill rule restated without its circular reference to the case id. No dimension, constraint or count changed, so figures computed before and after this edit remain comparable. |
| 2 | 2026-08-29 | §1.5's `empty` class described an **invalid** array for `utf8`: a zero-byte offsets buffer, where the columnar format requires `length + 1` entries and the C Data Interface makes that the producer's obligation. Class A is conforming input, so the cell did not belong in the model as written. The class now keeps the single mandatory offsets entry and empties the values buffer instead; §2's constraint 6 keeps its predicate and gains an accurate justification. **N is unchanged at 5650** — no class was added, removed or merged, only one class's realization corrected — but the version moves because §1 changed and §8 does not make exceptions. Found by running the first generated corpus past two consumers: arrow-rs read out of bounds on the case, which was our defect and not its own. |
| 2 | 2026-08-30 | Editorial, no version bump: §4's licensed-claim quotation still named version 1 after the bump above, and §7 offered two exclusions as "a candidate for `coverage_model_ver` 2" — a version that has since shipped without them. Neither is a dimension, a constraint or a count, so figures are unaffected. `tools/coverage_matrix.py --check` now reads the §4 quotation too: the header was gated and the sentence stating the claim was not, which is how it drifted. |
