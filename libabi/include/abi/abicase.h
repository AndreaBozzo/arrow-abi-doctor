/*
 * libabi -- the .abicase container format.
 *
 * A .abicase file is a portable, canonical description of an Arrow C Data /
 * C Stream Interface test case. It is not a memory image: Arrow C structures
 * contain raw pointers and their metadata encoding is native-endian, so no
 * portable image of one exists. See docs/abicase-format.md, which is normative
 * for everything in this header.
 *
 * Ownership model: a case owns an arena. Every string, array and node reachable
 * from an AbiCase lives in that arena, and abi_case_free() releases all of it in
 * one call. Nothing reachable from a case is individually freeable, and no
 * pointer handed to a builder function is retained -- builders copy.
 */
#ifndef ABI_ABICASE_H
#define ABI_ABICASE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ABI_DOCTOR_VERSION "0.1.0-m0"

/* --- format constants (docs/abicase-format.md 2.1) ----------------------- */

#define ABICASE_SCHEMA_VERSION 1u
#define ABICASE_HEADER_SIZE    40u
#define ABICASE_SECTION_HEADER_SIZE 8u
#define ABICASE_MAGIC0 0x41u /* 'A' */
#define ABICASE_MAGIC1 0x42u /* 'B' */
#define ABICASE_MAGIC2 0x49u /* 'I' */
#define ABICASE_MAGIC3 0x43u /* 'C' */
#define ABICASE_BOM    0x0000FEFFu
#define ABICASE_ID_BYTES 16u
#define ABICASE_ID_HEX_SIZE 33u /* 32 hex digits + NUL */

/* --- decoder limits (docs/abicase-format.md 11) -------------------------- */

#define ABI_LIMIT_TOTAL_SIZE  (64u * 1024u * 1024u)
#define ABI_LIMIT_ALLOC_COUNT 4096u
#define ABI_LIMIT_ALLOC_BYTES (16u * 1024u * 1024u)
#define ABI_LIMIT_ALIGNMENT   4096u
#define ABI_LIMIT_TREE_DEPTH  64u
#define ABI_LIMIT_TREE_NODES  4096u
#define ABI_LIMIT_BYTES_LEN   (1u * 1024u * 1024u)
#define ABI_LIMIT_OP_COUNT    4096u
#define ABI_LIMIT_PATTERN_PERIOD 256u

/* --- status -------------------------------------------------------------- */

typedef enum {
  ABI_OK = 0,
  ABI_ERR_INVALID_ARGUMENT,
  ABI_ERR_NO_MEMORY,
  ABI_ERR_TRUNCATED,
  ABI_ERR_BAD_MAGIC,
  ABI_ERR_BAD_VERSION,
  ABI_ERR_BAD_BYTE_ORDER,
  ABI_ERR_NOT_CANONICAL,
  ABI_ERR_LIMIT_EXCEEDED,
  ABI_ERR_BAD_REFERENCE,
  ABI_ERR_BAD_ENUM,
  ABI_ERR_TRAILING_BYTES,
  ABI_ERR_DIGEST_MISMATCH,
  ABI_ERR_MISSING_SECTION,
  ABI_ERR_CLASS_RULE,
  ABI_ERR_IO
} AbiStatus;

const char *abi_status_str(AbiStatus status);

/*
 * Decode failures carry the byte offset at which the problem was found. A
 * format tool whose only output is "invalid" is not usable on a corpus of
 * deliberately-odd files.
 */
typedef struct {
  AbiStatus status;
  size_t    offset;
  char      message[192];
} AbiError;

/* --- enums (docs/abicase-format.md 2.2, 5.2, 7.4, 8, 9) ------------------ */

typedef enum {
  ABI_CLASS_A  = 0,
  ABI_CLASS_B1 = 1,
  ABI_CLASS_B2 = 2,
  ABI_CLASS_C  = 3
} AbiClass;

typedef enum {
  ABI_FILL_RAW     = 0,
  ABI_FILL_ZERO    = 1,
  ABI_FILL_RLE     = 2,
  ABI_FILL_PATTERN = 3
} AbiFill;

typedef enum {
  ABI_ROLE_UNSPECIFIED   = 0,
  ABI_ROLE_VALIDITY      = 1,
  ABI_ROLE_DATA          = 2,
  ABI_ROLE_OFFSETS       = 3,
  ABI_ROLE_TYPE_IDS      = 4,
  ABI_ROLE_UNION_OFFSETS = 5,
  ABI_ROLE_SIZES         = 6,
  ABI_ROLE_VIEWS         = 7,
  ABI_ROLE_VARIADIC_DATA = 8,
  ABI_ROLE__MAX          = 8
} AbiBufferRole;

typedef enum {
  ABI_OP_NOP                  = 0,
  ABI_OP_IMPORT_SCHEMA        = 1,
  ABI_OP_IMPORT_ARRAY         = 2,
  ABI_OP_STREAM_GET_SCHEMA    = 3,
  ABI_OP_STREAM_GET_NEXT      = 4,
  ABI_OP_STREAM_GET_LAST_ERROR = 5,
  ABI_OP_RELEASE_BASE         = 6,
  ABI_OP_RELEASE_CHILD        = 7,  /* class C only: consumer misuse */
  ABI_OP_RELEASE_DICTIONARY   = 8,  /* class C only: consumer misuse */
  ABI_OP_MOVE_STRUCT          = 9,
  ABI_OP_USE_AFTER_RELEASE    = 10, /* class C only: consumer misuse */
  ABI_OP_EXPECT_EOF           = 11,
  ABI_OP__MAX                 = 11
} AbiOpCode;

typedef enum {
  ABI_VALIDATE_NONE    = 0,
  ABI_VALIDATE_MINIMAL = 1,
  ABI_VALIDATE_DEFAULT = 2,
  ABI_VALIDATE_FULL    = 3
} AbiValidationLevel;

typedef enum {
  ABI_EXPECT_ACCEPT      = 0,
  ABI_EXPECT_REJECT      = 1,
  ABI_EXPECT_EITHER      = 2,
  ABI_EXPECT_UNSPECIFIED = 3
} AbiExpectedOutcome;

/* --- data model ---------------------------------------------------------- */

/*
 * Length-carrying byte span. Used for every string in the model, because Arrow
 * distinguishes an absent name from an empty one and permits embedded NUL in
 * metadata values; a plain char* could represent neither.
 */
typedef struct {
  const uint8_t *data;
  uint32_t       size;
} AbiBytes;

typedef struct {
  uint64_t       size_bytes;
  uint32_t       alignment;
  const uint8_t *bytes; /* size_bytes long; NULL iff size_bytes == 0 */
} AbiAllocation;

typedef struct {
  uint8_t  role;
  uint8_t  present; /* 0 => the reconstructed pointer is NULL */
  uint32_t allocation_id;
  uint64_t byte_offset;
  uint64_t logical_length;
} AbiBufferView;

typedef struct {
  AbiBytes key;
  AbiBytes value;
} AbiMetadataKV;

typedef struct AbiSchemaNode {
  AbiBytes format;
  int      has_name;
  AbiBytes name;
  int      has_metadata;
  AbiMetadataKV *metadata;
  uint32_t       metadata_count;
  int64_t  flags;
  uint32_t n_children;  /* written into ArrowSchema.n_children */
  uint32_t child_count; /* nodes actually present; may differ for B1/C */
  struct AbiSchemaNode **children;
  struct AbiSchemaNode  *dictionary;
  uint32_t child_capacity;    /* builder bookkeeping; never serialized */
  uint32_t metadata_capacity; /* builder bookkeeping; never serialized */
} AbiSchemaNode;

typedef struct AbiArrayNode {
  int64_t  length;
  int64_t  null_count; /* -1 == not computed */
  int64_t  offset;
  uint32_t n_buffers;    /* written into ArrowArray.n_buffers */
  uint32_t buffer_count; /* views actually present; may differ for B1/C */
  AbiBufferView *buffers;
  uint32_t n_children;
  uint32_t child_count;
  struct AbiArrayNode **children;
  struct AbiArrayNode  *dictionary;
  uint32_t buffer_capacity; /* builder bookkeeping; never serialized */
  uint32_t child_capacity;  /* builder bookkeeping; never serialized */
} AbiArrayNode;

typedef struct {
  uint16_t code;
  uint32_t arg0;
  uint32_t arg1;
} AbiOp;

typedef struct {
  uint64_t seed;
  AbiBytes rng_algorithm;
  uint32_t rng_version;
  AbiBytes generator_version;
  AbiBytes abi_doctor_version;
  AbiBytes spec_revision;
} AbiProvenance;

typedef struct {
  uint8_t  validation_level;
  uint8_t  expected_outcome;
  AbiBytes spec_clause;
  AbiBytes notes;
} AbiExpected;

typedef struct AbiArena AbiArena;

typedef struct {
  AbiClass       cls;
  AbiProvenance  provenance;
  AbiAllocation *allocations;
  uint32_t       alloc_count;
  AbiSchemaNode *schema; /* required */
  AbiArrayNode  *array;  /* NULL => no ARRAY section */
  AbiOp         *ops;
  uint32_t       op_count; /* 0 => no CALLSEQ section */
  AbiExpected    expected;

  AbiArena *arena; /* owns everything above */
  uint32_t  alloc_capacity;
  uint32_t  op_capacity;
} AbiCase;

/* --- lifecycle ----------------------------------------------------------- */

AbiCase *abi_case_new(AbiClass cls);
void     abi_case_free(AbiCase *c);

/* Frees a buffer returned by abi_case_encode / abi_case_read_file. */
void abi_free(void *p);

/* --- building ------------------------------------------------------------ */

/* NULL string arguments are treated as empty, except where noted. */
AbiStatus abi_case_set_provenance(AbiCase *c, uint64_t seed,
                                  const char *rng_algorithm, uint32_t rng_version,
                                  const char *generator_version,
                                  const char *abi_doctor_version,
                                  const char *spec_revision);

AbiStatus abi_case_set_expected(AbiCase *c, AbiValidationLevel level,
                                AbiExpectedOutcome outcome,
                                const char *spec_clause, const char *notes);

/*
 * Copies `size` bytes into the case arena and returns the new allocation id.
 * `bytes` may be NULL iff size == 0. `alignment` must be a power of two in
 * [1, ABI_LIMIT_ALIGNMENT] and describes the allocation, not any view of it.
 */
AbiStatus abi_case_add_allocation(AbiCase *c, const void *bytes, uint64_t size,
                                  uint32_t alignment, uint32_t *out_id);

AbiSchemaNode *abi_schema_new(AbiCase *c, const char *format);
AbiStatus abi_schema_set_format(AbiCase *c, AbiSchemaNode *n, const void *format,
                                uint32_t len);
/* name == NULL clears the name (ArrowSchema.name == NULL), which differs from "". */
AbiStatus abi_schema_set_name(AbiCase *c, AbiSchemaNode *n, const char *name);
AbiStatus abi_schema_add_metadata(AbiCase *c, AbiSchemaNode *n, const void *key,
                                  uint32_t key_len, const void *value,
                                  uint32_t value_len);
/* Marks metadata present with zero pairs -- distinct from absent metadata. */
AbiStatus abi_schema_set_metadata_present(AbiCase *c, AbiSchemaNode *n);
AbiStatus abi_schema_add_child(AbiCase *c, AbiSchemaNode *parent,
                               AbiSchemaNode *child);
void      abi_schema_set_dictionary(AbiSchemaNode *n, AbiSchemaNode *dict);
/*
 * Breaks child_count == n_children on purpose. Class B1/C only.
 *
 * Call this AFTER every abi_schema_add_child() for the node: each add sets the
 * declared count to the provided count, so adding a child afterwards silently
 * repairs the inconsistency this exists to create -- turning a B1 case into a
 * well-formed one that tests nothing. Class A and B2 reject the mismatch at
 * encode time, so only B1 and C can lose it quietly.
 */
void      abi_schema_set_declared_children(AbiSchemaNode *n, uint32_t n_children);

AbiArrayNode *abi_array_new(AbiCase *c);
AbiStatus abi_array_add_buffer(AbiCase *c, AbiArrayNode *n, AbiBufferRole role,
                               uint32_t allocation_id, uint64_t byte_offset,
                               uint64_t logical_length);
/* A NULL buffer pointer: distinct from a view onto a zero-length allocation. */
AbiStatus abi_array_add_null_buffer(AbiCase *c, AbiArrayNode *n,
                                    AbiBufferRole role);
AbiStatus abi_array_add_child(AbiCase *c, AbiArrayNode *parent,
                              AbiArrayNode *child);
void      abi_array_set_dictionary(AbiArrayNode *n, AbiArrayNode *dict);
/*
 * Break the declared counts on purpose. Class B1/C only.
 * Call AFTER every add for the node -- see abi_schema_set_declared_children().
 */
void      abi_array_set_declared_buffers(AbiArrayNode *n, uint32_t n_buffers);
void      abi_array_set_declared_children(AbiArrayNode *n, uint32_t n_children);

void      abi_case_set_schema(AbiCase *c, AbiSchemaNode *schema);
void      abi_case_set_array(AbiCase *c, AbiArrayNode *array);
AbiStatus abi_case_add_op(AbiCase *c, AbiOpCode code, uint32_t arg0,
                          uint32_t arg1);

/* --- serialization ------------------------------------------------------- */

/*
 * Encodes to the canonical byte string. *out_data is malloc'd; free with
 * abi_free(). Encoding a case that violates the format's invariants fails
 * rather than emitting a file the decoder would reject.
 */
AbiStatus abi_case_encode(const AbiCase *c, uint8_t **out_data, size_t *out_size);

/*
 * Decodes and validates. Rejects any non-canonical input, so that
 * encode(decode(b)) == b holds byte for byte for every accepted b.
 * `err` may be NULL. On failure *out is left untouched.
 */
AbiStatus abi_case_decode(const uint8_t *data, size_t size, AbiCase **out,
                          AbiError *err);

/* Convenience I/O. */
AbiStatus abi_case_write_file(const AbiCase *c, const char *path);
AbiStatus abi_case_read_file(const char *path, AbiCase **out, AbiError *err);
AbiStatus abi_read_file(const char *path, uint8_t **out_data, size_t *out_size);

/* --- identity ------------------------------------------------------------ */

/*
 * Lowercase hex of the payload id: the canonical identity of the case. Two
 * cases with the same id are the same case, on any host. `out_hex` must be at
 * least ABICASE_ID_HEX_SIZE bytes.
 */
AbiStatus abi_case_id(const AbiCase *c, char *out_hex);
/* Same, read straight out of an encoded file without decoding it. */
AbiStatus abi_case_id_of_bytes(const uint8_t *data, size_t size, char *out_hex);

/* --- canonical allocation payload encoding (format 5.2) ------------------ */

/*
 * The canonical choice for `bytes`: smallest encoded payload, ties to the
 * lowest fill code. Encoder and decoder call this same function, which is what
 * lets the decoder reject a non-minimal encoding without trusting the writer.
 * `out_period` / `out_run_count` are set for PATTERN / RLE respectively.
 */
AbiFill abi_fill_choose(const uint8_t *bytes, uint64_t size,
                        uint64_t *out_payload_size, uint32_t *out_period,
                        uint32_t *out_run_count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ABI_ABICASE_H */
