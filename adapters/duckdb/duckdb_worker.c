/*
 * DuckDB behind the worker protocol: an independent C Data Interface consumer.
 *
 * DuckDB imports Arrow with its own code rather than through Arrow C++, which
 * is what makes it a second voice rather than a second reading of the same
 * implementation. It is built here from source at a pinned release (README.md
 * in this directory), so a sanitizer build instruments DuckDB itself and not
 * only the harness around it.
 *
 * A schema and an array go through duckdb_schema_from_arrow() and
 * duckdb_data_chunk_from_arrow(), the C API's Arrow import. The schema stays
 * the caller's and is released by it; the array becomes DuckDB's, moved into
 * a wrapper the data chunk owns and released when the chunk is destroyed.
 * DuckDB moves what it takes into storage of its own, so its releases are
 * judged loosely, as the pyarrow worker's are.
 *
 * Streams are not taken. The current C API has no stream import, and the
 * deprecated one, duckdb_arrow_scan(), never releases the schemas it asks a
 * stream for -- known upstream (duckdb/duckdb#16050, closed as deprecated) and
 * reproduced by findings/. Run through it, every stream cell would report that
 * one leak and nothing else could be seen there, so the stream cells are left
 * unrun and DuckDB makes no claim over them.
 *
 * It hands the data back: the chunk it built is exported with
 * duckdb_data_chunk_to_arrow(), and that is the `received` digest. A value
 * DuckDB lost or changed on the way in is a logical-digest disagreement, which
 * is the silent divergence a differential run exists to catch.
 */
#include <stdio.h>
#include <string.h>

#include "abi/callseq.h"
#include "duckdb.h"
#include "worker.h"

#define MAX_COLUMNS 64

typedef struct {
  duckdb_connection    con;
  duckdb_arrow_options opts;
  AbiWorkerDigest     *digest;

  struct ArrowSchema *schema; /* the caller's: DuckDB copies what it needs */
  duckdb_arrow_converted_schema converted;
  duckdb_data_chunk             chunk; /* owns the imported array */
} Duck;

/* Takes the message and destroys the error; 0 when there was none. */
static int failed(duckdb_error_data e, const char *what, char *why, size_t n) {
  if (!e) return 0;
  snprintf(why, n, "%s: %s", what, duckdb_error_data_message(e));
  duckdb_destroy_error_data(&e);
  return 1;
}

/*
 * What DuckDB hands back for its chunk, as a schema and an array, digested.
 * The chunk carries no names, so the handed-over schema's are used; the
 * logical digest does not read them.
 */
static void digest_chunk(Duck *d) {
  struct ArrowSchema  s;
  struct ArrowArray   a;
  duckdb_logical_type types[MAX_COLUMNS];
  const char         *names[MAX_COLUMNS];
  idx_t               n = duckdb_data_chunk_get_column_count(d->chunk), i;
  char               *why = d->digest->received_error;
  size_t              wn = sizeof(d->digest->received_error);

  memset(&s, 0, sizeof(s));
  memset(&a, 0, sizeof(a));
  if (n > MAX_COLUMNS) {
    snprintf(why, wn, "%lu columns, more than this worker digests",
             (unsigned long)n);
    return;
  }
  for (i = 0; i < n; i++) {
    const struct ArrowSchema *child =
        (int64_t)i < d->schema->n_children ? d->schema->children[i] : NULL;
    names[i] = child && child->name ? child->name : "";
    types[i] = duckdb_vector_get_column_type(
        duckdb_data_chunk_get_vector(d->chunk, i));
  }
  if (!failed(duckdb_to_arrow_schema(d->opts, types, names, n, &s),
              "duckdb_to_arrow_schema", why, wn) &&
      !failed(duckdb_data_chunk_to_arrow(d->opts, d->chunk, &a),
              "duckdb_data_chunk_to_arrow", why, wn)) {
    abi_worker_digest_received(d->digest, &s, &a);
  }
  for (i = 0; i < n; i++)
    duckdb_destroy_logical_type(&types[i]);
  if (a.release) a.release(&a);
  if (s.release) s.release(&s);
}

static int duck_import_schema(void *ctx, struct ArrowSchema *s, char *why,
                              size_t n) {
  Duck *d = (Duck *)ctx;
  /*
   * Kept only once converted: a refused schema goes back to the harness
   * (callseq.c), and releasing it here as well would make two owners.
   */
  if (failed(duckdb_schema_from_arrow(d->con, s, &d->converted),
             "duckdb_schema_from_arrow", why, n)) {
    return 1;
  }
  d->schema = s;
  return 0;
}

/*
 * DuckDB 1.5.5 moves the release callback into a wrapper of its own before it
 * converts the first column, so an array refused during conversion has
 * already been released by DuckDB and nothing comes back. Whatever it left
 * live is the harness's again (callseq.c); the log judges which happened.
 */
static int duck_import_array(void *ctx, struct ArrowArray *a, char *why,
                             size_t n) {
  Duck *d = (Duck *)ctx;

  if (!d->converted) {
    snprintf(why, n, "no schema was converted before the array");
    return 1;
  }
  if (failed(duckdb_data_chunk_from_arrow(d->con, a, d->converted, &d->chunk),
             "duckdb_data_chunk_from_arrow", why, n)) {
    return 1;
  }
  digest_chunk(d);
  return 0;
}

static void duck_release_base(void *ctx) {
  Duck *d = (Duck *)ctx;
  /* Destroying the chunk releases the array it took. */
  if (d->chunk) duckdb_destroy_data_chunk(&d->chunk);
  if (d->converted) duckdb_destroy_arrow_converted_schema(&d->converted);
  if (d->schema && d->schema->release) d->schema->release(d->schema);
}

int main(int argc, char **argv) {
  AbiWorkerArgs   args;
  AbiCaseList     list;
  AbiWorker       w;
  duckdb_config   config;
  duckdb_database db;
  char           *open_error = NULL;
  size_t          i;

  if (abi_worker_parse_args(argc, argv, &args) != 0) return 1;
  if (abi_worker_read_cases(args.cases, &list) != 0) return 1;

  /* One thread: nothing here is parallel, and a second thread would only make
     a sanitizer report harder to read. */
  if (duckdb_create_config(&config) != DuckDBSuccess ||
      duckdb_set_config(config, "threads", "1") != DuckDBSuccess ||
      duckdb_open_ext(NULL, &db, config, &open_error) != DuckDBSuccess) {
    fprintf(stderr, "error: cannot open DuckDB: %s\n",
            open_error ? open_error : "(no message)");
    abi_case_list_free(&list);
    return 1;
  }
  duckdb_destroy_config(&config);

  if (abi_worker_open(&args, &w) != 0) {
    duckdb_close(&db);
    abi_case_list_free(&list);
    return 1;
  }
  w.consumer_moves = 1;

  abi_worker_header(&w, "duckdb", duckdb_library_version());
  for (i = 0; i < list.count; i++) {
    Duck            d;
    AbiConsumer     consumer;
    AbiWorkerDigest dg;

    memset(&d, 0, sizeof(d));
    memset(&consumer, 0, sizeof(consumer));
    memset(&dg, 0, sizeof(dg));
    d.digest = &dg;
    /* A connection per case, so nothing one case sets outlives it. */
    if (duckdb_connect(db, &d.con) != DuckDBSuccess) {
      abi_worker_result(&w, list.paths[i], NULL, "error",
                        "cannot connect to DuckDB", NULL);
      continue;
    }
    duckdb_connection_get_arrow_options(d.con, &d.opts);
    consumer.ctx = &d;
    consumer.import_schema = duck_import_schema;
    consumer.import_array = duck_import_array;
    consumer.release_base = duck_release_base;
    abi_worker_run_case(&w, list.paths[i], &consumer, &dg);
    duckdb_destroy_arrow_options(&d.opts);
    duckdb_disconnect(&d.con);
  }

  abi_worker_close(&w);
  duckdb_close(&db);
  abi_case_list_free(&list);
  return 0;
}
