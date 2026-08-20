/*
 * .abicase -> real ArrowSchema / ArrowArray, with lifecycle observation.
 *
 * Three things here are load-bearing and easy to get subtly wrong:
 *
 *  - Aliasing is physical. One real allocation per AbiAllocation, and views are
 *    pointers into it. Two buffers the case says alias end up as the same
 *    address, which is the only way an ownership test means anything.
 *
 *  - Metadata is written in NATIVE byte order. That is the opposite of the file
 *    format's rule and it is deliberate: Arrow's metadata wire form uses
 *    native-endian int32, so this buffer must match the host the consumer runs
 *    on. It is the reason .abicase stores metadata decoded in the first place.
 *
 *  - Release frees what was allocated, not what was declared. A B1 case may say
 *    n_children = 5 while providing 2; the release callback still has to free
 *    the 5 pointer slots it asked for, so the slot count lives in private_data
 *    rather than being re-derived from the struct the consumer can see.
 */
#include <stdlib.h>
#include <string.h>

#include "abi/reconstruct.h"

#define ABI_ALLOC_MAGIC 0xAB1CA5E0D0C704ULL

/*
 * Header placed immediately before every block handed out, so free() knows the
 * size and the original malloc pointer. Copied in and out with memcpy because
 * the caller's alignment request can leave it unaligned for its own fields.
 */
typedef struct {
  void    *base;
  uint64_t size;
  uint64_t magic;
} AbiAllocHdr;

typedef struct {
  AbiReconstruction *rec;
  uint32_t           child_slots; /* pointers actually allocated */
  char              *format_buf;
  char              *name_buf;
  char              *metadata_buf;
  uint8_t            is_root;
  char               path[ABI_EVENT_PATH_MAX];
} AbiNodePriv;

struct AbiReconstruction {
  void   **alloc_data; /* aligned pointers, one per AbiAllocation */
  uint32_t alloc_count;
  int      buffers_freed;

  struct ArrowSchema schema;
  struct ArrowArray  array;
  int                has_array;

  AbiObserver obs;
};

const char *abi_event_kind_str(AbiEventKind kind) {
  switch (kind) {
  case ABI_EV_SCHEMA_EXPORTED: return "SCHEMA_EXPORTED";
  case ABI_EV_ARRAY_EXPORTED: return "ARRAY_EXPORTED";
  case ABI_EV_SCHEMA_RELEASE_ENTER: return "SCHEMA_RELEASE_ENTER";
  case ABI_EV_SCHEMA_RELEASE_EXIT: return "SCHEMA_RELEASE_EXIT";
  case ABI_EV_ARRAY_RELEASE_ENTER: return "ARRAY_RELEASE_ENTER";
  case ABI_EV_ARRAY_RELEASE_EXIT: return "ARRAY_RELEASE_EXIT";
  case ABI_EV_VIOLATION_CHILD_RELEASED_BY_CONSUMER:
    return "VIOLATION_CHILD_RELEASED_BY_CONSUMER";
  case ABI_EV_HARNESS_RELEASED: return "HARNESS_RELEASED";
  case ABI_EV__MAX: break;
  }
  return "?";
}

/* --- instrumented allocator ---------------------------------------------- */

static void obs_event(AbiReconstruction *r, AbiEventKind kind, const char *path,
                      uint8_t by_consumer) {
  AbiEvent *e;
  if (r->obs.event_count >= ABI_MAX_EVENTS) {
    r->obs.events_dropped++;
    return;
  }
  e = &r->obs.events[r->obs.event_count++];
  e->seq = r->obs.next_seq++;
  e->kind = (uint8_t)kind;
  e->depth = (uint8_t)r->obs.release_depth;
  e->by_consumer = by_consumer;
  memset(e->path, 0, sizeof(e->path));
  if (path) {
    size_t n = strlen(path);
    if (n >= ABI_EVENT_PATH_MAX) n = ABI_EVENT_PATH_MAX - 1;
    memcpy(e->path, path, n);
  }
}

static void *obs_alloc_aligned(AbiReconstruction *r, size_t size,
                               size_t align) {
  size_t      total;
  uint8_t    *base;
  uintptr_t   start, aligned;
  AbiAllocHdr h;

  if (align == 0) align = 1;
  if (size > SIZE_MAX - align - sizeof(AbiAllocHdr)) return NULL;
  total = size + align + sizeof(AbiAllocHdr);
  base = (uint8_t *)malloc(total);
  if (!base) return NULL;

  start = (uintptr_t)base + sizeof(AbiAllocHdr);
  aligned = (start + (uintptr_t)align - 1u) & ~((uintptr_t)align - 1u);

  h.base = base;
  h.size = (uint64_t)size;
  h.magic = ABI_ALLOC_MAGIC;
  memcpy((void *)(aligned - sizeof(AbiAllocHdr)), &h, sizeof(h));

  r->obs.alloc.bytes_allocated += (uint64_t)size;
  r->obs.alloc.blocks_allocated++;
  return (void *)aligned;
}

static void *obs_alloc(AbiReconstruction *r, size_t size) {
  void *p = obs_alloc_aligned(r, size, sizeof(void *));
  if (p && size) memset(p, 0, size);
  return p;
}

static void obs_free(AbiReconstruction *r, void *p) {
  AbiAllocHdr h;
  if (!p) return;
  memcpy(&h, (uint8_t *)p - sizeof(h), sizeof(h));
  /*
   * The magic is not paranoia about our own code: it catches a consumer that
   * hands back a pointer we never produced, which would otherwise corrupt the
   * counters that the leak verdict is computed from.
   */
  if (h.magic != ABI_ALLOC_MAGIC) return;
  r->obs.alloc.bytes_freed += h.size;
  r->obs.alloc.blocks_freed++;
  free(h.base);
}

/* --- release callbacks ---------------------------------------------------- */

/*
 * private_data is always live when a release callback runs. The consumer
 * releases the base structure exactly once; the callback clears `release` and
 * `private_data` on the way out, so the pointer cannot be reached again through
 * a conforming path. There is deliberately no NULL guard here: a crash would be
 * loud and attributable, whereas a silent early return would hide a defect in
 * this file.
 */
static void schema_release(struct ArrowSchema *s) {
  AbiNodePriv       *p = (AbiNodePriv *)s->private_data;
  AbiReconstruction *r = p->rec;
  uint32_t           i, slots = p->child_slots;
  uint8_t            is_root = p->is_root;
  char               path[ABI_EVENT_PATH_MAX];
  uint8_t            by_consumer = (r->obs.release_depth == 0) ? 1u : 0u;

  memcpy(path, p->path, sizeof(path));

  if (by_consumer && !is_root) {
    /* The consumer must release only the base structure; the producer's own
       release is what walks into the children. */
    r->obs.violations++;
    obs_event(r, ABI_EV_VIOLATION_CHILD_RELEASED_BY_CONSUMER, path, 1);
  }
  obs_event(r, ABI_EV_SCHEMA_RELEASE_ENTER, path, by_consumer);
  r->obs.release_depth++;

  for (i = 0; i < slots; i++) {
    struct ArrowSchema *ch = s->children ? s->children[i] : NULL;
    if (!ch) continue;
    if (ch->release) ch->release(ch);
    obs_free(r, ch);
  }
  obs_free(r, s->children);
  if (s->dictionary) {
    if (s->dictionary->release) s->dictionary->release(s->dictionary);
    obs_free(r, s->dictionary);
  }
  obs_free(r, p->format_buf);
  obs_free(r, p->name_buf);
  obs_free(r, p->metadata_buf);

  r->obs.release_depth--;
  obs_event(r, ABI_EV_SCHEMA_RELEASE_EXIT, path, by_consumer);

  obs_free(r, p);
  s->format = NULL;
  s->name = NULL;
  s->metadata = NULL;
  s->children = NULL;
  s->dictionary = NULL;
  s->private_data = NULL;
  s->release = NULL; /* marks the structure released */
}

static void free_backing_allocations(AbiReconstruction *r) {
  uint32_t i;
  if (r->buffers_freed) return;
  for (i = 0; i < r->alloc_count; i++)
    obs_free(r, r->alloc_data[i]);
  obs_free(r, r->alloc_data);
  r->alloc_data = NULL;
  r->buffers_freed = 1;
}

static void array_release(struct ArrowArray *a) {
  AbiNodePriv       *p = (AbiNodePriv *)a->private_data;
  AbiReconstruction *r = p->rec;
  uint32_t           i, slots = p->child_slots;
  uint8_t            is_root = p->is_root;
  char               path[ABI_EVENT_PATH_MAX];
  uint8_t            by_consumer = (r->obs.release_depth == 0) ? 1u : 0u;

  memcpy(path, p->path, sizeof(path));

  if (by_consumer && !is_root) {
    r->obs.violations++;
    obs_event(r, ABI_EV_VIOLATION_CHILD_RELEASED_BY_CONSUMER, path, 1);
  }
  obs_event(r, ABI_EV_ARRAY_RELEASE_ENTER, path, by_consumer);
  r->obs.release_depth++;

  for (i = 0; i < slots; i++) {
    struct ArrowArray *ch = a->children ? a->children[i] : NULL;
    if (!ch) continue;
    if (ch->release) ch->release(ch);
    obs_free(r, ch);
  }
  obs_free(r, a->children);
  if (a->dictionary) {
    if (a->dictionary->release) a->dictionary->release(a->dictionary);
    obs_free(r, a->dictionary);
  }
  obs_free(r, a->buffers);

  /*
   * Buffer memory belongs to the array, and the root's release is where a
   * producer frees it. Doing it here rather than per node is what keeps aliased
   * buffers from being freed twice: many views, one allocation, one free.
   */
  if (is_root) free_backing_allocations(r);

  r->obs.release_depth--;
  obs_event(r, ABI_EV_ARRAY_RELEASE_EXIT, path, by_consumer);

  obs_free(r, p);
  a->buffers = NULL;
  a->children = NULL;
  a->dictionary = NULL;
  a->private_data = NULL;
  a->release = NULL;
}

/* --- construction --------------------------------------------------------- */

static void child_path(char *dst, size_t dst_size, const char *parent,
                       const char *leaf) {
  size_t n = strlen(parent);
  if (n + strlen(leaf) + 1 >= dst_size) {
    /* Paths are report cosmetics; a deep tree truncates rather than overflows.
     */
    memcpy(dst, parent, dst_size - 1);
    dst[dst_size - 1] = '\0';
    return;
  }
  memcpy(dst, parent, n);
  memcpy(dst + n, leaf, strlen(leaf) + 1);
}

static AbiNodePriv *new_priv(AbiReconstruction *r, const char *path,
                             uint8_t is_root) {
  AbiNodePriv *p = (AbiNodePriv *)obs_alloc(r, sizeof(AbiNodePriv));
  if (!p) return NULL;
  p->rec = r;
  p->is_root = is_root;
  memset(p->path, 0, sizeof(p->path));
  if (path) {
    size_t n = strlen(path);
    if (n >= ABI_EVENT_PATH_MAX) n = ABI_EVENT_PATH_MAX - 1;
    memcpy(p->path, path, n);
  }
  return p;
}

/* NUL-terminated copy: Arrow format and name are C strings. */
static char *dup_cstr(AbiReconstruction *r, AbiBytes b) {
  char *s = (char *)obs_alloc(r, (size_t)b.size + 1);
  if (!s) return NULL;
  if (b.size) memcpy(s, b.data, b.size);
  s[b.size] = '\0';
  return s;
}

/*
 * Arrow's metadata wire form, built with NATIVE-endian int32 on purpose:
 *
 *   int32 n_kv, then n_kv times (int32 key_len, key, int32 value_len, value)
 *
 * This is the exact opposite of the .abicase rule that every integer is
 * little-endian, and it is why the file stores metadata decoded into key/value
 * pairs. Do not "fix" this to use abi_store_u32: on a big-endian consumer that
 * would produce metadata it cannot read.
 */
static char *encode_metadata(AbiReconstruction *r, const AbiSchemaNode *n,
                             size_t *out_size) {
  size_t   total = sizeof(int32_t);
  uint32_t i;
  char    *buf;
  size_t   off;
  int32_t  tmp;

  for (i = 0; i < n->metadata_count; i++) {
    total += sizeof(int32_t) + n->metadata[i].key.size;
    total += sizeof(int32_t) + n->metadata[i].value.size;
  }
  buf = (char *)obs_alloc(r, total);
  if (!buf) return NULL;

  tmp = (int32_t)n->metadata_count;
  memcpy(buf, &tmp, sizeof(tmp));
  off = sizeof(tmp);
  for (i = 0; i < n->metadata_count; i++) {
    tmp = (int32_t)n->metadata[i].key.size;
    memcpy(buf + off, &tmp, sizeof(tmp));
    off += sizeof(tmp);
    if (n->metadata[i].key.size) {
      memcpy(buf + off, n->metadata[i].key.data, n->metadata[i].key.size);
      off += n->metadata[i].key.size;
    }
    tmp = (int32_t)n->metadata[i].value.size;
    memcpy(buf + off, &tmp, sizeof(tmp));
    off += sizeof(tmp);
    if (n->metadata[i].value.size) {
      memcpy(buf + off, n->metadata[i].value.data, n->metadata[i].value.size);
      off += n->metadata[i].value.size;
    }
  }
  *out_size = total;
  return buf;
}

static uint32_t max_u32(uint32_t a, uint32_t b) { return (a > b) ? a : b; }

static int build_schema(AbiReconstruction *r, const AbiSchemaNode *n,
                        struct ArrowSchema *out, const char *path,
                        uint8_t is_root) {
  AbiNodePriv *p;
  uint32_t     slots, i;

  memset(out, 0, sizeof(*out));
  p = new_priv(r, path, is_root);
  if (!p) return 0;
  /*
   * Armed before the contents are built, not after. If any allocation below
   * fails, this node is still releasable and abi_reconstruction_free() can walk
   * the partially-built tree and free it; setting these at the end would leak
   * everything allocated up to the failure.
   */
  out->private_data = p;
  out->release = schema_release;

  p->format_buf = dup_cstr(r, n->format);
  if (!p->format_buf) return 0;
  out->format = p->format_buf;

  if (n->has_name) {
    p->name_buf = dup_cstr(r, n->name);
    if (!p->name_buf) return 0;
    out->name = p->name_buf;
  }
  if (n->has_metadata) {
    size_t msize = 0;
    p->metadata_buf = encode_metadata(r, n, &msize);
    if (!p->metadata_buf) return 0;
    out->metadata = p->metadata_buf;
  }
  out->flags = n->flags;

  /*
   * Slots cover both counts. When a B1 case declares more children than it
   * provides, the extra slots are real, NULL-initialized pointers: a consumer
   * that walks n_children dereferences a NULL child, which is the defect under
   * test and is unambiguously in the consumer. Allocating only the provided
   * count would instead have the consumer read past our array, making the fault
   * ours and aborting the harness under a sanitizer before the consumer is even
   * reached.
   */
  slots = max_u32(n->n_children, n->child_count);
  p->child_slots = slots;
  if (slots) {
    out->children = (struct ArrowSchema **)obs_alloc(
        r, slots * sizeof(struct ArrowSchema *));
    if (!out->children) return 0;
  }
  for (i = 0; i < n->child_count; i++) {
    char                cpath[ABI_EVENT_PATH_MAX];
    char                leaf[16];
    struct ArrowSchema *ch =
        (struct ArrowSchema *)obs_alloc(r, sizeof(struct ArrowSchema));
    if (!ch) return 0;
    out->children[i] = ch;
    snprintf(leaf, sizeof(leaf), "%u/", (unsigned)i);
    child_path(cpath, sizeof(cpath), path, leaf);
    if (!build_schema(r, n->children[i], ch, cpath, 0)) return 0;
  }
  out->n_children =
      (int64_t)n->n_children; /* declared: may be a deliberate lie */

  if (n->dictionary) {
    char                cpath[ABI_EVENT_PATH_MAX];
    struct ArrowSchema *d =
        (struct ArrowSchema *)obs_alloc(r, sizeof(struct ArrowSchema));
    if (!d) return 0;
    out->dictionary = d;
    child_path(cpath, sizeof(cpath), path, "dict/");
    if (!build_schema(r, n->dictionary, d, cpath, 0)) return 0;
  }

  return 1;
}

static int build_array(AbiReconstruction *r, const AbiArrayNode *n,
                       struct ArrowArray *out, const char *path,
                       uint8_t is_root) {
  AbiNodePriv *p;
  uint32_t     slots, i;

  memset(out, 0, sizeof(*out));
  p = new_priv(r, path, is_root);
  if (!p) return 0;
  out->private_data = p; /* armed early: see build_schema() */
  out->release = array_release;

  out->length = n->length;
  out->null_count = n->null_count;
  out->offset = n->offset;

  slots = max_u32(n->n_buffers, n->buffer_count);
  if (slots) {
    out->buffers = (const void **)obs_alloc(r, slots * sizeof(const void *));
    if (!out->buffers) return 0;
  }
  for (i = 0; i < n->buffer_count; i++) {
    const AbiBufferView *v = &n->buffers[i];
    if (!v->present) {
      out->buffers[i] = NULL;
      continue;
    }
    /* Views into one shared allocation: aliasing is a real address match. */
    out->buffers[i] =
        (const void *)((uint8_t *)r->alloc_data[v->allocation_id] +
                       v->byte_offset);
  }
  out->n_buffers = (int64_t)n->n_buffers;

  slots = max_u32(n->n_children, n->child_count);
  p->child_slots = slots;
  if (slots) {
    out->children =
        (struct ArrowArray **)obs_alloc(r, slots * sizeof(struct ArrowArray *));
    if (!out->children) return 0;
  }
  for (i = 0; i < n->child_count; i++) {
    char               cpath[ABI_EVENT_PATH_MAX];
    char               leaf[16];
    struct ArrowArray *ch =
        (struct ArrowArray *)obs_alloc(r, sizeof(struct ArrowArray));
    if (!ch) return 0;
    out->children[i] = ch;
    snprintf(leaf, sizeof(leaf), "%u/", (unsigned)i);
    child_path(cpath, sizeof(cpath), path, leaf);
    if (!build_array(r, n->children[i], ch, cpath, 0)) return 0;
  }
  out->n_children = (int64_t)n->n_children;

  if (n->dictionary) {
    char               cpath[ABI_EVENT_PATH_MAX];
    struct ArrowArray *d =
        (struct ArrowArray *)obs_alloc(r, sizeof(struct ArrowArray));
    if (!d) return 0;
    out->dictionary = d;
    child_path(cpath, sizeof(cpath), path, "dict/");
    if (!build_array(r, n->dictionary, d, cpath, 0)) return 0;
  }

  return 1;
}

AbiStatus abi_reconstruct(const AbiCase *c, AbiReconstruction **out,
                          AbiError *err) {
  AbiReconstruction *r;
  uint32_t           i;

  if (!c || !out) return ABI_ERR_INVALID_ARGUMENT;
  if (err) {
    err->status = ABI_OK;
    err->offset = 0;
    err->message[0] = '\0';
  }
  if (!c->schema) return ABI_ERR_MISSING_SECTION;

  r = (AbiReconstruction *)calloc(1, sizeof(AbiReconstruction));
  if (!r) return ABI_ERR_NO_MEMORY;

  r->alloc_count = c->alloc_count;
  if (c->alloc_count) {
    r->alloc_data = (void **)obs_alloc(r, c->alloc_count * sizeof(void *));
    if (!r->alloc_data) {
      abi_reconstruction_free(r);
      return ABI_ERR_NO_MEMORY;
    }
    for (i = 0; i < c->alloc_count; i++) {
      const AbiAllocation *a = &c->allocations[i];
      /*
       * One real allocation per case allocation, at the alignment the case
       * asked for. Misalignment is expressed by a view's byte_offset, never by
       * weakening this, so that "misaligned" means exactly what the case says
       * and not whatever the platform allocator happened to return.
       */
      void *mem = obs_alloc_aligned(
          r, (size_t)a->size_bytes ? (size_t)a->size_bytes : 1, a->alignment);
      if (!mem) {
        abi_reconstruction_free(r);
        return ABI_ERR_NO_MEMORY;
      }
      if (a->size_bytes) memcpy(mem, a->bytes, (size_t)a->size_bytes);
      r->alloc_data[i] = mem;
    }
  }

  if (!build_schema(r, c->schema, &r->schema, "/", 1)) {
    abi_reconstruction_free(r);
    return ABI_ERR_NO_MEMORY;
  }
  obs_event(r, ABI_EV_SCHEMA_EXPORTED, "/", 0);

  if (c->array) {
    if (!build_array(r, c->array, &r->array, "/", 1)) {
      abi_reconstruction_free(r);
      return ABI_ERR_NO_MEMORY;
    }
    r->has_array = 1;
    obs_event(r, ABI_EV_ARRAY_EXPORTED, "/", 0);
  }

  *out = r;
  return ABI_OK;
}

struct ArrowSchema *abi_reconstruction_schema(AbiReconstruction *r) {
  return r ? &r->schema : NULL;
}

struct ArrowArray *abi_reconstruction_array(AbiReconstruction *r) {
  return (r && r->has_array) ? &r->array : NULL;
}

const AbiObserver *abi_reconstruction_observer(const AbiReconstruction *r) {
  return r ? &r->obs : NULL;
}

int abi_reconstruction_leaked(const AbiReconstruction *r) {
  if (!r) return 0;
  return (r->obs.alloc.bytes_allocated != r->obs.alloc.bytes_freed) ||
         (r->obs.alloc.blocks_allocated != r->obs.alloc.blocks_freed);
}

void abi_reconstruction_release_all(AbiReconstruction *r) {
  if (!r) return;
  /*
   * Whatever the consumer left live is released here, and logged as
   * HARNESS_RELEASED so the report can tell "the consumer released it" from
   * "we cleaned up after a consumer that did not".
   *
   * Separate from free() on purpose: the event log and the allocation counters
   * live in the reconstruction, so a harness that wants to report on the
   * cleanup has to be able to release first and read afterwards.
   */
  if (r->schema.release) {
    obs_event(r, ABI_EV_HARNESS_RELEASED, "/", 0);
    r->schema.release(&r->schema);
  }
  if (r->has_array && r->array.release) {
    obs_event(r, ABI_EV_HARNESS_RELEASED, "/", 0);
    r->array.release(&r->array);
  }
  free_backing_allocations(r);
}

void abi_reconstruction_free(AbiReconstruction *r) {
  if (!r) return;
  abi_reconstruction_release_all(r);
  free(r);
}

void abi_reconstruction_print_log(const AbiReconstruction *r, FILE *out) {
  uint32_t i;
  if (!r || !out) return;

  fprintf(out, "lifecycle event log (%lu event(s)%s):\n",
          (unsigned long)r->obs.event_count,
          r->obs.events_dropped ? ", TRUNCATED" : "");
  for (i = 0; i < r->obs.event_count; i++) {
    const AbiEvent *e = &r->obs.events[i];
    fprintf(out, "  %03lu %*s%-38s %-8s%s\n", (unsigned long)e->seq,
            (int)(e->depth * 2), "", abi_event_kind_str((AbiEventKind)e->kind),
            e->path[0] ? e->path : "-",
            e->by_consumer ? "  <- entered by consumer" : "");
  }
  fprintf(
      out, "  ALLOC_DELTA  %lld bytes / %lld blocks\n",
      (long long)(r->obs.alloc.bytes_allocated - r->obs.alloc.bytes_freed),
      (long long)(r->obs.alloc.blocks_allocated - r->obs.alloc.blocks_freed));
  fprintf(out, "  violations:  %lu\n", (unsigned long)r->obs.violations);
}
