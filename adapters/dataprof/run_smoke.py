#!/usr/bin/env python3
"""M0.5 smoke test.

Hands a reconstructed .abicase to real Arrow consumers through the Arrow
PyCapsule interface and reports what the lifecycle observer saw.

This is deliberately not a differential pair. dataprof is a target we control,
and the question is whether *our* instrument works: does the reconstruction
produce structures a real Arrow consumer accepts, does the consumer release
them, and do the allocation counters balance afterwards. A disagreement here is
far more likely to be our bug than the consumer's, which is the whole point of
smoke-testing against something we own before pointing the thing at anyone
else's engine.

Three consumers run, and the third exists to remove a confound:

  pyarrow              our object -> pa.record_batch()
  dataprof direct      our object -> dataprof.profile()
  dataprof via pyarrow our object -> pa.record_batch() -> dataprof.profile()

If direct is rejected but via-pyarrow is accepted, the *data* is fine and the
rejection is about what type of object the consumer will take. Without the third
run, a rejection cannot be attributed to either side.
"""

from __future__ import annotations

import argparse
import gc
import sys
import traceback

import _abicase


class Outcome:
    def __init__(self, case_name: str, consumer: str) -> None:
        self.case_name = case_name
        self.consumer = consumer
        self.accepted = False
        self.detail = ""
        self.error = ""
        self.crashed = False
        self.lifecycle: dict = {}

    @property
    def released_by_consumer(self) -> bool:
        """True when a release callback was entered from outside our code.

        The observer records the nesting depth of the producer's own release
        callbacks, so a release entered at depth 0 came from the consumer. That
        is the distinction a boolean counter cannot make.
        """
        return any(
            e["by_consumer"] and e["kind"].endswith("RELEASE_ENTER")
            for e in self.lifecycle.get("events", [])
        )

    @property
    def harness_had_to_clean_up(self) -> bool:
        return any(e["kind"] == "HARNESS_RELEASED" for e in self.lifecycle.get("events", []))

    @property
    def held_by_consumer(self) -> bool:
        """Bytes are outstanding, but a live capsule still owns them.

        Not a leak. A consumer that takes the capsules and then fails without
        consuming them leaves structures the capsule owns; `release_all()`
        cannot reach them, because they were moved out. Python releases them
        when it collects the capsule. Issue #6: the allocator counter means
        "outstanding at this instant", and reporting that as a leak would send
        someone chasing a bug that is not there.
        """
        life = self.lifecycle
        return bool(life.get("outstanding")) and life.get("capsules_outstanding", 0) > 0

    @property
    def leaked(self) -> bool:
        """Bytes are outstanding and nothing is left that could release them."""
        life = self.lifecycle
        return bool(life.get("outstanding")) and life.get("capsules_outstanding", 1) == 0

    @property
    def clean(self) -> bool:
        """Our side behaved: nothing leaked and no lifecycle rule was broken."""
        return not self.leaked and self.lifecycle.get("violations", 1) == 0


def consume_pyarrow(case):
    import pyarrow as pa

    batch = pa.record_batch(case)
    return f"{batch.num_rows} rows x {batch.num_columns} cols, {batch.schema.names}"


def _describe(report) -> str:
    bits = []
    for attr in ("row_count", "total_rows", "column_count", "columns"):
        value = getattr(report, attr, None)
        if value is None:
            continue
        bits.append(f"{attr}={len(value) if hasattr(value, '__len__') else value}")
    return ", ".join(bits) or type(report).__name__


def consume_dataprof_direct(case):
    import dataprof

    # dataprof.profile() documents support for "Arrow PyCapsule-compatible
    # objects", which should route to import_via_pycapsule -> arrow-rs from_ffi.
    return _describe(dataprof.profile(case, name="abicase"))


def consume_dataprof_via_pyarrow(case):
    import dataprof
    import pyarrow as pa

    # Same bytes, same capsules, but handed over as a pyarrow RecordBatch. Our
    # producer is exercised identically; only the object dataprof sees differs.
    batch = pa.record_batch(case)
    return _describe(dataprof.profile(batch, name="abicase"))


CONSUMERS = {
    "pyarrow": consume_pyarrow,
    "dataprof-direct": consume_dataprof_direct,
    "dataprof-via-pa": consume_dataprof_via_pyarrow,
}


def run(case_name: str, make_case, consumer: str, verbose: bool) -> Outcome:
    out = Outcome(case_name, consumer)
    case = make_case()
    try:
        out.detail = CONSUMERS[consumer](case)
        out.accepted = True
    except BaseException as exc:  # noqa: BLE001
        # BaseException, not Exception: pyo3 derives PanicException from
        # BaseException precisely so it is not swallowed by accident, so a Rust
        # panic crossing FFI would otherwise take the whole run down. A crashing
        # consumer has to be an observed datum, not the end of the report.
        out.error = f"{type(exc).__name__}: {exc}"
        out.crashed = not isinstance(exc, Exception)
        if verbose:
            traceback.print_exc()

    # A failed consumer's traceback keeps its frames alive, and those frames
    # hold whatever capsules it had taken. The exception -> traceback -> frame
    # cycle outlives the except block, so without a collection here the capsules
    # are still alive when the counters are read and the outstanding bytes look
    # like a leak. Drop everything droppable first; whatever is still held after
    # this really is held by the consumer (issue #6).
    gc.collect()

    # Release whatever the consumer left live, then read the final report. This
    # order is what keeps "the consumer released it" distinguishable from "we
    # cleaned up after a consumer that did not".
    case.release_all()
    out.lifecycle = case.lifecycle()
    del case
    return out


def print_outcome(out: Outcome) -> None:
    life = out.lifecycle
    if out.crashed:
        verdict = "CRSH"
    elif not out.clean:
        verdict = "LEAK"
    elif out.held_by_consumer:
        # Outstanding but owned by a live capsule. Not a failure, and shown
        # separately so it is never mistaken for one.
        verdict = "HELD"
    else:
        verdict = "ok  " if out.accepted else "--  "
    state = "accepted" if out.accepted else ("CRASHED" if out.crashed else "rejected")
    print(f"  [{verdict}] {out.consumer:<16} {state}")
    print(f"           {out.detail if out.accepted else out.error}")
    print(
        "           capsules: schema={} array={} | released by consumer: {}{}".format(
            life.get("schema_taken"),
            life.get("array_taken"),
            out.released_by_consumer,
            "  (harness cleaned up)" if out.harness_had_to_clean_up else "",
        )
    )
    if out.leaked:
        alloc_state = "LEAKED"
    elif out.held_by_consumer:
        alloc_state = "still held by the consumer"
    else:
        alloc_state = "clean"
    print(
        "           alloc {}/{} bytes, {}/{} blocks | outstanding={} "
        "capsules_outstanding={} -> {} | violations={}".format(
            life.get("bytes_allocated"),
            life.get("bytes_freed"),
            life.get("blocks_allocated"),
            life.get("blocks_freed"),
            life.get("outstanding"),
            life.get("capsules_outstanding"),
            alloc_state,
            life.get("violations"),
        )
    )


def print_held(held: list[Outcome]) -> None:
    """Report outcomes whose bytes are outstanding but still owned by a capsule.

    Held is not a leak and does not flip the verdict; it is printed because the
    distinction is the whole content of issue #6, and a bare count of
    outstanding bytes would read as the other thing.
    """
    if not held:
        return
    print("\nstill held by the consumer at the moment of measurement (not a leak):")
    for o in held:
        print(
            f"  {o.consumer} {o.case_name}: "
            f"{o.lifecycle.get('outstanding')} outstanding, "
            f"{o.lifecycle.get('capsules_outstanding')} capsule(s) alive"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "files", nargs="*", help=".abicase files to run in addition to the fixtures"
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--log", action="store_true", help="print the full lifecycle event log")
    args = parser.parse_args()

    import dataprof
    import pyarrow

    print(f"abi-doctor M0.5 smoke  (libabi {_abicase.__abi_doctor_version__})")
    print(f"consumers: dataprof {dataprof.__version__}, pyarrow {pyarrow.__version__}")

    cases = [
        ("fixture:smoke", _abicase.smoke),
        ("fixture:rich", _abicase.rich),
        # Class B1: structurally fine, semantically invalid. Expected outcome is
        # a clean refusal; a crash or an out-of-bounds read is a bug whatever
        # the input.
        ("fixture:bad-dict-index", _abicase.bad_dict_index),
    ]
    for path in args.files:
        cases.append((path, lambda p=path: _abicase.load(p)))

    outcomes = []
    for case_name, make_case in cases:
        print(f"\n{case_name}")
        for consumer in CONSUMERS:
            out = run(case_name, make_case, consumer, args.verbose)
            outcomes.append(out)
            print_outcome(out)
            if args.log:
                for e in out.lifecycle.get("events", []):
                    mark = "  <- by consumer" if e["by_consumer"] else ""
                    indent = "  " * e["depth"]
                    print(f"             {e['seq']:>3} {indent}{e['kind']} {e['path']}{mark}")

    # --- verdict -----------------------------------------------------------
    #
    # The instrument is what is under test, so the pass condition is about our
    # side: nothing leaked, no lifecycle rule broken, and the plainly-valid
    # smoke fixture was accepted by at least one real Arrow consumer. A single
    # consumer refusing our object *type* is a finding about that consumer, not
    # a failure of the reconstruction -- which is exactly what the third run
    # exists to establish.
    leaked = [o for o in outcomes if o.leaked]
    held = [o for o in outcomes if o.held_by_consumer]
    violations = [o for o in outcomes if o.lifecycle.get("violations")]
    crashed = [o for o in outcomes if o.crashed]
    smoke = [o for o in outcomes if o.case_name == "fixture:smoke"]
    smoke_accepted = [o for o in smoke if o.accepted]

    print("\n" + "-" * 74)
    print(
        f"{len(outcomes)} run(s), {sum(o.accepted for o in outcomes)} accepted | "
        f"leaks: {len(leaked)} | still held by consumer: {len(held)} | "
        f"lifecycle violations: {len(violations)} | consumer crashes: {len(crashed)}"
    )

    print_held(held)

    # A consumer crash is a datum about the consumer, not a failure of this
    # harness, so it does not flip the verdict below. It still gets its own line:
    # buried in a nine-run table, the one outcome the project exists to find is
    # the easiest to miss.
    if crashed:
        print("\nconsumer crashes (an uncatchable exception crossed the boundary):")
        for o in crashed:
            print(f"  {o.consumer} {o.case_name}: {o.error}")

    findings = []
    for case_name, _ in cases:
        direct = next(
            (o for o in outcomes if o.case_name == case_name and o.consumer == "dataprof-direct"),
            None,
        )
        via = next(
            (o for o in outcomes if o.case_name == case_name and o.consumer == "dataprof-via-pa"),
            None,
        )
        if direct and via and not direct.accepted and via.accepted:
            findings.append((case_name, direct.error))

    if findings:
        print("\nfindings (consumer accepted the same data when wrapped by pyarrow,")
        print("          so the data is well-formed and the refusal is about object type):")
        for case_name, err in findings:
            print(f"  dataprof {case_name}: {err}")

    failed = bool(leaked or violations or not smoke_accepted)
    print("\nM0.5 smoke:", "FAIL" if failed else "PASS")
    if not smoke_accepted:
        print("  the plainly-valid smoke fixture was accepted by no consumer")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
