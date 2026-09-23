/*
 * Reconstruction: .abicase -> real ArrowSchema / ArrowArray, plus the lifecycle
 * observation that makes what the consumer did with them a measurement rather
 * than an inference.
 *
 * M0 produced a portable description of a case. This turns one back into actual
 * C Data Interface structures on this host: aligned allocations, buffer views
 * that genuinely alias where the case says they alias, metadata re-encoded into
 * Arrow's native-endian wire form, and release callbacks that behave the way
 * the specification says a producer's must.
 *
 * The reconstruction owns everything it builds and outlives the structures it
 * exports, so the event log and the allocation counters are still readable
 * after the consumer has released -- which is the whole point of having them.
 */
#ifndef ABI_RECONSTRUCT_H
#define ABI_RECONSTRUCT_H

#include <stdio.h>

#include "abi/abicase.h"
#include "abi/arrow_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- lifecycle event log (design spec 5.1) ------------------------------- */

/*
 * Counters are not enough. `child_release_invoked = true` cannot distinguish
 *
 *   consumer -> parent.release() -> producer -> child.release()   CORRECT
 *   consumer -> child.release()                                   VIOLATION
 *
 * so events carry the nesting depth of the producer's own release callbacks.
 * A release entered at depth 0 was entered by the consumer; a child release
 * entered at depth 0 is the violation above, observed rather than deduced.
 */
typedef enum {
  ABI_EV_SCHEMA_EXPORTED = 0,
  ABI_EV_ARRAY_EXPORTED,
  ABI_EV_SCHEMA_RELEASE_ENTER,
  ABI_EV_SCHEMA_RELEASE_EXIT,
  ABI_EV_ARRAY_RELEASE_ENTER,
  ABI_EV_ARRAY_RELEASE_EXIT,
  /*
   * Allocation is not an event kind: one entry per malloc would bury the
   * lifecycle in noise, and what the report needs is the delta. It is counted
   * in AbiAllocStats and printed as a single line, as the design spec's example
   * log does.
   */
  ABI_EV_VIOLATION_CHILD_RELEASED_BY_CONSUMER,
  ABI_EV_HARNESS_RELEASED,
  /*
   * A base structure moved to a new address, per Arrow move semantics: bitwise
   * copy, source marked released, no callback invoked. `addr` is where it went,
   * which is where its one release is then expected.
   */
  ABI_EV_SCHEMA_MOVED,
  ABI_EV_ARRAY_MOVED,
  /* A base structure handed to the consumer, at `addr`. */
  ABI_EV_SCHEMA_IMPORTED,
  ABI_EV_ARRAY_IMPORTED,
  /*
   * The consumer refused a structure and left it live, so ownership came back
   * to the harness. Without this, the harness cleaning up after a refusal
   * would read as a consumer that took the structure and never released it.
   */
  ABI_EV_SCHEMA_RETURNED,
  ABI_EV_ARRAY_RETURNED,
  /*
   * The C Stream Interface (abi_reconstruct_stream). The stream is a base
   * structure like the others: exported, possibly moved, handed over,
   * released exactly once. A batch get_next() hands over is an ARRAY_MOVED /
   * ARRAY_IMPORTED pair at the consumer's `out`; a schema get_schema() hands
   * over is SCHEMA_EXPORTED / SCHEMA_IMPORTED there. STREAM_EOF is get_next()
   * answering with the released array that marks the end.
   */
  ABI_EV_STREAM_EXPORTED,
  ABI_EV_STREAM_MOVED,
  ABI_EV_STREAM_IMPORTED,
  ABI_EV_STREAM_RETURNED,
  ABI_EV_STREAM_RELEASE_ENTER,
  ABI_EV_STREAM_RELEASE_EXIT,
  ABI_EV_STREAM_EOF,
  ABI_EV__MAX
} AbiEventKind;

#define ABI_EVENT_PATH_MAX 24
#define ABI_MAX_EVENTS     1024

typedef struct {
  uint32_t seq;
  uint8_t  kind;
  uint8_t  depth;       /* producer release-callback nesting at the time */
  uint8_t  by_consumer; /* entered at depth 0, and not by the harness */
  char     path[ABI_EVENT_PATH_MAX];
  /*
   * The address of the structure the event concerns: where it was exported,
   * moved to, handed over or released from. A release is only meaningful
   * against the place the structure was last known to live.
   */
  uint64_t addr;
} AbiEvent;

typedef struct {
  uint64_t bytes_allocated;
  uint64_t bytes_freed;
  uint64_t blocks_allocated;
  uint64_t blocks_freed;
} AbiAllocStats;

typedef struct {
  AbiEvent      events[ABI_MAX_EVENTS];
  uint32_t      event_count;
  uint32_t      events_dropped;
  uint32_t      next_seq;
  AbiAllocStats alloc;
  uint32_t      release_depth;
  uint32_t      violations;
} AbiObserver;

const char *abi_event_kind_str(AbiEventKind kind);

/* --- reconstruction ------------------------------------------------------ */

typedef struct AbiReconstruction AbiReconstruction;

/*
 * Builds the C structures described by `c`. Everything is copied, so `c` need
 * not outlive the reconstruction.
 */
AbiStatus abi_reconstruct(const AbiCase *c, AbiReconstruction **out,
                          AbiError *err);

/*
 * The exported structures. The caller may hand these to a consumer, and may
 * move them bitwise and mark the source released, as the specification permits:
 * no private_data points into its own struct, so the structures are
 * relocatable. Returns NULL for the array of a schema-only case.
 */
struct ArrowSchema *abi_reconstruction_schema(AbiReconstruction *r);
struct ArrowArray  *abi_reconstruction_array(AbiReconstruction *r);

const AbiObserver *abi_reconstruction_observer(const AbiReconstruction *r);

/*
 * The case as an ArrowArrayStream (docs/coverage-matrix.md 1.7, the stream
 * lifecycles). get_schema() builds a fresh schema on every call, from a copy of
 * the case the reconstruction keeps. get_next() hands over the case's array --
 * moved out of the reconstruction, so it can be handed over once -- and then
 * answers EOF; a schema-only case answers EOF at once. Releasing the stream
 * releases a batch it never handed over, nested inside its own release, as a
 * producer's stream owns what it has not yet delivered.
 *
 * No schema is pre-exported: abi_reconstruction_schema() of a stream
 * reconstruction is a released shell. Hand the stream over instead.
 */
AbiStatus abi_reconstruct_stream(const AbiCase *c, AbiReconstruction **out,
                                 AbiError *err);

/* The stream of a stream reconstruction; NULL for any other. */
struct ArrowArrayStream *abi_reconstruction_stream(AbiReconstruction *r);

/* Arrow move semantics for the stream, as for the schema and the array. */
AbiStatus abi_reconstruction_move_stream(AbiReconstruction       *r,
                                         struct ArrowArrayStream *dst,
                                         struct ArrowArrayStream *src);
void abi_reconstruction_note_stream_import(AbiReconstruction             *r,
                                           const struct ArrowArrayStream *addr);
void abi_reconstruction_note_stream_returned(
    AbiReconstruction *r, const struct ArrowArrayStream *addr);
void abi_reconstruction_harness_release_stream(AbiReconstruction       *r,
                                               struct ArrowArrayStream *s);

/*
 * Arrow move semantics on a base structure: `*dst = *src`, then `src` is marked
 * released without its callback being invoked. Logged as SCHEMA_MOVED /
 * ARRAY_MOVED with `dst` as the address, so the lifecycle state machine
 * (abi/lifecycle.h) knows where the one legitimate release now has to come
 * from. `src` must be live.
 */
AbiStatus abi_reconstruction_move_schema(AbiReconstruction  *r,
                                         struct ArrowSchema *dst,
                                         struct ArrowSchema *src);
AbiStatus abi_reconstruction_move_array(AbiReconstruction *r,
                                        struct ArrowArray *dst,
                                        struct ArrowArray *src);

/*
 * Records that a base structure at `addr` was handed to the consumer. The
 * handoff is the line between "the harness still owns this" and "releasing it
 * is now the consumer's job", which is what the state machine judges a release
 * against.
 */
void abi_reconstruction_note_schema_import(AbiReconstruction        *r,
                                           const struct ArrowSchema *addr);
void abi_reconstruction_note_array_import(AbiReconstruction       *r,
                                          const struct ArrowArray *addr);

/* The consumer refused the structure at `addr` and left it live. */
void abi_reconstruction_note_schema_returned(AbiReconstruction        *r,
                                             const struct ArrowSchema *addr);
void abi_reconstruction_note_array_returned(AbiReconstruction       *r,
                                            const struct ArrowArray *addr);

/*
 * Releases a live base structure the consumer left behind, wherever it now
 * lives, as the harness: logged HARNESS_RELEASED and never attributed to the
 * consumer. A no-op for one already released.
 */
void abi_reconstruction_harness_release_schema(AbiReconstruction  *r,
                                               struct ArrowSchema *s);
void abi_reconstruction_harness_release_array(AbiReconstruction *r,
                                              struct ArrowArray *a);

/*
 * Releases anything the consumer left live, logging it as HARNESS_RELEASED, and
 * frees the backing allocations. The reconstruction itself survives, so the
 * event log and the counters can still be read -- which is the only way to
 * report on a consumer that imported and never released.
 */
void abi_reconstruction_release_all(AbiReconstruction *r);

/* release_all(), then frees the reconstruction. */
void abi_reconstruction_free(AbiReconstruction *r);

/*
 * Non-zero when allocation and free counts disagree. Measured by an
 * instrumented allocator: without one, "0 leaks" is declared, not measured.
 */
int abi_reconstruction_leaked(const AbiReconstruction *r);

/* Ordered, human-readable event log -- the report artifact of a single run. */
void abi_reconstruction_print_log(const AbiReconstruction *r, FILE *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ABI_RECONSTRUCT_H */
