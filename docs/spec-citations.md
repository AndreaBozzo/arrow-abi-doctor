# Specification citations

Every case class rests on a clause of the Arrow C Data / C Stream Interface
documentation, quoted verbatim here. A case whose expected outcome cannot be
attributed to a clause is a case whose expectation is an opinion, and it does
not get filed upstream.

The `.abicase` `EXPECTED` section carries a `spec_clause` string; it refers to
an entry in this file.

> **Populated during M1**, alongside the first generated corpus. M0 defines the
> mechanism — `spec_clause`, and the `EITHER` / `UNSPECIFIED` outcomes that
> exist precisely because the specification permits documented limitations —
> without yet making claims about specific clauses.

## Claims that must always carry a citation

| Claim | Needs |
|---|---|
| "a conforming consumer must accept this" | the clause that makes it valid |
| "a conforming consumer must reject this" | the clause it violates |
| "the specification does not say" | evidence of the gap, then a maintainer question |

## Framing rule

Corpus B1 is presented as **robustness against trusted-but-invalid producers,
and against validation boundaries** — never as hardening against malicious
producers. The specification states the C Data Interface is not designed for
untrusted producers, since pointer legitimacy is not verifiable in general, and
recommends validation against trusted-but-buggy ones. The second framing invites
a correct rebuttal; the first does not.
