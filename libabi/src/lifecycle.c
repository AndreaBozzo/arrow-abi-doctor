/*
 * The lifecycle state machine of abi/lifecycle.h.
 *
 * One machine per base structure -- the schema, the array, the stream -- fed
 * the ordered log. Children never get a machine of their own: the
 * specification gives the consumer exactly one thing to release, the base, and
 * a child is released by the producer walking its own tree. A child release
 * entered by the consumer is therefore not a state transition but a violation,
 * reported where it happens.
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

const char *abi_lifecycle_object_str(AbiLifecycleObject object) {
  switch (object) {
  case ABI_LC_SCHEMA: return "schema";
  case ABI_LC_ARRAY: return "array";
  case ABI_LC_STREAM: return "stream";
  case ABI_LC__OBJECTS: break;
  }
  return "?";
}

typedef enum { ST_ABSENT = 0, ST_EXPORTED, ST_IMPORTED, ST_RELEASED } State;

typedef struct {
  State    state;
  uint64_t loc; /* where the structure was last exported, moved or handed */
} Machine;

typedef enum {
  T_NONE = 0,
  T_EXPORTED,
  T_MOVED,
  T_IMPORTED,
  T_RETURNED,
  T_RELEASE
} Transition;

/* Which machine an event drives, and how. Everything else is not a transition
   of a base structure (exits, EOF, the harness's own marker). */
static Transition classify(AbiEventKind kind, int *object) {
  switch (kind) {
  case ABI_EV_SCHEMA_EXPORTED: *object = ABI_LC_SCHEMA; return T_EXPORTED;
  case ABI_EV_ARRAY_EXPORTED: *object = ABI_LC_ARRAY; return T_EXPORTED;
  case ABI_EV_STREAM_EXPORTED: *object = ABI_LC_STREAM; return T_EXPORTED;
  case ABI_EV_SCHEMA_MOVED: *object = ABI_LC_SCHEMA; return T_MOVED;
  case ABI_EV_ARRAY_MOVED: *object = ABI_LC_ARRAY; return T_MOVED;
  case ABI_EV_STREAM_MOVED: *object = ABI_LC_STREAM; return T_MOVED;
  case ABI_EV_SCHEMA_IMPORTED: *object = ABI_LC_SCHEMA; return T_IMPORTED;
  case ABI_EV_ARRAY_IMPORTED: *object = ABI_LC_ARRAY; return T_IMPORTED;
  case ABI_EV_STREAM_IMPORTED: *object = ABI_LC_STREAM; return T_IMPORTED;
  case ABI_EV_SCHEMA_RETURNED: *object = ABI_LC_SCHEMA; return T_RETURNED;
  case ABI_EV_ARRAY_RETURNED: *object = ABI_LC_ARRAY; return T_RETURNED;
  case ABI_EV_STREAM_RETURNED: *object = ABI_LC_STREAM; return T_RETURNED;
  case ABI_EV_SCHEMA_RELEASE_ENTER: *object = ABI_LC_SCHEMA; return T_RELEASE;
  case ABI_EV_ARRAY_RELEASE_ENTER: *object = ABI_LC_ARRAY; return T_RELEASE;
  case ABI_EV_STREAM_RELEASE_ENTER: *object = ABI_LC_STREAM; return T_RELEASE;
  default: return T_NONE;
  }
}

static void add(AbiLifecycleVerdict *v, AbiLifecycleRule rule, int object,
                const AbiEvent *e) {
  if (v->count < ABI_LC_MAX_FINDINGS) {
    AbiLifecycleFinding *f = &v->findings[v->count];
    f->rule = (uint8_t)rule;
    f->object = (uint8_t)object;
    f->seq = e ? e->seq : 0;
    memset(f->path, 0, sizeof(f->path));
    memcpy(f->path, e ? e->path : "/", e ? sizeof(f->path) - 1 : 1);
  }
  v->count++;
}

static int is_base(const AbiEvent *e) { return strcmp(e->path, "/") == 0; }

static void release(AbiLifecycleVerdict *out, Machine *mc, int object,
                    const AbiEvent *e, int strict_location) {
  if (!is_base(e)) {
    if (e->by_consumer) add(out, ABI_LC_CHILD_RELEASED_BY_CONSUMER, object, e);
    return;
  }
  /*
   * A base release nested inside another -- a stream releasing a batch it
   * never handed over -- is not by_consumer, and the structure was never
   * imported, so it falls through every rule below: the producer releasing
   * what it still owns is correct and not a finding.
   */
  if (mc->state == ST_RELEASED) {
    add(out, ABI_LC_RELEASED_TWICE, object, e);
  } else if (e->by_consumer) {
    if (mc->state != ST_IMPORTED) {
      add(out, ABI_LC_RELEASED_BEFORE_IMPORT, object, e);
    } else if (strict_location && e->addr != mc->loc) {
      add(out, ABI_LC_RELEASED_AT_STALE_LOCATION, object, e);
    }
  } else if (mc->state == ST_IMPORTED) {
    /*
     * A base release at depth 0 not entered by the consumer is the harness's
     * (reconstruct.c marks its own). The consumer was handed this and never
     * released it. The harness cleaning up after a structure that was never
     * handed over is the correct outcome, not a finding: the consumer never
     * owned it.
     */
    add(out, ABI_LC_NOT_RELEASED_BY_CONSUMER, object, e);
  }
  mc->state = ST_RELEASED;
}

void abi_lifecycle_verify(const AbiObserver *o, int strict_location,
                          AbiLifecycleVerdict *out) {
  Machine  m[ABI_LC__OBJECTS];
  uint32_t i;

  memset(out, 0, sizeof(*out));
  memset(m, 0, sizeof(m));
  if (!o) return;
  out->incomplete = o->events_dropped != 0;

  for (i = 0; i < o->event_count; i++) {
    const AbiEvent *e = &o->events[i];
    int             object = 0;
    Machine        *mc;

    switch (classify((AbiEventKind)e->kind, &object)) {
    case T_NONE: break;
    case T_EXPORTED:
      m[object].state = ST_EXPORTED;
      m[object].loc = e->addr;
      break;
    case T_MOVED:
      /* A move changes where, never what: the state is kept. */
      m[object].loc = e->addr;
      break;
    case T_IMPORTED:
      mc = &m[object];
      if (mc->state != ST_EXPORTED) {
        add(out, ABI_LC_IMPORTED_OUT_OF_ORDER, object, e);
      }
      mc->state = ST_IMPORTED;
      mc->loc = e->addr;
      break;
    case T_RETURNED:
      /* A refusal: ownership is the harness's again. */
      if (m[object].state == ST_IMPORTED) m[object].state = ST_EXPORTED;
      break;
    case T_RELEASE: release(out, &m[object], object, e, strict_location); break;
    }
  }

  /*
   * A structure never released is still live. The last event says nothing
   * useful about where, so the finding carries no event.
   */
  for (i = 0; i < ABI_LC__OBJECTS; i++) {
    if (m[i].state == ST_EXPORTED || m[i].state == ST_IMPORTED) {
      add(out, ABI_LC_NEVER_RELEASED, (int)i, NULL);
    }
  }
}
