/*
 * abicase-gen -- instantiate the bounded conformance model as .abicase files.
 *
 * The model is docs/coverage-matrix.md; its executable form is
 * tools/coverage_matrix.py. This program deliberately does *not* re-derive the
 * product. It reads one accepted tuple per line -- exactly what
 * `coverage_matrix.py --list` prints -- and builds the case that tuple
 * describes. A second enumeration would be a second model, and the drift
 * between them would surface as a coverage figure rather than as a failure.
 *
 * Three decisions the corpus rests on:
 *
 * 1. **Nothing tuple-derived enters the payload.** The case id is a digest over
 *    the payload (format 2.3), and the provenance and expected blocks are
 *    inside it. Stamping the tuple into `notes`, or deriving a per-case seed
 *    from it, would make every id distinct by construction and turn the
 *    uniqueness check into a tautology. The tuple lives in manifest.tsv
 *    instead, so a duplicate id means what it should: two cells of the model
 *    built the same case, and one of them is redundant.
 *
 * 2. **Buffer bytes are written little-endian explicitly**, never memcpy'd from
 *    a host integer, so the same tuple yields the same bytes -- and therefore
 *    the same case id -- on every host. What a given int32 *means* still does
 *    not survive an endianness change; that is format 10.1 and is unchanged.
 *
 * 3. **Slots outside the [offset, offset+length) window carry deliberately
 *    different values.** A consumer that ignores ArrowArray.offset then reads
 *    visibly wrong data rather than plausible data, which is what makes the
 *    offset-classes of the model detectable by a digest instead of only by a
 *    crash.
 *
 * No randomness is involved anywhere: every byte is a function of the tuple.
 * The provenance seed is therefore 0 with an empty rng_algorithm, which is the
 * format's encoding for "no RNG was used" (format 4).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi/abicase.h"
#include "abi/arrow_abi.h"

#define GEN_VERSION       "corpus-a/1"
#define GEN_SPEC_REVISION "arrow-c-data-2026-03"
#define GEN_ALIGNMENT     64u /* Arrow's recommended allocation alignment */
#define GEN_MAX_BUFFERS   3
#define GEN_MAX_STR       3 /* longest string gen_str() produces */
/*
 * Wide enough for the longest message any check below can build: a
 * duplicate-id report carries two whole tuples and a write failure carries
 * a whole path. Sized rather than truncated because GCC computes the worst
 * case and -Wformat-truncation is an error in this tree -- which only the
 * cross build noticed, its glibc headers being the ones with _FORTIFY.
 */
#define GEN_ERR_SIZE 640

/* --- the model tuple ----------------------------------------------------- */

typedef enum { GT_INT32 = 0, GT_INT64, GT_FLOAT64, GT_UTF8, GT_BOOL } GenType;

typedef enum { GN_NONE = 0, GN_ALL, GN_ALTERNATING, GN_SPARSE } GenNulls;

typedef enum {
  GB_NORMAL = 0,
  GB_EMPTY,
  GB_OMITTED_VALIDITY,
  GB_ALIASED
} GenBufState;

typedef enum {
  GL_DIRECT = 0,
  GL_MOVED,
  GL_STREAMED,
  GL_EARLY_RELEASE,
  GL_EOF
} GenLifecycle;

typedef struct {
  GenType      type;
  int          length_class;
  int          offset_class;
  GenNulls     nulls;
  GenBufState  buffers;
  int          align_class;
  GenLifecycle lifecycle;
  int64_t      length;
  int64_t      offset;
  uint32_t     shift; /* alignment-class as a byte shift: 0, 1 or 4 */
} GenTuple;

static const char *const TYPE_NAMES[] = {"int32", "int64", "float64", "utf8",
                                         "bool"};
static const char *const LENGTH_NAMES[] = {"zero", "one", "small", "medium"};
static const int64_t     LENGTH_VALUES[] = {0, 1, 9, 1024};
static const char *const OFFSET_NAMES[] = {"0", "1", "7", "8", "9"};
static const int64_t     OFFSET_VALUES[] = {0, 1, 7, 8, 9};
static const char *const NULL_NAMES[] = {"none", "all", "alternating",
                                         "sparse"};
static const char *const BUFFER_NAMES[] = {"normal", "empty",
                                           "omitted-validity", "aliased"};
static const char *const ALIGN_NAMES[] = {"natural", "+1", "+4"};
static const uint32_t    ALIGN_SHIFTS[] = {0u, 1u, 4u};
static const char *const LIFECYCLE_NAMES[] = {"direct", "moved", "streamed",
                                              "early-release", "EOF"};

/*
 * Lifecycles this generator can build. The three stream lifecycles need the C
 * Stream Interface (issue #4). They are skipped and counted, never silently
 * dropped: a partial corpus that does not say which cells it left out is a
 * coverage claim with a hole in it.
 */
static int lifecycle_supported(GenLifecycle l) {
  return l == GL_DIRECT || l == GL_MOVED;
}

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static int name_index(const char *const *names, size_t count, const char *s) {
  size_t i;
  for (i = 0; i < count; i++) {
    if (strcmp(names[i], s) == 0) return (int)i;
  }
  return -1;
}

/* --- byte helpers -------------------------------------------------------- */

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_le64(uint8_t *p, uint64_t v) {
  put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
  put_le32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

/* Arrow validity and boolean bitmaps are least-significant-bit first. */
static void set_bit(uint8_t *bitmap, int64_t i, int one) {
  uint8_t mask = (uint8_t)(1u << (unsigned)(i & 7));
  if (one) {
    bitmap[i >> 3] |= mask;
  } else {
    bitmap[i >> 3] &= (uint8_t)~mask;
  }
}

static uint64_t round_up(uint64_t v, uint64_t to) {
  return (v + to - 1u) / to * to;
}

/* --- values -------------------------------------------------------------- */

/*
 * Is logical slot `i` null under this pattern? Patterns are defined over
 * logical indices, that is relative to ArrowArray.offset, so the physical bit
 * positions move with the offset-class (model 1.4).
 */
static int slot_is_null(const GenTuple *t, int64_t i) {
  switch (t->nulls) {
  case GN_NONE: return 0;
  case GN_ALL: return 1;
  case GN_ALTERNATING: return (i % 2) == 0;
  case GN_SPARSE: return i == t->length - 1;
  }
  return 0;
}

static int64_t null_count_of(const GenTuple *t) {
  int64_t i, n = 0;
  for (i = 0; i < t->length; i++) {
    if (slot_is_null(t, i)) n++;
  }
  return n;
}

/* The integer value at physical index p; negative outside the window. */
static int64_t gen_int(const GenTuple *t, int64_t p) {
  return (p < t->offset) ? -(p + 1) : 1000 + (p - t->offset);
}

/*
 * The float64 bit pattern at physical index p, built as bits rather than as a
 * double so that -0.0 and the NaN payload are exact and host-independent.
 * Logical slots 0 and 1 carry -0.0 and a quiet NaN: those are the two values
 * whose normalization the logical digest (issue #7) has to state a rule for,
 * and a rule with no case behind it is untested.
 */
static uint64_t gen_f64_bits(const GenTuple *t, int64_t p) {
  double   d;
  uint64_t bits;
  int64_t  i;

  if (p < t->offset) {
    d = -(double)(p + 1);
  } else {
    i = p - t->offset;
    if (i == 0) return 0x8000000000000000ull; /* -0.0 */
    if (i == 1) return 0x7FF8000000000001ull; /* quiet NaN, payload 1 */
    d = (double)i + 0.5;
  }
  memcpy(&bits, &d, sizeof(bits));
  return bits;
}

/* The boolean value at physical index p. */
static int gen_bool(const GenTuple *t, int64_t p) {
  return (p < t->offset) ? 0 : ((p - t->offset) % 3) != 0;
}

/* The string at physical index p, into `out` (GEN_MAX_STR bytes). */
static size_t gen_str(const GenTuple *t, int64_t p, char *out) {
  int64_t i;
  size_t  len, k;

  if (p < t->offset) {
    out[0] = 'X';
    return 1;
  }
  i = p - t->offset;
  len = (size_t)(i % 3) + 1;
  for (k = 0; k < len; k++) {
    out[k] = (char)('a' + (int)(i % 26));
  }
  return len;
}

/* --- buffers ------------------------------------------------------------- */

typedef struct {
  AbiBufferRole role;
  int           present; /* 0 => the reconstructed pointer is NULL */
  uint64_t      size;    /* payload bytes */
  uint32_t      width;   /* natural placement granularity, for aliasing */
  uint8_t      *bytes;   /* size bytes, owned by the caller */
} GenBuf;

static void free_buffers(GenBuf *bufs, int count) {
  int i;
  for (i = 0; i < count; i++) {
    free(bufs[i].bytes);
    bufs[i].bytes = NULL;
  }
}

/* A buffer of `size` bytes, zeroed; size 0 is not produced here. */
static uint8_t *new_buf(uint64_t size) {
  return (uint8_t *)calloc((size_t)size, 1);
}

/*
 * Build every buffer of the type, sized for offset + length elements with a
 * floor of one byte (model 1.5). Returns the count, or -1 if out of memory.
 */
static int build_buffers(const GenTuple *t, GenBuf *out) {
  int64_t  n = t->offset + t->length; /* physical element count */
  int64_t  p;
  uint64_t vbytes = (uint64_t)((n + 7) / 8);
  int      count = 0;

  if (vbytes == 0) vbytes = 1;
  memset(out, 0, sizeof(*out) * GEN_MAX_BUFFERS);

  /* validity, always buffer 0 */
  out[0].role = ABI_ROLE_VALIDITY;
  out[0].present = (t->buffers != GB_OMITTED_VALIDITY);
  out[0].size = vbytes;
  out[0].width = 1;
  out[0].bytes = new_buf(vbytes);
  if (!out[0].bytes) return -1;
  for (p = 0; p < n; p++) {
    /*
     * Slots before the window are marked valid while the window itself carries
     * the pattern, so "ignored the offset" and "misread the pattern" are two
     * different observations rather than one.
     */
    int valid = (p < t->offset) ? 1 : !slot_is_null(t, p - t->offset);
    set_bit(out[0].bytes, p, valid);
  }
  count = 1;

  switch (t->type) {
  case GT_INT32:
  case GT_INT64:
  case GT_FLOAT64: {
    uint32_t w = (t->type == GT_INT32) ? 4u : 8u;
    uint64_t size = (uint64_t)n * w;
    if (size == 0) size = 1;
    out[1].role = ABI_ROLE_DATA;
    out[1].present = 1;
    out[1].size = size;
    out[1].width = w;
    out[1].bytes = new_buf(size);
    if (!out[1].bytes) return -1;
    for (p = 0; p < n; p++) {
      if (t->type == GT_INT32) {
        put_le32(out[1].bytes + p * 4, (uint32_t)(int32_t)gen_int(t, p));
      } else if (t->type == GT_INT64) {
        put_le64(out[1].bytes + p * 8, (uint64_t)gen_int(t, p));
      } else {
        put_le64(out[1].bytes + p * 8, gen_f64_bits(t, p));
      }
    }
    count = 2;
    break;
  }
  case GT_BOOL: {
    uint64_t size = vbytes;
    out[1].role = ABI_ROLE_DATA;
    out[1].present = 1;
    out[1].size = size;
    out[1].width = 1;
    out[1].bytes = new_buf(size);
    if (!out[1].bytes) return -1;
    for (p = 0; p < n; p++) {
      set_bit(out[1].bytes, p, gen_bool(t, p));
    }
    count = 2;
    break;
  }
  case GT_UTF8: {
    uint64_t osize = ((uint64_t)n + 1u) * 4u;
    uint64_t total = 0, dsize;
    char     s[GEN_MAX_STR];

    out[1].role = ABI_ROLE_OFFSETS;
    out[1].present = 1;
    out[1].size = osize;
    out[1].width = 4;
    out[1].bytes = new_buf(osize);
    if (!out[1].bytes) return -1;
    for (p = 0; p < n; p++) {
      put_le32(out[1].bytes + p * 4, (uint32_t)total);
      total += (uint64_t)gen_str(t, p, s);
    }
    put_le32(out[1].bytes + n * 4, (uint32_t)total);

    dsize = total ? total : 1;
    out[2].role = ABI_ROLE_DATA;
    out[2].present = 1;
    out[2].size = dsize;
    out[2].width = 1;
    out[2].bytes = new_buf(dsize);
    if (!out[2].bytes) return -1;
    total = 0;
    for (p = 0; p < n; p++) {
      size_t len = gen_str(t, p, s);
      memcpy(out[2].bytes + total, s, len);
      total += len;
    }
    count = 3;
    break;
  }
  }
  return count;
}

/* --- case construction --------------------------------------------------- */

/* One allocation holding `size` payload bytes placed `shift` bytes in. */
static AbiStatus add_shifted_alloc(AbiCase *c, const uint8_t *payload,
                                   uint64_t size, uint32_t shift,
                                   uint32_t *out_id) {
  uint8_t  *tmp;
  AbiStatus st;

  tmp = (uint8_t *)calloc((size_t)(size + shift), 1);
  if (!tmp) return ABI_ERR_NO_MEMORY;
  memcpy(tmp + shift, payload, (size_t)size);
  st = abi_case_add_allocation(c, tmp, size + shift, GEN_ALIGNMENT, out_id);
  free(tmp);
  return st;
}

/*
 * Attach the buffers to the array node, realizing the buffer-state class.
 *
 * The alignment class is a byte shift of the *view* within a 64-byte-aligned
 * allocation (model 1.6), never a weaker allocation alignment: "misaligned"
 * then means exactly what the case says and nothing else.
 */
static AbiStatus attach_buffers(AbiCase *c, AbiArrayNode *a, const GenTuple *t,
                                const GenBuf *bufs, int count) {
  AbiStatus st;
  int       i;

  if (t->buffers == GB_ALIASED) {
    /*
     * One backing allocation for every buffer, at distinct correctly-placed
     * offsets -- what a producer with an arena actually does. The relative
     * placement is natural for each buffer's width; the whole block then moves
     * by the alignment-class shift, so the two dimensions stay independent.
     */
    uint64_t place[GEN_MAX_BUFFERS];
    uint64_t cursor = 0;
    uint8_t *image;
    uint32_t alias_id = 0;

    for (i = 0; i < count; i++) {
      cursor = round_up(cursor, bufs[i].width);
      place[i] = cursor;
      cursor += bufs[i].size;
    }
    image = (uint8_t *)calloc((size_t)cursor, 1);
    if (!image) return ABI_ERR_NO_MEMORY;
    for (i = 0; i < count; i++) {
      memcpy(image + place[i], bufs[i].bytes, (size_t)bufs[i].size);
    }
    st = add_shifted_alloc(c, image, cursor, t->shift, &alias_id);
    free(image);
    if (st != ABI_OK) return st;

    for (i = 0; i < count; i++) {
      st = abi_array_add_buffer(c, a, bufs[i].role, alias_id,
                                t->shift + place[i], bufs[i].size);
      if (st != ABI_OK) return st;
    }
    return ABI_OK;
  }

  for (i = 0; i < count; i++) {
    uint32_t id = 0;

    if (!bufs[i].present) {
      st = abi_array_add_null_buffer(c, a, bufs[i].role);
      if (st != ABI_OK) return st;
      continue;
    }
    if (t->buffers == GB_EMPTY && bufs[i].role != ABI_ROLE_OFFSETS) {
      /*
       * A non-NULL buffer pointer over a zero-length allocation, which Arrow
       * distinguishes from a NULL one and so does the format (5.1). One
       * allocation per buffer rather than one shared: sharing would make this
       * class quietly also an aliasing case.
       *
       * The offsets buffer is excluded, which is why this class is not simply
       * "every buffer is empty" (model 1.5). The columnar format says an
       * offsets buffer "contains length + 1 signed integers", and the C Data
       * Interface puts the obligation on us: "The producer MUST ensure that
       * each contiguous buffer is large enough to represent length + offset
       * values". A zero-byte offsets buffer for a zero-length utf8 array
       * therefore describes an *invalid* array -- corpus B1, not this model --
       * and a consumer that sizes the buffer from the spec and reads it, as it
       * must since the interface transmits no buffer sizes, reads out of
       * bounds because of us. The values buffer is the one that is empty here.
       */
      st = abi_case_add_allocation(c, NULL, 0, GEN_ALIGNMENT, &id);
      if (st != ABI_OK) return st;
      st = abi_array_add_buffer(c, a, bufs[i].role, id, 0, 0);
      if (st != ABI_OK) return st;
      continue;
    }
    st = add_shifted_alloc(c, bufs[i].bytes, bufs[i].size, t->shift, &id);
    if (st != ABI_OK) return st;
    st = abi_array_add_buffer(c, a, bufs[i].role, id, t->shift, bufs[i].size);
    if (st != ABI_OK) return st;
  }
  return ABI_OK;
}

static const char *format_string(GenType t) {
  switch (t) {
  case GT_INT32: return "i";
  case GT_INT64: return "l";
  case GT_FLOAT64: return "g";
  case GT_UTF8: return "u";
  case GT_BOOL: return "b";
  }
  return NULL;
}

/*
 * ACCEPT unless the specification lets a consumer decline the case provided it
 * documents the limitation, which is a non-zero offset or a non-natural
 * alignment (model 5).
 *
 * Model 5 also lists "any type" as EITHER, on the ground that a consumer need
 * not support every data type. That is not encoded here: taken literally it
 * makes every case in the model EITHER and the field carries no information at
 * all, and a rejection at offset 0 with natural alignment stops being
 * distinguishable from a defect. Which types a given consumer supports is a
 * per-consumer judgement made once, in the compatibility matrix, not a property
 * of the case.
 */
static AbiExpectedOutcome outcome_of(const GenTuple *t) {
  return (t->offset != 0 || t->shift != 0) ? ABI_EXPECT_EITHER
                                           : ABI_EXPECT_ACCEPT;
}

static AbiCase *build_case(const GenTuple *t) {
  AbiCase       *c = abi_case_new(ABI_CLASS_A);
  AbiSchemaNode *root_s, *s;
  AbiArrayNode  *root_a, *a;
  GenBuf         bufs[GEN_MAX_BUFFERS];
  int            count;

  if (!c) return NULL;
  count = build_buffers(t, bufs);
  if (count < 0) {
    free_buffers(bufs, GEN_MAX_BUFFERS);
    abi_case_free(c);
    return NULL;
  }

  if (abi_case_set_provenance(c, 0, "", 0, GEN_VERSION, ABI_DOCTOR_VERSION,
                              GEN_SPEC_REVISION) != ABI_OK ||
      abi_case_set_expected(c, ABI_VALIDATE_FULL, outcome_of(t),
                            "C Data Interface: Structure definitions",
                            "corpus A, bounded conformance model 1") !=
          ABI_OK) {
    goto fail;
  }

  /*
   * One field inside a struct, not a bare top-level array.
   *
   * A bare `i` at the top level is legal C Data Interface, but it is not what
   * the consumers this corpus is aimed at import: pyarrow, Arrow C++ and DuckDB
   * all take a record batch, and a non-struct root is refused before any
   * dimension of the model has been looked at. A corpus every consumer declines
   * at the door would measure nothing. So the model's seven dimensions describe
   * the *field*, and the case delivers it the way a producer would.
   *
   * The struct root is deliberately fixed and uninteresting -- length L at
   * offset 0, no nulls, validity omitted -- so that everything varying between
   * cases is varying in the field.
   *
   * Names are explicit for the same reason as the shape: Arrow distinguishes an
   * absent name from an empty one, and the model has no dimension for it, so
   * the generator states its choice rather than inheriting a builder default.
   */
  root_s = abi_schema_new(c, "+s");
  s = abi_schema_new(c, format_string(t->type));
  if (!root_s || !s) goto fail;
  if (abi_schema_set_name(c, root_s, "") != ABI_OK ||
      abi_schema_set_name(c, s, "f0") != ABI_OK) {
    goto fail;
  }
  s->flags = ARROW_FLAG_NULLABLE;
  if (abi_schema_add_child(c, root_s, s) != ABI_OK) goto fail;
  abi_case_set_schema(c, root_s);

  root_a = abi_array_new(c);
  a = abi_array_new(c);
  if (!root_a || !a) goto fail;
  root_a->length = t->length;
  root_a->null_count = 0;
  root_a->offset = 0;
  if (abi_array_add_null_buffer(c, root_a, ABI_ROLE_VALIDITY) != ABI_OK) {
    goto fail;
  }
  a->length = t->length;
  a->null_count = null_count_of(t);
  a->offset = t->offset;
  if (attach_buffers(c, a, t, bufs, count) != ABI_OK) goto fail;
  if (abi_array_add_child(c, root_a, a) != ABI_OK) goto fail;
  abi_case_set_array(c, root_a);

  /*
   * `moved` is the same array, moved before it is handed over: bitwise copy to
   * new storage, source marked released, no callback (coverage-matrix 1.7).
   * The CALLSEQ executor performs it and the lifecycle state machine checks
   * that the one release then comes from the new location. The op is part of
   * the payload, so the two lifecycles of one array are two case ids.
   */
  if (t->lifecycle == GL_MOVED &&
      abi_case_add_op(c, ABI_OP_MOVE_STRUCT, 0, 0) != ABI_OK) {
    goto fail;
  }
  if (abi_case_add_op(c, ABI_OP_IMPORT_SCHEMA, 0, 0) != ABI_OK ||
      abi_case_add_op(c, ABI_OP_IMPORT_ARRAY, 0, 0) != ABI_OK ||
      abi_case_add_op(c, ABI_OP_RELEASE_BASE, 0, 0) != ABI_OK) {
    goto fail;
  }

  free_buffers(bufs, count);
  return c;

fail:
  free_buffers(bufs, count);
  abi_case_free(c);
  return NULL;
}

/* --- driver -------------------------------------------------------------- */

typedef struct {
  char id[ABICASE_ID_HEX_SIZE];
  char tuple[128];
} GenSeen;

typedef struct {
  GenSeen *seen;
  size_t   seen_count;
  size_t   seen_capacity;
  uint64_t total_bytes;
  uint64_t max_bytes;
  size_t   generated;
  size_t   skipped[COUNT_OF(LIFECYCLE_NAMES)];
} GenState;

static void tuple_text(const GenTuple *t, char *out, size_t size) {
  snprintf(out, size, "%s\t%s\t%s\t%s\t%s\t%s\t%s", TYPE_NAMES[t->type],
           LENGTH_NAMES[t->length_class], OFFSET_NAMES[t->offset_class],
           NULL_NAMES[t->nulls], BUFFER_NAMES[t->buffers],
           ALIGN_NAMES[t->align_class], LIFECYCLE_NAMES[t->lifecycle]);
}

/*
 * Parse one tab-separated tuple. An unrecognized value in any dimension is a
 * hard error rather than a skip: it means the Python model has grown a class
 * this generator cannot build, and the only safe reaction is to stop instead
 * of quietly emitting a corpus that is missing it.
 */
static int parse_tuple(const char *line, GenTuple *t, char *err, size_t errsz) {
  char   buf[256];
  char  *field[7];
  char  *p;
  int    i, idx;
  size_t len;
  static const struct {
    const char        *what;
    const char *const *names;
    size_t             count;
  } dims[7] = {{"type", TYPE_NAMES, COUNT_OF(TYPE_NAMES)},
               {"length", LENGTH_NAMES, COUNT_OF(LENGTH_NAMES)},
               {"offset", OFFSET_NAMES, COUNT_OF(OFFSET_NAMES)},
               {"nulls", NULL_NAMES, COUNT_OF(NULL_NAMES)},
               {"buffers", BUFFER_NAMES, COUNT_OF(BUFFER_NAMES)},
               {"alignment", ALIGN_NAMES, COUNT_OF(ALIGN_NAMES)},
               {"lifecycle", LIFECYCLE_NAMES, COUNT_OF(LIFECYCLE_NAMES)}};
  int values[7];

  len = strlen(line);
  if (len >= sizeof(buf)) {
    snprintf(err, errsz, "line of %lu bytes is too long", (unsigned long)len);
    return -1;
  }
  memcpy(buf, line, len + 1);

  p = buf;
  for (i = 0; i < 7; i++) {
    field[i] = p;
    if (i < 6) {
      p = strchr(p, '\t');
      if (!p) {
        snprintf(err, errsz, "expected 7 tab-separated fields, found %d",
                 i + 1);
        return -1;
      }
      *p++ = '\0';
    }
  }
  if (strchr(field[6], '\t')) {
    snprintf(err, errsz, "expected 7 tab-separated fields, found more");
    return -1;
  }

  for (i = 0; i < 7; i++) {
    idx = name_index(dims[i].names, dims[i].count, field[i]);
    if (idx < 0) {
      snprintf(err, errsz, "unknown %s class %s", dims[i].what, field[i]);
      return -1;
    }
    values[i] = idx;
  }

  t->type = (GenType)values[0];
  t->length_class = values[1];
  t->offset_class = values[2];
  t->nulls = (GenNulls)values[3];
  t->buffers = (GenBufState)values[4];
  t->align_class = values[5];
  t->lifecycle = (GenLifecycle)values[6];
  t->length = LENGTH_VALUES[t->length_class];
  t->offset = OFFSET_VALUES[t->offset_class];
  t->shift = ALIGN_SHIFTS[t->align_class];
  return 0;
}

/*
 * A duplicate id means two cells of the model produced byte-identical cases,
 * which is a redundancy in the model rather than a problem with the file --
 * exactly what section 8 of the coverage document wants surfaced rather than
 * absorbed. Linear scan: n is a few thousand and this runs once.
 */
static int remember_id(GenState *g, const char *id, const char *tuple,
                       char *err, size_t errsz) {
  size_t i;

  for (i = 0; i < g->seen_count; i++) {
    if (strcmp(g->seen[i].id, id) == 0) {
      snprintf(err, errsz, "duplicate case id %s: [%s] and [%s]", id,
               g->seen[i].tuple, tuple);
      return -1;
    }
  }
  if (g->seen_count == g->seen_capacity) {
    size_t   cap = g->seen_capacity ? g->seen_capacity * 2 : 1024;
    GenSeen *grown = (GenSeen *)realloc(g->seen, cap * sizeof(*grown));
    if (!grown) {
      snprintf(err, errsz, "out of memory");
      return -1;
    }
    g->seen = grown;
    g->seen_capacity = cap;
  }
  snprintf(g->seen[g->seen_count].id, sizeof(g->seen[0].id), "%s", id);
  snprintf(g->seen[g->seen_count].tuple, sizeof(g->seen[0].tuple), "%s", tuple);
  g->seen_count++;
  return 0;
}

/*
 * Encode, then decode and re-encode and compare byte for byte. The corpus is
 * only usable as identity if the encoding is canonical, and a generator is
 * exactly the place where a non-canonical encoding would be produced in bulk
 * and noticed late.
 */
static int encode_checked(const AbiCase *c, uint8_t **out, size_t *out_size,
                          char *err, size_t errsz) {
  uint8_t  *bytes = NULL, *again = NULL;
  size_t    n = 0, n_again = 0;
  AbiCase  *round = NULL;
  AbiError  e;
  AbiStatus st;

  memset(&e, 0, sizeof(e));
  st = abi_case_encode(c, &bytes, &n);
  if (st != ABI_OK) {
    snprintf(err, errsz, "encode: %s", abi_status_str(st));
    return -1;
  }
  st = abi_case_decode(bytes, n, &round, &e);
  if (st != ABI_OK) {
    snprintf(err, errsz, "decode: %s: %s", abi_status_str(st), e.message);
    abi_free(bytes);
    return -1;
  }
  st = abi_case_encode(round, &again, &n_again);
  abi_case_free(round);
  if (st != ABI_OK) {
    snprintf(err, errsz, "re-encode: %s", abi_status_str(st));
    abi_free(bytes);
    return -1;
  }
  if (n != n_again || memcmp(bytes, again, n) != 0) {
    snprintf(err, errsz, "re-encode differs (%lu -> %lu bytes)",
             (unsigned long)n, (unsigned long)n_again);
    abi_free(again);
    abi_free(bytes);
    return -1;
  }
  abi_free(again);
  *out = bytes;
  *out_size = n;
  return 0;
}

static int write_bytes(const char *path, const uint8_t *data, size_t size) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  if (fwrite(data, 1, size, f) != size) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
}

static int emit_case(GenState *g, const GenTuple *t, const char *out_dir,
                     FILE *manifest, char *err, size_t errsz) {
  AbiCase *c;
  uint8_t *bytes = NULL;
  size_t   size = 0;
  char     id[ABICASE_ID_HEX_SIZE];
  char     tuple[128];
  char     path[512];

  tuple_text(t, tuple, sizeof(tuple));
  c = build_case(t);
  if (!c) {
    snprintf(err, errsz, "build failed");
    return -1;
  }
  if (encode_checked(c, &bytes, &size, err, errsz) != 0) {
    abi_case_free(c);
    return -1;
  }
  abi_case_free(c);

  if (abi_case_id_of_bytes(bytes, size, id) != ABI_OK) {
    snprintf(err, errsz, "case id");
    abi_free(bytes);
    return -1;
  }
  if (remember_id(g, id, tuple, err, errsz) != 0) {
    abi_free(bytes);
    return -1;
  }
  snprintf(path, sizeof(path), "%s/%s.abicase", out_dir, id);
  if (write_bytes(path, bytes, size) != 0) {
    snprintf(err, errsz, "cannot write %s", path);
    abi_free(bytes);
    return -1;
  }
  abi_free(bytes);

  fprintf(manifest, "%s\t%lu\t%s\n", id, (unsigned long)size, tuple);
  g->generated++;
  g->total_bytes += size;
  if (size > g->max_bytes) g->max_bytes = size;
  return 0;
}

static int usage(void) {
  fprintf(stderr,
          "usage: abicase-gen --out DIR [TUPLES]\n"
          "\n"
          "  Reads accepted model tuples, one per line, tab separated, as\n"
          "  printed by `python tools/coverage_matrix.py --list`. Reads stdin\n"
          "  when TUPLES is omitted. Writes DIR/<case_id>.abicase and\n"
          "  DIR/manifest.tsv.\n");
  return 2;
}

int main(int argc, char **argv) {
  const char   *out_dir = NULL;
  const char   *tuples_path = NULL;
  FILE         *in, *manifest;
  GenState      g;
  GenTuple      t;
  char          line[256];
  char          err[GEN_ERR_SIZE];
  char          manifest_path[512];
  unsigned long lineno = 0;
  int           i, rc = 0;
  size_t        k;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out_dir = argv[++i];
    } else if (argv[i][0] == '-') {
      return usage();
    } else if (!tuples_path) {
      tuples_path = argv[i];
    } else {
      return usage();
    }
  }
  if (!out_dir) return usage();

  memset(&g, 0, sizeof(g));
  in = tuples_path ? fopen(tuples_path, "rb") : stdin;
  if (!in) {
    fprintf(stderr, "error: cannot read %s\n", tuples_path);
    return 1;
  }
  snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.tsv", out_dir);
  /* Binary mode: the repository is LF everywhere, including on Windows. */
  manifest = fopen(manifest_path, "wb");
  if (!manifest) {
    fprintf(stderr, "error: cannot write %s\n", manifest_path);
    if (in != stdin) fclose(in);
    return 1;
  }
  fprintf(manifest, "# case_id\tbytes\ttype\tlength\toffset\tnulls\tbuffers"
                    "\talignment\tlifecycle\n");

  while (fgets(line, sizeof(line), in)) {
    size_t len = strlen(line);
    lineno++;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len == 0 || line[0] == '#') continue;

    if (parse_tuple(line, &t, err, sizeof(err)) != 0) {
      fprintf(stderr, "error: line %lu: %s\n", lineno, err);
      rc = 1;
      break;
    }
    if (!lifecycle_supported(t.lifecycle)) {
      g.skipped[t.lifecycle]++;
      continue;
    }
    if (emit_case(&g, &t, out_dir, manifest, err, sizeof(err)) != 0) {
      fprintf(stderr, "error: line %lu [%s]: %s\n", lineno, line, err);
      rc = 1;
      break;
    }
  }

  if (in != stdin) fclose(in);
  if (fclose(manifest) != 0) {
    fprintf(stderr, "error: cannot finish %s\n", manifest_path);
    rc = 1;
  }
  free(g.seen);

  printf("generated %lu case(s) into %s\n", (unsigned long)g.generated,
         out_dir);
  if (g.generated) {
    printf("bytes     total %lu, max %lu, mean %lu\n",
           (unsigned long)g.total_bytes, (unsigned long)g.max_bytes,
           (unsigned long)(g.total_bytes / g.generated));
  }
  for (k = 0; k < COUNT_OF(LIFECYCLE_NAMES); k++) {
    if (g.skipped[k]) {
      printf("skipped   %lu with lifecycle %s (not yet constructible)\n",
             (unsigned long)g.skipped[k], LIFECYCLE_NAMES[k]);
    }
  }
  return rc;
}
