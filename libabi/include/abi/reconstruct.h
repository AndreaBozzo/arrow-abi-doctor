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
  ABI_EV__MAX
} AbiEventKind;

#define ABI_EVENT_PATH_MAX 24
#define ABI_MAX_EVENTS     1024

typedef struct {
  uint32_t seq;
  uint8_t  kind;
  uint8_t  depth;       /* producer release-callback nesting at the time */
  uint8_t  by_consumer; /* entered at depth 0, i.e. called from outside */
  char     path[ABI_EVENT_PATH_MAX];
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
