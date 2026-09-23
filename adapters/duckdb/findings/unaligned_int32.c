/*
 * DuckDB keeps an unaligned Arrow buffer as the data of its own vector.
 *
 * Standalone: DuckDB's C API and nothing of this project. One int32 column of
 * four values, whose values buffer starts one byte past a 4-byte boundary --
 * legal in the C Data Interface, which lets a consumer decline unaligned
 * memory provided it says so ("Consumers MAY decide not to support unaligned
 * memory"). DuckDB accepts it, and does not copy it: the vector it builds
 * points into the caller's buffer, at the same unaligned address. Every
 * typed read DuckDB then makes through that vector is a load of a misaligned
 * int32_t, which is undefined behaviour in C++; with UBSan's alignment check
 * on, duckdb_data_chunk_to_arrow() below is reported as exactly that
 * (README.md has the report).
 *
 * Prints what it found; exits 0 when DuckDB's vector data is the unaligned
 * caller buffer, which is the report reproducing, and 1 when it is not.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The C Data and C Stream Interface structures, verbatim from the Arrow
   specification, which asks for them to be copied: DuckDB's own header only
   declares them. */
#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE           2
#define ARROW_FLAG_MAP_KEYS_SORTED    4

struct ArrowSchema {
  /* Array type description */
  const char          *format;
  const char          *name;
  const char          *metadata;
  int64_t              flags;
  int64_t              n_children;
  struct ArrowSchema **children;
  struct ArrowSchema  *dictionary;

  /* Release callback */
  void (*release)(struct ArrowSchema *);
  /* Opaque producer-specific data */
  void *private_data;
};

struct ArrowArray {
  /* Array data description */
  int64_t             length;
  int64_t             null_count;
  int64_t             offset;
  int64_t             n_buffers;
  int64_t             n_children;
  const void        **buffers;
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

#include "duckdb.h"

static void release_child(struct ArrowSchema *s) { s->release = NULL; }

static void release_schema(struct ArrowSchema *s) {
  free(s->children[0]);
  free(s->children);
  s->release = NULL;
}

static void release_child_array(struct ArrowArray *a) { a->release = NULL; }

static void release_array(struct ArrowArray *a) {
  free(a->children[0]->buffers);
  free(a->children[0]);
  free(a->children);
  free(a->buffers);
  a->release = NULL;
}

int main(void) {
  /* Aligned storage, then a view one byte in: an unaligned int32 buffer. */
  static int32_t                storage[8];
  unsigned char                *values = (unsigned char *)storage + 1;
  int32_t                       v;
  int                           i;
  struct ArrowSchema            schema, *child_schema;
  struct ArrowArray             array, *child;
  duckdb_database               db;
  duckdb_connection             con;
  duckdb_arrow_converted_schema converted;
  duckdb_data_chunk             chunk;
  duckdb_arrow_options          opts;
  duckdb_error_data             err;
  struct ArrowArray             out;
  const void                   *data;

  for (i = 0; i < 4; i++) {
    v = 1000 + i;
    memcpy(values + i * 4, &v, sizeof(v));
  }

  child_schema = calloc(1, sizeof(*child_schema));
  child_schema->format = "i";
  child_schema->name = "x";
  child_schema->flags = ARROW_FLAG_NULLABLE;
  child_schema->release = release_child;
  memset(&schema, 0, sizeof(schema));
  schema.format = "+s";
  schema.name = "";
  schema.n_children = 1;
  schema.children = calloc(1, sizeof(*schema.children));
  schema.children[0] = child_schema;
  schema.release = release_schema;

  child = calloc(1, sizeof(*child));
  child->length = 4;
  child->n_buffers = 2;
  child->buffers = calloc(2, sizeof(void *));
  child->buffers[1] = values;
  child->release = release_child_array;
  memset(&array, 0, sizeof(array));
  array.length = 4;
  array.n_buffers = 1;
  array.buffers = calloc(1, sizeof(void *));
  array.n_children = 1;
  array.children = calloc(1, sizeof(*array.children));
  array.children[0] = child;
  array.release = release_array;

  if (duckdb_open(NULL, &db) != DuckDBSuccess ||
      duckdb_connect(db, &con) != DuckDBSuccess) {
    fprintf(stderr, "cannot open DuckDB\n");
    return 2;
  }
  err = duckdb_schema_from_arrow(con, &schema, &converted);
  if (err) {
    fprintf(stderr, "schema: %s\n", duckdb_error_data_message(err));
    return 2;
  }
  err = duckdb_data_chunk_from_arrow(con, &array, converted, &chunk);
  if (err) {
    fprintf(stderr, "array: %s\n", duckdb_error_data_message(err));
    return 2;
  }
  data = duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 0));

  /* DuckDB reading its own vector, as it does for any use of the data. */
  duckdb_connection_get_arrow_options(con, &opts);
  memset(&out, 0, sizeof(out));
  err = duckdb_data_chunk_to_arrow(opts, chunk, &out);
  if (err) {
    fprintf(stderr, "export: %s\n", duckdb_error_data_message(err));
    return 2;
  }
  memcpy(&v, (const unsigned char *)out.children[0]->buffers[1] + 12,
         sizeof(v));
  printf("DuckDB %s: buffer at %p (address %% 4 = %d), vector data at %p; "
         "last value read back %d\n",
         duckdb_library_version(), (void *)values, (int)((uintptr_t)values % 4),
         data, (int)v);

  out.release(&out);
  duckdb_destroy_arrow_options(&opts);
  duckdb_destroy_data_chunk(&chunk);
  duckdb_destroy_arrow_converted_schema(&converted);
  schema.release(&schema);
  duckdb_disconnect(&con);
  duckdb_close(&db);
  return data == (const void *)values && (uintptr_t)data % 4 != 0 ? 0 : 1;
}
