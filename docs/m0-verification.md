# M0 verification record

M0 asks for four things:

1. `.abicase` with a canonical encoding, an alias-aware allocation/view model,
   and a provenance block;
2. idempotent round trip on the same host, byte for byte;
3. logical, structural and topological equivalence verified **between two
   architectures**;
4. no adapters, no observer.

This file records how each was measured, so that "verified" means something a
reader can re-run rather than something the author asserts.

Re-run everything with:

```sh
cmake -S . -B build -G Ninja -DABI_WERROR=ON && cmake --build build
ctest --test-dir build --output-on-failure
./tools/cross-arch-check.sh
```

---

## 1. Format

Specified in [abicase-format.md](abicase-format.md), implemented in `libabi/`.
The provenance block carries seed, RNG algorithm and version, generator version,
abi-doctor version and spec revision. Consumer, compiler, architecture, OS and
active sanitizers deliberately live in the *run report* instead: a reproducer
without the consumer version is not reproducible upstream, but that is a
property of the observation, not of the input.

The allocation/view model is §5 and §7.2 of the format spec. It is what makes
aliasing survive replay, and the reason is worth restating: serializing buffers
as independent blocks would turn two children sharing one allocation into two
allocations holding equal bytes. Nothing would look wrong. Every ownership and
lifetime test built on that case would simply stop testing anything.

## 2. Byte-identical round trip

`test_roundtrip` encodes, decodes and re-encodes; the two byte strings must be
equal. Applied to the rich fixture, a schema-only case, a class B1 case with
deliberately inconsistent counts, a class C case carrying misuse ops, and a B1
short-buffer case.

The property is total rather than best-effort because the decoder is **strictly
canonical**: it rejects any input a conforming encoder would not have produced —
non-ascending or duplicated sections, non-zero reserved bytes, a non-minimal
allocation payload encoding, trailing bytes inside a section or after the last
one. Given that, `decode(b) = c` implies `encode(c) = b`.

The non-minimal encoding rule is enforced by re-running the encoder's own
selection function over the decoded bytes and comparing both the chosen code
*and* its parameters. Comparing only the code is not enough: `"abababab"` is
periodic with period 2 and with period 4, both pass the structural checks, and a
file declaring the non-minimal one would decode correctly and re-encode
differently.

## 3. Cross-architecture equivalence

Verified by `tools/cross-arch-check.sh`, which builds for **s390x** — genuinely
big-endian — and runs it under `qemu-user-static` alongside the native build.

| Check | Result |
|---|---|
| cross binary really is big-endian | `ELF 64-bit MSB executable, IBM S/390` |
| full test suite on big-endian | 123 checks, 0 failures |
| both hosts encode the same case to the same bytes | identical, 908 bytes |
| case id agrees across hosts | `af3ded84db715f0957eec7188f9859ec` |
| each host replays the other's file | ok, both directions |
| structure and topology dumps agree | identical, 55 lines |
| aliasing preserved | 1 aliased allocation, both hosts |
| committed golden fixture replays on big-endian | ok |

Three platforms produce the identical case id:

| Platform | Endianness | Result |
|---|---|---|
| Windows 11 / MinGW-w64 UCRT gcc 16.1, x86-64 | little | 123 checks, 0 failures |
| Linux / gcc 13.3, x86-64 | little | 123 checks, 0 failures |
| Linux / gcc 13.3 cross, s390x under qemu | **big** | 123 checks, 0 failures |

### The check discriminates

A test that has never failed is not a test. The cross-architecture check was
validated by injecting the exact defect it exists to catch — replacing the
shift-based `abi_store_u32` with a native `memcpy`:

```c
static inline void abi_store_u32(uint8_t *p, uint32_t v) {
  memcpy(p, &v, sizeof(v)); /* INJECTED DEFECT: native byte order */
}
```

| Where | Result with the defect |
|---|---|
| x86-64 test suite | **123 checks, 0 failures** — completely invisible |
| s390x test suite | 91 checks, **17 failures** |
| cross-host byte comparison | files differ at byte 9 (the `bom` field); the two hosts produce different case ids |

The two halves were checked separately: the byte-comparison step discriminates
on its own, not only via the test-suite step that runs before it. The
big-endian run also produced the diagnostic the format was designed for —
`byte-order marker is 0xfffe0000, expected 0x0000feff (file written
big-endian)` — which is the `bom` header field doing its job.

This is the whole argument for the cross-architecture requirement: on
little-endian hardware the defect is not merely undetected, it is undetectable.

## 4. Robustness

| Sweep | Scale | Result |
|---|---|---|
| truncation — every proper prefix must be refused | 908 prefixes | 0 accepted |
| single-bit corruption — refuse, or decode and re-encode identically | 7 264 flips | 0 unstable |

The corruption sweep accepts 2 flips as valid variants, and that is correct:
they are the two low bits of the class byte, which turn a class A case into a
valid class B1 or B2 one. Both re-encode to exactly the corrupted bytes, which
is the invariant under test — a file that decoded but re-encoded differently
would mean the format is not canonical.

Both sweeps are only meaningful under a sanitizer. Clean under:

| Tool | Result |
|---|---|
| ASan + UBSan (`-fno-sanitize-recover=all`) | clean |
| Valgrind memcheck, `--leak-check=full --errors-for-leak-kinds=all` | clean, exit 0 |

The arena allocator is why leaks are structurally unlikely: everything reachable
from a case lives in one arena and `abi_case_free()` releases all of it in a
single call, so there is no partially-built tree to leak on an error path. That
matters more than usual here, because this tool's own memory behaviour is what
it will be reporting about other people's.

## 5. Size budget

The format targets cases small enough to attach to an issue. The rich fixture —
five allocations, a dictionary, aliasing, metadata, a call sequence —
encodes to **908 bytes**, against a 10 KB budget, asserted by `test_size_budget`.

That comes from the canonical allocation payload encoding (format §5.2): a
zeroed 64-byte buffer costs 0 payload bytes, an all-`0xFF` validity bitmap costs
5, and a two-run buffer costs 14.

## What M0 does not claim

- No adapter has consumed a case. Nothing here says anything yet about
  Arrow C++, DuckDB, arrow-rs or any other engine.
- No lifecycle has been observed. The `CALLSEQ` section is encoded and
  validated, not executed; that is M1, when the observer exists.
- `abi_case_validate()` checks the *container*, not Arrow semantics. A
  `.abicase` may describe a deliberately malformed Arrow structure while being a
  perfectly canonical file. The two notions are independent and are not
  conflated anywhere in the code.
- The MSVC CI job has not been run locally — no MSVC toolchain is installed on
  the development machine. MinGW-w64, gcc and the s390x cross-gcc have been.
