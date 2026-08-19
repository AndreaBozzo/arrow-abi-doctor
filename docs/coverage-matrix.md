# Bounded conformance model

> **NOT YET NORMATIVE.** This file becomes binding **in M1**, before the
> generator is first run in anger. It is sketched here only so the shape is
> visible; the M1 version is the one that counts.

The real Arrow input space is not enumerable. What can be made exhaustive is a
finite model of equivalence classes, declared **before** the runs that claim
coverage over it. Declaring it afterwards would make M2-B unfalsifiable, which
is why the milestone treats publication timing as a blocking constraint rather
than a documentation task.

Intended dimensions (from the design spec, §7):

```
type            in { int32, int64, float64, utf8, bool }
length-class    in { 0, 1, small, medium }
offset-class    in { 0, 1, 7, byte-boundary +/-1 }
null-pattern    in { none, all, alternating, sparse }
buffer-state    in { normal, empty, NULL, aliased }
alignment-class in { natural, +1, +4 }
lifecycle       in { direct, moved, streamed, early-release, EOF }
```

The only claim this supports, once M1 fills it in:

> exhaustive over the finite equivalence-class matrix defined in
> coverage-matrix.md

Random fuzzing is additional evidence and is reported **separately**. It is not
coverage and must never be presented as such.
