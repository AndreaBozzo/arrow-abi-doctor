#!/usr/bin/env python3
"""Regression check for the harness leak accounting (issue #6).

The allocator counter means "outstanding at this instant", not "leaked". A
consumer that takes the PyCapsules and then fails *without consuming them*
leaves live structures the capsule owns. `release_all()` cannot reach them --
they were moved out of the reconstruction -- so at the moment of measurement
those bytes are outstanding while nothing has leaked: Python releases them when
it collects the capsule.

That distinction is one of the few things this harness actually asserts, so it
gets a check that fails when the distinction is gone. Run it against the unfixed
build and it fails; that is the only reason to trust it here.

Four synthetic consumers, chosen so that each part of the fix is discriminated
by at least one of them:

  consumes-nothing    never takes a capsule                  -> clean
  takes-then-fails    takes capsules into its own locals,
                      then raises                            -> clean, because
                                                                the capsules die
                                                                with its frame
  takes-into-cycle    same, but its locals sit in a
                      reference cycle                        -> clean, but only
                                                                after a
                                                                collection
  takes-and-stashes   takes capsules, stores them where the
                      harness cannot reach, then raises      -> held, not leaked

`takes-into-cycle` is the one that covers the collection step in `run()`.
`takes-then-fails` does not: plain locals die by refcount the moment the except
block drops the exception, so that scenario passes with and without the
collection and would have guarded nothing on its own. A cycle -- ordinary in any
Python object graph with back-references -- is what only the collector can free.
`takes-and-stashes` covers the capsule counter in the extension and the
classification built on it.
"""

from __future__ import annotations

import sys

import _abicase
import run_smoke

# Where "takes-and-stashes" puts the capsules: module scope, so nothing the
# harness does between the failure and the measurement can reclaim them. This
# stands in for a consumer that keeps a reference inside its own object graph.
STASH: list = []

# __arrow_c_array__ cuts two: the schema and the array.
CAPSULES_PER_ARRAY = 2


def consumes_nothing(case):
    raise RuntimeError("rejected before touching the capsules")


def takes_then_fails(case):
    _caps = case.__arrow_c_array__()  # held by this frame and nothing else
    raise RuntimeError("took the capsules, then failed without consuming them")


def takes_into_cycle(case):
    # The capsules are unreachable once this frame dies, but they sit in a
    # cycle, so refcounting alone will not free them. Without the collection in
    # run(), the report says "still held by the consumer" about a consumer that
    # is gone.
    holder = {"caps": case.__arrow_c_array__()}
    holder["self"] = holder
    raise RuntimeError("took the capsules into a cycle, then failed")


def takes_and_stashes(case):
    STASH.extend(case.__arrow_c_array__())
    raise RuntimeError("took the capsules, stashed them, then failed")


CHECKS: list[tuple[str, object, dict]] = [
    (
        "consumes-nothing",
        consumes_nothing,
        {"clean": True, "leaked": False, "held": False, "capsules": 0, "outstanding": False},
    ),
    (
        "takes-then-fails",
        takes_then_fails,
        {"clean": True, "leaked": False, "held": False, "capsules": 0, "outstanding": False},
    ),
    (
        "takes-into-cycle",
        takes_into_cycle,
        {"clean": True, "leaked": False, "held": False, "capsules": 0, "outstanding": False},
    ),
    (
        "takes-and-stashes",
        takes_and_stashes,
        {
            "clean": True,
            "leaked": False,
            "held": True,
            "capsules": CAPSULES_PER_ARRAY,
            "outstanding": True,
        },
    ),
]


def check(name: str, consumer, expected: dict, failures: list[str]) -> None:
    run_smoke.CONSUMERS[name] = consumer
    try:
        out = run_smoke.run("fixture:smoke", _abicase.smoke, name, verbose=False)
    finally:
        del run_smoke.CONSUMERS[name]

    life = out.lifecycle
    actual = {
        "clean": out.clean,
        "leaked": out.leaked,
        "held": out.held_by_consumer,
        "capsules": life.get("capsules_outstanding"),
        "outstanding": bool(life.get("outstanding")),
    }
    ok = actual == expected
    print(f"  [{'ok' if ok else 'FAIL'}] {name}")
    print(f"         expected {expected}")
    print(f"         actual   {actual}")
    if not ok:
        differing = sorted(k for k in expected if actual.get(k) != expected[k])
        failures.append(f"{name}: {', '.join(differing)}")


def main() -> int:
    print(f"leak accounting check  (libabi {_abicase.__abi_doctor_version__})")
    failures: list[str] = []

    for name, consumer, expected in CHECKS:
        check(name, consumer, expected, failures)

    # Draining the stash must release what it was holding. If it does not, the
    # bytes really were leaking and the "held" verdict above was a comfortable
    # answer rather than a true one -- which would be a worse bug than the false
    # positive this whole check exists to prevent.
    case = _abicase.smoke()
    STASH.extend(case.__arrow_c_array__())
    case.release_all()
    before = case.lifecycle()
    STASH.clear()
    after = case.lifecycle()
    # .get(), not [], so that a build without these keys reports a failure
    # rather than raising: this check has to be legible when it fails, and the
    # build it is meant to fail on is exactly the one missing the keys.
    drained = (
        before.get("capsules_outstanding") == CAPSULES_PER_ARRAY
        and after.get("capsules_outstanding") == 0
        and before.get("outstanding")
        and not after.get("outstanding")
    )
    print(f"  [{'ok' if drained else 'FAIL'}] held bytes are released when the holder lets go")
    for tag, life in (("before", before), ("after ", after)):
        print(
            f"         {tag} {life['bytes_allocated'] - life['bytes_freed']} bytes outstanding, "
            f"{life.get('capsules_outstanding')} capsule(s)"
        )
    if not drained:
        failures.append("held bytes were not released when the holder let go")

    print()
    if failures:
        print(f"FAIL  {len(failures)} check(s):")
        for f in failures:
            print(f"  {f}")
        return 1
    print(f"PASS  {len(CHECKS) + 1} checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
