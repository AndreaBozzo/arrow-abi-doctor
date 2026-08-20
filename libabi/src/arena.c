#include <stdlib.h>

#include "abi_internal.h"

#define ABI_ARENA_MIN_CHUNK ((size_t)4096)

typedef struct AbiArenaChunk {
  struct AbiArenaChunk *next;
  uint8_t              *data;
  size_t                capacity;
  size_t                used;
} AbiArenaChunk;

struct AbiArena {
  AbiArenaChunk *head;
  size_t         used_total;
};

AbiArena *abi_arena_new(void) {
  AbiArena *a = (AbiArena *)calloc(1, sizeof(AbiArena));
  return a;
}

void abi_arena_free(AbiArena *a) {
  AbiArenaChunk *ch;
  if (!a) return;
  ch = a->head;
  while (ch) {
    AbiArenaChunk *next = ch->next;
    free(ch->data);
    free(ch);
    ch = next;
  }
  free(a);
}

size_t abi_arena_bytes_used(const AbiArena *a) { return a ? a->used_total : 0; }

static int abi_is_pow2(size_t v) { return v != 0 && (v & (v - 1)) == 0; }

void *abi_arena_alloc(AbiArena *a, size_t size, size_t align) {
  AbiArenaChunk *ch;
  size_t         cap, want;

  if (!a) return NULL;
  if (align == 0) align = 1;
  if (!abi_is_pow2(align)) return NULL;
  if (size == 0)
    size = 1; /* distinct, non-NULL address for zero-size requests */

  ch = a->head;
  if (ch) {
    size_t off = (ch->used + align - 1u) & ~(align - 1u);
    if (off <= ch->capacity && size <= ch->capacity - off) {
      ch->used = off + size;
      a->used_total += size;
      return ch->data + off;
    }
  }

  if (size > SIZE_MAX - align) return NULL;
  want = size + align;
  cap = ABI_ARENA_MIN_CHUNK;
  while (cap < want) {
    if (cap > SIZE_MAX / 2) return NULL;
    cap *= 2;
  }

  ch = (AbiArenaChunk *)calloc(1, sizeof(AbiArenaChunk));
  if (!ch) return NULL;
  ch->data = (uint8_t *)malloc(cap);
  if (!ch->data) {
    free(ch);
    return NULL;
  }
  ch->capacity = cap;
  ch->used = size;
  ch->next = a->head;
  a->head = ch;
  a->used_total += size;
  return ch->data;
}

void *abi_arena_calloc(AbiArena *a, size_t size, size_t align) {
  void *p = abi_arena_alloc(a, size, align);
  if (p && size) memset(p, 0, size);
  return p;
}

void *abi_arena_dup(AbiArena *a, const void *src, size_t size, size_t align) {
  void *p = abi_arena_alloc(a, size, align);
  if (p && src && size) memcpy(p, src, size);
  return p;
}

int abi_arena_bytes(AbiArena *a, AbiBytes *out, const void *src,
                    uint32_t size) {
  if (!a || !out) return 0;
  if (size == 0) {
    /*
     * Empty but not absent: a non-NULL pointer keeps "" distinguishable from a
     * cleared field at every layer above, which the format relies on for
     * ArrowSchema.name.
     */
    out->data = (const uint8_t *)abi_arena_alloc(a, 1, 1);
    out->size = 0;
    return out->data != NULL;
  }
  out->data = (const uint8_t *)abi_arena_dup(a, src, size, 1);
  out->size = size;
  return out->data != NULL;
}

int abi_arena_cstr(AbiArena *a, AbiBytes *out, const char *s) {
  size_t len = s ? strlen(s) : 0;
  if (len > ABI_LIMIT_BYTES_LEN) return 0;
  return abi_arena_bytes(a, out, s, (uint32_t)len);
}
