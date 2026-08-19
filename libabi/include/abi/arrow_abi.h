/*
 * The Arrow C Data Interface and C Stream Interface structures, verbatim.
 *
 * Vendored rather than depended upon: these definitions are the ABI under test,
 * so taking them from an Arrow build would mean testing an implementation
 * against its own header. They are also frozen by the specification, which is
 * the point of the interface, so vendoring costs nothing in maintenance.
 *
 * The include guards are the ones the specification prescribes, so this header
 * composes with any other that defines the same structures -- whichever is
 * included first wins and the other is a no-op, which is exactly the intent.
 */
#ifndef ABI_ARROW_ABI_H
#define ABI_ARROW_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE           2
#define ARROW_FLAG_MAP_KEYS_SORTED    4

struct ArrowSchema {
  /* Array type description */
  const char         *format;
  const char         *name;
  const char         *metadata;
  int64_t             flags;
  int64_t             n_children;
  struct ArrowSchema **children;
  struct ArrowSchema  *dictionary;

  /* Release callback */
  void (*release)(struct ArrowSchema *);
  /* Opaque producer-specific data */
  void *private_data;
};

struct ArrowArray {
  /* Array data description */
  int64_t            length;
  int64_t            null_count;
  int64_t            offset;
  int64_t            n_buffers;
  int64_t            n_children;
  const void       **buffers;
  struct ArrowArray **children;
  struct ArrowArray  *dictionary;

  /* Release callback */
  void (*release)(struct ArrowArray *);
  /* Opaque producer-specific data */
  void *private_data;
};

#endif /* ARROW_C_DATA_INTERFACE */

#ifndef ARROW_C_STREAM_INTERFACE
#define ARROW_C_STREAM_INTERFACE

struct ArrowArrayStream {
  /* Callbacks providing stream functionality */
  int (*get_schema)(struct ArrowArrayStream *, struct ArrowSchema *out);
  int (*get_next)(struct ArrowArrayStream *, struct ArrowArray *out);
  const char *(*get_last_error)(struct ArrowArrayStream *);

  /* Release callback */
  void (*release)(struct ArrowArrayStream *);
  /* Opaque producer-specific data */
  void *private_data;
};

#endif /* ARROW_C_STREAM_INTERFACE */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ABI_ARROW_ABI_H */
