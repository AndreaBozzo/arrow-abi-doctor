/*
 * duckdb_arrow_scan() never releases the schemas it asks a stream for.
 *
 * Standalone: DuckDB's C API and nothing of this project. A stream of one
 * int32 column hands out a freshly allocated schema on every get_schema() call
 * and counts how many of them are released. The C Stream Interface makes the
 * caller of get_schema() the owner of what it returns ("The schema ... must be
 * released" -- the ArrowArrayStream documentation), and every get_schema() call
 * here is DuckDB's: the program itself never asks for one.
 *
 * Prints the two counts; exits 0 when they differ, which is the report
 * reproducing, and 1 when every schema was released.
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

static int schemas_made;
static int schemas_released;
static int batches_left = 1;

static void release_child(struct ArrowSchema *s) { s->release = NULL; }

static void release_schema(struct ArrowSchema *s) {
  free(s->children[0]);
  free(s->children);
  s->release = NULL;
  schemas_released++;
}

static int get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
  struct ArrowSchema *child = calloc(1, sizeof(*child));
  (void)st;
  child->format = "i";
  child->name = "x";
  child->flags = 2; /* nullable */
  child->release = release_child;
  memset(out, 0, sizeof(*out));
  out->format = "+s";
  out->name = "";
  out->n_children = 1;
  out->children = calloc(1, sizeof(*out->children));
  out->children[0] = child;
  out->release = release_schema;
  schemas_made++;
  return 0;
}

static const int32_t values[3] = {1, 2, 3};

static void release_array(struct ArrowArray *a) {
  free(a->children[0]->buffers);
  free(a->children[0]);
  free(a->children);
  free(a->buffers);
  a->release = NULL;
}

static void release_child_array(struct ArrowArray *a) { a->release = NULL; }

static int get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
  struct ArrowArray *child;
  (void)st;
  memset(out, 0, sizeof(*out));
  if (batches_left-- <= 0) return 0; /* end of stream: release left NULL */
  child = calloc(1, sizeof(*child));
  child->length = 3;
  child->n_buffers = 2;
  child->buffers = calloc(2, sizeof(void *));
  child->buffers[1] = values;
  child->release = release_child_array;
  out->length = 3;
  out->n_buffers = 1;
  out->buffers = calloc(1, sizeof(void *));
  out->n_children = 1;
  out->children = calloc(1, sizeof(*out->children));
  out->children[0] = child;
  out->release = release_array;
  return 0;
}

static const char *get_last_error(struct ArrowArrayStream *st) {
  (void)st;
  return NULL;
}

static void release_stream(struct ArrowArrayStream *st) { st->release = NULL; }

int main(void) {
  duckdb_database         db;
  duckdb_connection       con;
  duckdb_result           res;
  struct ArrowArrayStream stream;

  memset(&stream, 0, sizeof(stream));
  stream.get_schema = get_schema;
  stream.get_next = get_next;
  stream.get_last_error = get_last_error;
  stream.release = release_stream;

  if (duckdb_open(NULL, &db) != DuckDBSuccess ||
      duckdb_connect(db, &con) != DuckDBSuccess) {
    fprintf(stderr, "cannot open DuckDB\n");
    return 2;
  }
  if (duckdb_arrow_scan(con, "t", (duckdb_arrow_stream)&stream) !=
      DuckDBSuccess) {
    fprintf(stderr, "duckdb_arrow_scan failed\n");
    return 2;
  }
  if (duckdb_query(con, "SELECT sum(x) FROM t", &res) != DuckDBSuccess) {
    fprintf(stderr, "query: %s\n", duckdb_result_error(&res));
    return 2;
  }
  printf("sum(x) = %lld\n", (long long)duckdb_value_int64(&res, 0, 0));
  duckdb_destroy_result(&res);
  duckdb_query(con, "DROP VIEW t", &res);
  duckdb_destroy_result(&res);
  stream.release(&stream);
  duckdb_disconnect(&con);
  duckdb_close(&db);

  printf("DuckDB %s: get_schema() called %d time(s), %d schema(s) released\n",
         duckdb_library_version(), schemas_made, schemas_released);
  return schemas_made != schemas_released ? 0 : 1;
}
