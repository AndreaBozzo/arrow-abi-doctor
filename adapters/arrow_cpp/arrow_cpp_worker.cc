/*
 * Arrow C++ behind the worker protocol, built from a pinned source release
 * with the sanitizers on.
 *
 * pyarrow already runs Arrow C++'s import under the coordinator
 * (adapters/dataprof/abi_worker.py), but from a wheel nobody instrumented: a
 * crash there is observed, an out-of-bounds read that does not crash is not.
 * This is the same import path, through the same functions, built so that it
 * is -- which makes the two directly comparable, and a difference between
 * them a statement about instrumentation rather than about the path.
 *
 *   - a schema and an array: arrow::ImportSchema(), then
 *     arrow::ImportRecordBatch() against it. ImportSchema releases the C
 *     schema whether or not it succeeds; the array is moved into an object
 *     the record batch keeps alive.
 *   - a stream: arrow::ImportRecordBatchReader(), which moves the stream into
 *     the reader and asks it for its schema at once, as pyarrow's
 *     RecordBatchReader.from_stream() does. STREAM_GET_SCHEMA reads the
 *     schema the reader already has; STREAM_GET_NEXT is ReadNext().
 *
 * Arrow moves what it takes into storage of its own, so its releases are
 * judged loosely, as pyarrow's are. It hands the data back: every batch is
 * re-exported with arrow::ExportRecordBatch(), and that is the `received`
 * digest.
 */
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <arrow/config.h>

#include <cstdio>
#include <cstring>
#include <memory>

#include "abi/callseq.h"
#include "worker.h"

namespace {

struct ArrowCpp {
  AbiWorkerDigest                          *digest = nullptr;
  std::shared_ptr<arrow::Schema>            schema;
  std::shared_ptr<arrow::RecordBatch>       batch;
  std::shared_ptr<arrow::RecordBatchReader> reader;
  int                                       batches = 0;
};

int refuse(const arrow::Status &st, const char *what, char *why, size_t n) {
  std::snprintf(why, n, "%s: %s", what, st.ToString().c_str());
  return 1;
}

/* What Arrow hands back for `batch`, exported and digested. */
void digest_batch(ArrowCpp *c, const arrow::RecordBatch &batch) {
  struct ArrowSchema s;
  struct ArrowArray  a;
  std::memset(&s, 0, sizeof(s));
  std::memset(&a, 0, sizeof(a));
  arrow::Status st = arrow::ExportRecordBatch(batch, &a, &s);
  if (!st.ok()) {
    std::snprintf(c->digest->received_error, sizeof(c->digest->received_error),
                  "ExportRecordBatch: %s", st.ToString().c_str());
    return;
  }
  abi_worker_digest_received(c->digest, &s, &a);
  a.release(&a);
  s.release(&s);
}

int import_schema(void *ctx, struct ArrowSchema *s, char *why, size_t n) {
  auto *c = static_cast<ArrowCpp *>(ctx);
  auto  r = arrow::ImportSchema(s);
  if (!r.ok()) return refuse(r.status(), "ImportSchema", why, n);
  c->schema = *r;
  return 0;
}

int import_array(void *ctx, struct ArrowArray *a, char *why, size_t n) {
  auto *c = static_cast<ArrowCpp *>(ctx);
  if (!c->schema) {
    std::snprintf(why, n, "no schema was imported before the array");
    return 1;
  }
  auto r = arrow::ImportRecordBatch(a, c->schema);
  if (!r.ok()) return refuse(r.status(), "ImportRecordBatch", why, n);
  c->batch = *r;
  digest_batch(c, *c->batch);
  return 0;
}

int import_stream(void *ctx, struct ArrowArrayStream *s, char *why, size_t n) {
  auto *c = static_cast<ArrowCpp *>(ctx);
  auto  r = arrow::ImportRecordBatchReader(s);
  if (!r.ok()) return refuse(r.status(), "ImportRecordBatchReader", why, n);
  c->reader = *r;
  return 0;
}

int stream_get_schema(void *ctx, char *why, size_t n) {
  auto *c = static_cast<ArrowCpp *>(ctx);
  if (!c->reader->schema()) {
    std::snprintf(why, n, "the reader has no schema");
    return 1;
  }
  return 0;
}

int stream_get_next(void *ctx, int *eof, char *why, size_t n) {
  auto *c = static_cast<ArrowCpp *>(ctx);
  /* Done with the previous batch, it lets it go before asking for the next. */
  c->batch.reset();
  arrow::Status st = c->reader->ReadNext(&c->batch);
  if (!st.ok()) return refuse(st, "ReadNext", why, n);
  *eof = c->batch == nullptr;
  if (!c->batch) return 0;
  /* One batch is all the model's streams hold; a second is not digested. */
  if (c->batches++ == 0) {
    digest_batch(c, *c->batch);
  } else {
    c->digest->have_received = 0;
    std::snprintf(c->digest->received_error, sizeof(c->digest->received_error),
                  "the stream delivered more than one batch");
  }
  return 0;
}

/* Dropping the last reference releases what Arrow imported. */
void release_base(void *ctx) {
  auto *c = static_cast<ArrowCpp *>(ctx);
  c->batch.reset();
  c->reader.reset();
  c->schema.reset();
}

} // namespace

int main(int argc, char **argv) {
  AbiWorkerArgs args;
  AbiCaseList   list;
  AbiWorker     w;

  if (abi_worker_parse_args(argc, argv, &args) != 0) return 1;
  if (abi_worker_read_cases(args.cases, &list) != 0) return 1;
  if (abi_worker_open(&args, &w) != 0) {
    abi_case_list_free(&list);
    return 1;
  }
  w.consumer_moves = 1;
  w.hands_back = 1;

  abi_worker_header(&w, "arrow-cpp",
                    arrow::GetBuildInfo().version_string.c_str());
  for (size_t i = 0; i < list.count; i++) {
    ArrowCpp        c;
    AbiConsumer     consumer;
    AbiWorkerDigest dg;

    std::memset(&consumer, 0, sizeof(consumer));
    std::memset(&dg, 0, sizeof(dg));
    c.digest = &dg;
    consumer.ctx = &c;
    consumer.import_schema = import_schema;
    consumer.import_array = import_array;
    consumer.release_base = release_base;
    consumer.import_stream = import_stream;
    consumer.stream_get_schema = stream_get_schema;
    consumer.stream_get_next = stream_get_next;
    abi_worker_run_case(&w, list.paths[i], &consumer, &dg);
  }

  abi_worker_close(&w);
  abi_case_list_free(&list);
  return 0;
}
