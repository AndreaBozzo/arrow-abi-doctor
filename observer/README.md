# observer — lifecycle instrumentation

Lands in **M1**. Instrumented allocator, ordered event log, lifecycle state
machine, dual digest.

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
