# corpus

Generated and hand-written cases, by class.

| Directory | Class | Meaning |
|---|---|---|
| `a/`  | A  | conforming producer, valid but rare input |
| `b1/` | B1 | producer invalid in structure or data |
| `b2/` | B2 | producer invalid in protocol or lifecycle |
| `c/`  | C  | consumer misuse |

Populated from M1. The committed M0 fixture lives in
`libabi/tests/fixtures/` instead, because it is a format test rather than a
case for any engine.
