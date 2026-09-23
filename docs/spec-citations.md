# Specification citations

Every case class rests on a clause of the Arrow specification, quoted verbatim
here. A case whose expected outcome cannot be attributed to a clause is a case
whose expectation is an opinion, and it is not committed.

The `.abicase` `EXPECTED` section carries a `spec_clause` string. It must be
the id of an entry below — a `###` heading, matched exactly — and
`tools/check_b1.py` fails on any case whose clause names no entry.

Quotes are from the reStructuredText sources of the Arrow documentation at
**apache/arrow@13f20f2ae158d8d4df68a6ae362a2660a5fd4419**, read 2026-09-23. The
only rendering applied is that role markup (`:c:member:\`X\``, `:ref:\`X <y>\``)
is shown as its text `X`; the words are unchanged.

## Claims that must always carry a citation

| Claim | Needs |
|---|---|
| "a conforming consumer must accept this" | the clause that makes it valid |
| "a conforming consumer must reject this" | the clause it violates |
| "the specification does not say" | evidence of the gap, then a maintainer question |

## Framing rule

Corpus B1 is presented as **robustness against trusted-but-invalid producers,
and against validation boundaries** — never as hardening against malicious
producers. The specification now says so normatively, in its security model
(`sec-cdi-untrusted`, `sec-cdi-validate` below): a C Data Interface structure
from an untrusted producer must never be consumed, because nothing can guard
against it, and validation is recommended precisely because a *trusted*
producer can have bugs. A project claiming to fuzz a trust boundary the
specification says does not exist would be answered by pointing at those
sentences.

What a validator owes is the third security clause (`sec-validation-api`): a
well-defined error, not a crash. That is the property the reference validator is
held to (`refval/README.md`), not agreement with any expected verdict.

## Entries

### cdi-null-buffers

`format/CDataInterface.rst`, "ArrowArray.buffers".

> The buffer pointers MAY be null only in two situations:
>
> 1. for the null bitmap buffer, if ArrowArray.null_count is 0;
> 2. for any buffer (including variadic buffers), if the size in bytes of the
>    corresponding buffer would be 0.

Used by B1 `validity-absent-with-nulls`, `data-buffer-null`.

### cdi-buffer-size

`format/CDataInterface.rst`, "ArrowArray.buffers".

> The producer MUST ensure that each contiguous buffer is large enough to
> represent `length + offset` values encoded according to the Columnar format
> specification.

An obligation on the producer, and one no consumer can check: the interface
carries no buffer sizes. The B1 cases that break it are therefore
`UNSPECIFIED` — what a consumer does with them is recorded, never judged.

Used by B1 `data-buffer-too-small`, `last-offset-beyond-values`,
`length-beyond-int32`.

### cdi-alignment

`format/CDataInterface.rst`, "ArrowArray.buffers".

> It is recommended, but not required, that the memory addresses of the
> buffers be aligned at least according to the type of primitive data that
> they contain. Consumers MAY decide not to support unaligned memory.

Why the `+1` and `+4` alignment classes of `docs/coverage-matrix.md` §1.6 are
`EITHER` cells, and the clause behind apache/arrow-nanoarrow#945.

### cdi-children

`format/CDataInterface.rst`, "ArrowArray.n_children" and "ArrowArray.children".

> Mandatory. The number of children this array has. The number of children is
> a function of the data type, as described in the Columnar format
> specification.

> Optional. A C array of pointers to each child array of this array. There
> must be ArrowArray.n_children pointers.

Used by B1 `children-fewer-than-declared`.

### cdi-null-count

`format/CDataInterface.rst`, "ArrowArray.null_count".

> Mandatory. The number of null items in the array. MAY be -1 if not yet
> computed.

Used by B1 `null-count-disagrees`.

### col-offsets-length

`format/Columnar.rst`, "Variable-size Binary Layout".

> The offsets buffer contains `length + 1` signed integers (either 32-bit or
> 64-bit, depending on the data type), which encode the start position of each
> slot in the data buffer.

Also the clause that made `coverage_model_ver` 1's `empty` realization invalid
(`docs/coverage-matrix.md` §8, change log). Used by B1 `offsets-buffer-empty`.

### col-offsets-monotonic

`format/Columnar.rst`, "Variable-size Binary Layout".

> Offsets must be monotonically increasing, that is `offsets[j+1] >=
> offsets[j]` for `0 <= j < length`, even for null slots. This property
> ensures the location for all values is valid and well defined.

Used by B1 `offsets-decreasing`.

### col-dictionary-indices

`format/Columnar.rst`, "Dictionary-encoded Layout".

> When a field is dictionary encoded, the values are represented by an array of
> non-negative integers representing the index of the value in the dictionary.

Used by B1 `dictionary-index-out-of-range`.

### sec-cdi-untrusted

`format/Security.rst`, "C Data Interface", "Advice for users".

> You should **never** consume a C Data Interface structure from an untrusted
> producer, as it is by construction impossible to guard against dangerous
> behavior in this case.

### sec-cdi-validate

`format/Security.rst`, "C Data Interface", "Advice for implementors".

> When consuming a C Data Interface structure, you can assume that it comes from
> a trusted producer, for the reason explained above. However, it is still
> **recommended** that you validate it for soundness (for example that the right
> number of buffers is passed for a given datatype), as a trusted producer can
> have bugs anyway.

### sec-validation-api

`format/Security.rst`, "Columnar Format", "Invalid data", "Advice for
implementors".

> A typical validation API must return a well-defined error, not crash, if the
> given Arrow data is invalid; it must always be safe to execute regardless of
> whether the data is valid or not.

### C Data Interface: Structure definitions

`format/CDataInterface.rst`, "Structure definitions".

> The following free-standing definitions are enough to support the Arrow C
> data interface in your project.

The clause every Corpus A case carries. It predates the id convention above —
it is a section title, not an id — and it stays, because the clause is part of
the payload and renaming it would change every Corpus A case id. It names the
structures, not a validity argument: why each Corpus A cell is valid Arrow is
argued cell class by cell class in `docs/coverage-matrix.md` §1, and every
buffer is sized per `cdi-buffer-size` and `col-offsets-length`.
