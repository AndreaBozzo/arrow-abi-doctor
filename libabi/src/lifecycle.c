/*
 * The lifecycle state machine of abi/lifecycle.h.
 *
 * One machine per base structure, fed the ordered log. Children never get a
 * machine of their own: the specification gives the consumer exactly one thing
 * to release, the base, and a child is released by the producer walking its
 * own tree. A child release entered by the consumer is therefore not a state
 * transition but a violation, reported where it happens.
 */
#include <string.h>

#include "abi/lifecycle.h"

const char *abi_lifecycle_rule_str(AbiLifecycleRule rule) {
  switch (rule) {
  case ABI_LC_RELEASED_BEFORE_IMPORT: return "released-before-import";
  case ABI_LC_NOT_RELEASED_BY_CONSUMER: return "not-released-by-consumer";
  case ABI_LC_RELEASED_TWICE: return "released-twice";
  case ABI_LC_NEVER_RELEASED: return "never-released";
  case ABI_LC_RELEASED_AT_STALE_LOCATION: return "released-at-stale-location";
  case ABI_LC_IMPORTED_OUT_OF_ORDER: return "imported-out-of-order";
  case ABI_LC_CHILD_RELEASED_BY_CONSUMER: return "child-released-by-consumer";
  case ABI_LC__MAX: break;
  }
  return "?";
}

typedef enum { ST_ABSENT = 0, ST_EXPORTED, ST_IMPORTED, ST_RELEASED } State;

typedef struct {
  State    state;
  uint64_t loc; /* where the structure was last exported, moved or handed */
} Machine;

static void add(AbiLifecycleVerdict *v, AbiLifecycleRule rule, int is_array,
                const AbiEvent *e) {
  if (v->count < ABI_LC_MAX_FINDINGS) {
    AbiLifecycleFinding *f = &v->findings[v->count];
    f->rule = (uint8_t)rule;
    f->is_array = (uint8_t)is_array;
    f->seq = e ? e->seq : 0;
    memset(f->path, 0, sizeof(f->path));
    memcpy(f->path, e ? e->path : "/", e ? sizeof(f->path) - 1 : 1);
  }
  v->count++;
}

static int is_base(const AbiEvent *e) { return strcmp(e->path, "/") == 0; }

void abi_lifecycle_verify(const AbiObserver *o, int strict_location,
                          AbiLifecycleVerdict *out) {
  Machine  m[2];
  uint32_t i;

  memset(out, 0, sizeof(*out));
  memset(m, 0, sizeof(m));
  if (!o) return;
  out->incomplete = o->events_dropped != 0;

  for (i = 0; i < o->event_count; i++) {
    const AbiEvent *e = &o->events[i];
    int             arr = -1;
    Machine        *mc;

    switch ((AbiEventKind)e->kind) {
    case ABI_EV_SCHEMA_EXPORTED:
    case ABI_EV_ARRAY_EXPORTED:
      arr = e->kind == ABI_EV_ARRAY_EXPORTED;
      m[arr].state = ST_EXPORTED;
      m[arr].loc = e->addr;
      break;

    case ABI_EV_SCHEMA_MOVED:
    case ABI_EV_ARRAY_MOVED:
      /* A move changes where, never what: the state is kept. */
      arr = e->kind == ABI_EV_ARRAY_MOVED;
      m[arr].loc = e->addr;
      break;

    case ABI_EV_SCHEMA_IMPORTED:
    case ABI_EV_ARRAY_IMPORTED:
      arr = e->kind == ABI_EV_ARRAY_IMPORTED;
      mc = &m[arr];
      if (mc->state != ST_EXPORTED) {
        add(out, ABI_LC_IMPORTED_OUT_OF_ORDER, arr, e);
      }
      mc->state = ST_IMPORTED;
      mc->loc = e->addr;
      break;

    case ABI_EV_SCHEMA_RETURNED:
    case ABI_EV_ARRAY_RETURNED:
      /* A refusal: ownership is the harness's again. */
      arr = e->kind == ABI_EV_ARRAY_RETURNED;
      if (m[arr].state == ST_IMPORTED) m[arr].state = ST_EXPORTED;
      break;

    case ABI_EV_SCHEMA_RELEASE_ENTER:
    case ABI_EV_ARRAY_RELEASE_ENTER:
      arr = e->kind == ABI_EV_ARRAY_RELEASE_ENTER;
      if (!is_base(e)) {
        if (e->by_consumer) add(out, ABI_LC_CHILD_RELEASED_BY_CONSUMER, arr, e);
        break;
      }
      /* Nested inside another release: a dictionary's, not the consumer's. */
      if (e->depth != 0) break;
      mc = &m[arr];
      if (mc->state == ST_RELEASED) {
        add(out, ABI_LC_RELEASED_TWICE, arr, e);
      } else if (e->by_consumer) {
        if (mc->state != ST_IMPORTED) {
          add(out, ABI_LC_RELEASED_BEFORE_IMPORT, arr, e);
        } else if (strict_location && e->addr != mc->loc) {
          add(out, ABI_LC_RELEASED_AT_STALE_LOCATION, arr, e);
        }
      } else if (mc->state == ST_IMPORTED) {
        /*
         * A base release at depth 0 not entered by the consumer is the
         * harness's (reconstruct.c marks its own). The consumer was handed
         * this and never released it. The harness cleaning up after a
         * structure that was never handed over is the correct outcome, not a
         * finding: the consumer never owned it.
         */
        add(out, ABI_LC_NOT_RELEASED_BY_CONSUMER, arr, e);
      }
      mc->state = ST_RELEASED;
      break;

    default: break;
    }
  }

  /*
   * A structure never released is still live. The last event says nothing
   * useful about where, so the finding carries no event.
   */
  for (i = 0; i < 2; i++) {
    if (m[i].state == ST_EXPORTED || m[i].state == ST_IMPORTED) {
      add(out, ABI_LC_NEVER_RELEASED, (int)i, NULL);
    }
  }
}
