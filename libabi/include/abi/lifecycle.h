/*
 * The lifecycle state machine: a verdict on a path through the event log, not
 * on a counter.
 *
 * `release_count == 1` is not a universal constant. An EOF array arrives
 * already released, a moved structure has its source marked released with no
 * callback invoked, and an object never handed to the consumer must not be
 * released by it at all. What is correct depends on the state the structure
 * was in, so each base structure -- the schema and the array -- is walked
 * through
 *
 *   EXPORTED --move--> EXPORTED --import--> IMPORTED --release--> RELEASED
 *
 * over the ordered log of abi/reconstruct.h, and every transition the
 * specification forbids is reported by name.
 *
 * Predicates over the log rather than separate booleans (observer/README.md):
 * the same log answers every rule, so no rule can disagree with another about
 * what happened.
 */
#ifndef ABI_LIFECYCLE_H
#define ABI_LIFECYCLE_H

#include "abi/reconstruct.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  /* The consumer released a structure it was never handed. */
  ABI_LC_RELEASED_BEFORE_IMPORT = 0,
  /* The consumer took a structure and never released it; the harness did. */
  ABI_LC_NOT_RELEASED_BY_CONSUMER,
  /* A release entered for a structure already released. */
  ABI_LC_RELEASED_TWICE,
  /* Nothing ever released the structure: its memory is still live. */
  ABI_LC_NEVER_RELEASED,
  /*
   * Released from an address other than where it was last moved or handed
   * over. Only judged in strict mode: a real consumer moves structures into
   * its own storage without telling anyone, which is legal and leaves the
   * release at an address the log has never seen.
   */
  ABI_LC_RELEASED_AT_STALE_LOCATION,
  /* Handed over twice, or after its release. */
  ABI_LC_IMPORTED_OUT_OF_ORDER,
  /* The consumer released a child directly instead of the base. */
  ABI_LC_CHILD_RELEASED_BY_CONSUMER,
  ABI_LC__MAX
} AbiLifecycleRule;

const char *abi_lifecycle_rule_str(AbiLifecycleRule rule);

#define ABI_LC_MAX_FINDINGS 16

typedef struct {
  uint8_t  rule;                     /* AbiLifecycleRule */
  uint8_t  is_array;                 /* 0: the schema tree, 1: the array tree */
  uint32_t seq;                      /* the event that broke the rule */
  char     path[ABI_EVENT_PATH_MAX]; /* "/" for a base, deeper for a child */
} AbiLifecycleFinding;

typedef struct {
  uint32_t            count; /* findings, including any beyond the array */
  AbiLifecycleFinding findings[ABI_LC_MAX_FINDINGS];
  /* The log overflowed: a verdict over a truncated path is not a verdict. */
  int incomplete;
} AbiLifecycleVerdict;

/*
 * Walks the log. `strict_location` asks for the address rule too, which is
 * sound only when every move of the structures was logged -- when the consumer
 * is ours, not when it is a real engine with its own storage.
 *
 * Read the log after every owner is gone: after the consumer's objects are
 * collected and after abi_reconstruction_release_all(). A structure still held
 * by a live capsule is reported as NEVER_RELEASED, because at that instant it
 * is.
 */
void abi_lifecycle_verify(const AbiObserver *o, int strict_location,
                          AbiLifecycleVerdict *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ABI_LIFECYCLE_H */
