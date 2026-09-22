# observer — lifecycle instrumentation

Half of it already exists, and not here. The instrumented allocator and the
ordered event log with release depth have lived in libabi since M0.5
(`libabi/include/abi/reconstruct.h`, `libabi/src/reconstruct.c`), because they
have to sit inside the reconstructed producer's own callbacks. The dual digest
is `libabi/src/digest.c` (issue #7). What remains for M1 is the lifecycle state
machine, tracked with the CALLSEQ executor in issue #5, since the executor is
what produces the paths it checks.

Counters are not enough. `child_release_invoked = true` cannot distinguish

    consumer -> parent.release() -> producer -> child.release()   CORRECT
    consumer -> child.release()                                   VIOLATION

so the observer records an ordered log with callback context, and the C-obs
assertions are predicates over that log rather than separate booleans.

`release_count == 1` is likewise not a universal constant: an EOF array arrives
already released, a moved struct has its source marked released with no callback
invoked at all, and an object never imported must not be released. The expected
value depends on the state, so the observer verifies a path through a state
machine, not a counter.
