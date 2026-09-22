# observer — lifecycle instrumentation

None of it lives here. The instrumented allocator and the ordered event log
have lived in libabi since M0.5 (`libabi/include/abi/reconstruct.h`,
`libabi/src/reconstruct.c`), because they have to sit inside the reconstructed
producer's own callbacks. The dual digest is `libabi/src/digest.c` (issue #7).
The lifecycle state machine is `libabi/src/lifecycle.c` and the CALLSEQ
executor that produces the paths it judges is `libabi/src/callseq.c` (issue
#5). This directory keeps the reasoning.

Each base structure — the schema and the array — is walked through

    EXPORTED --move--> EXPORTED --import--> IMPORTED --release--> RELEASED

and the log names every transition the specification forbids: a release of
something the consumer was never handed, a structure it took and never
released, a release entered twice, a child released directly, one never
released at all. Moves and handoffs are events too, with the address the
structure went to, so a release can be judged against where the structure
last was. That address rule is only sound when every move is logged -- true of
the null worker, whose moves are the executor's own, and false of a real
engine, which moves a structure into its own storage when it imports and tells
nobody. Workers say which they judged by (`strict` in the result line).

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
