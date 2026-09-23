/*
 * The null consumer: the smallest conforming one there is.
 *
 * It takes what it is handed and releases it when told to, which is exactly
 * what the C Data Interface asks a consumer to do and nothing more. That makes
 * it the control voice of a differential run -- a case that the null consumer
 * cannot survive is a defect on our side of the boundary, because there is no
 * consumer logic left for it to be in. It also needs no third-party library, so
 * it is the one worker available on every host the coordinator runs on.
 *
 * It runs each case's CALLSEQ (abi/callseq.h) rather than a fixed path, so a
 * `moved` case really is moved before it is handed over, and the lifecycle
 * state machine judges the run strictly: every move is the executor's own, so
 * every release has an address the log knows. The class C ops are performed
 * too, as the misbehaving consumer they describe; they are safe here, and the
 * observer is what they test.
 *
 * It is not a consumer of *data*. It reads no buffer and compares no value; a
 * silent divergence is invisible to it by construction. It does report the
 * digest of what it was handed (`sent`), which is the harness's half of every
 * comparison a data-reading consumer makes -- and no `received`, because it
 * hands nothing back.
 */
#include <stdio.h>
#include <string.h>

#include "abi/callseq.h"
#include "worker.h"

typedef struct {
  struct ArrowSchema *schema;
  struct ArrowArray  *array;
  /* A stream, and what it has handed over, in storage of the consumer's. */
  struct ArrowArrayStream *stream;
  struct ArrowSchema       stream_schema;
  struct ArrowArray        stream_batch;
#if defined(ABI_ENABLE_USE_AFTER_RELEASE)
  const void *first_buffer; /* kept past its lifetime, on purpose */
#endif
} NullConsumer;

#if defined(ABI_ENABLE_USE_AFTER_RELEASE)
/*
 * The first buffer that exists anywhere in the tree. The root of a record
 * batch is a struct whose validity is usually omitted, so its own buffers[0]
 * is NULL -- and a "use after release" through NULL would read nothing and
 * report a misuse that never touched freed memory.
 */
static const void *first_buffer(const struct ArrowArray *a) {
  int64_t i;
  for (i = 0; i < a->n_buffers; i++)
    if (a->buffers[i]) return a->buffers[i];
  for (i = 0; i < a->n_children; i++) {
    const void *p = a->children[i] ? first_buffer(a->children[i]) : NULL;
    if (p) return p;
  }
  return NULL;
}
#endif

static int null_import_schema(void *ctx, struct ArrowSchema *s, char *why,
                              size_t n) {
  (void)why;
  (void)n;
  ((NullConsumer *)ctx)->schema = s;
  return 0;
}

static int null_import_array(void *ctx, struct ArrowArray *a, char *why,
                             size_t n) {
  NullConsumer *nc = (NullConsumer *)ctx;
  (void)why;
  (void)n;
  nc->array = a;
#if defined(ABI_ENABLE_USE_AFTER_RELEASE)
  nc->first_buffer = first_buffer(a);
#endif
  return 0;
}

/*
 * Parent-first and from outside the producer's own callbacks, so the observer
 * sees the depth-0 entry that distinguishes a consumer release from a nested
 * one.
 */
static void null_release_base(void *ctx) {
  NullConsumer *nc = (NullConsumer *)ctx;
  if (nc->array && nc->array->release) nc->array->release(nc->array);
  if (nc->schema && nc->schema->release) nc->schema->release(nc->schema);
  if (nc->stream_batch.release) nc->stream_batch.release(&nc->stream_batch);
  if (nc->stream_schema.release) nc->stream_schema.release(&nc->stream_schema);
  if (nc->stream && nc->stream->release) nc->stream->release(nc->stream);
}

/* --- the C Stream Interface ------------------------------------------------
 */

static int null_import_stream(void *ctx, struct ArrowArrayStream *s, char *why,
                              size_t n) {
  (void)why;
  (void)n;
  ((NullConsumer *)ctx)->stream = s;
  return 0;
}

static int null_stream_get_schema(void *ctx, char *why, size_t n) {
  NullConsumer *nc = (NullConsumer *)ctx;
  int           rc;
  /* A second call replaces the schema this consumer held. */
  if (nc->stream_schema.release) nc->stream_schema.release(&nc->stream_schema);
  rc = nc->stream->get_schema(nc->stream, &nc->stream_schema);
  if (rc) snprintf(why, n, "get_schema returned %d", rc);
  return rc;
}

static int null_stream_get_next(void *ctx, int *eof, char *why, size_t n) {
  NullConsumer *nc = (NullConsumer *)ctx;
  int           rc;
  /* Done with the previous batch, it lets it go before asking for the next. */
  if (nc->stream_batch.release) nc->stream_batch.release(&nc->stream_batch);
  rc = nc->stream->get_next(nc->stream, &nc->stream_batch);
  if (rc) {
    snprintf(why, n, "get_next returned %d", rc);
    return rc;
  }
  *eof = nc->stream_batch.release == NULL;
  return 0;
}

static int null_stream_get_last_error(void *ctx, char *why, size_t n) {
  NullConsumer *nc = (NullConsumer *)ctx;
  const char   *msg = nc->stream->get_last_error(nc->stream);
  snprintf(why, n, "get_last_error: %s", msg ? msg : "(none)");
  return 0;
}

static void null_release_child(void *ctx, uint32_t i) {
  struct ArrowArray *child = ((NullConsumer *)ctx)->array->children[i];
  if (child && child->release) child->release(child);
}

static void null_release_dictionary(void *ctx) {
  struct ArrowArray *dict = ((NullConsumer *)ctx)->array->dictionary;
  if (dict->release) dict->release(dict);
}

#if defined(ABI_ENABLE_USE_AFTER_RELEASE)
/* A read through a pointer whose owner has been released. Undefined behaviour
   by definition; a sanitizer build aborts here, which is the point. */
static void null_use_after_release(void *ctx) {
  const volatile unsigned char *p =
      (const volatile unsigned char *)((NullConsumer *)ctx)->first_buffer;
  if (p) (void)*p;
}
#endif

int main(int argc, char **argv) {
  AbiWorkerArgs args;
  AbiCaseList   list;
  AbiWorker     w;
  size_t        i;

  if (abi_worker_parse_args(argc, argv, &args) != 0) return 1;
  if (abi_worker_read_cases(args.cases, &list) != 0) return 1;
  if (abi_worker_open(&args, &w) != 0) {
    abi_case_list_free(&list);
    return 1;
  }

  abi_worker_header(&w, "null", NULL);
  for (i = 0; i < list.count; i++) {
    NullConsumer    nc;
    AbiConsumer     consumer;
    AbiWorkerDigest dg;

    memset(&nc, 0, sizeof(nc));
    memset(&consumer, 0, sizeof(consumer));
    memset(&dg, 0, sizeof(dg));
    consumer.ctx = &nc;
    consumer.import_schema = null_import_schema;
    consumer.import_array = null_import_array;
    consumer.release_base = null_release_base;
    consumer.release_child = null_release_child;
    consumer.release_dictionary = null_release_dictionary;
    consumer.import_stream = null_import_stream;
    consumer.stream_get_schema = null_stream_get_schema;
    consumer.stream_get_next = null_stream_get_next;
    consumer.stream_get_last_error = null_stream_get_last_error;
#if defined(ABI_ENABLE_USE_AFTER_RELEASE)
    consumer.use_after_release = null_use_after_release;
#endif
    abi_worker_run_case(&w, list.paths[i], &consumer, &dg);
  }

  abi_worker_close(&w);
  abi_case_list_free(&list);
  return 0;
}
