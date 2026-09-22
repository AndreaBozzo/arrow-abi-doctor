#include "worker.h"

#include <stdlib.h>
#include <string.h>

/*
 * Host facts for the header line. Recorded rather than inferred: a report that
 * says "no leaks" without saying whether a leak detector was linked in is
 * making a claim it did not measure.
 */
#if defined(__x86_64__) || defined(_M_X64)
#define ABI_WORKER_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ABI_WORKER_ARCH "aarch64"
#elif defined(__s390x__)
#define ABI_WORKER_ARCH "s390x"
#else
#define ABI_WORKER_ARCH "unknown"
#endif

#if defined(_WIN32)
#define ABI_WORKER_OS "windows"
#elif defined(__linux__)
#define ABI_WORKER_OS "linux"
#elif defined(__APPLE__)
#define ABI_WORKER_OS "macos"
#else
#define ABI_WORKER_OS "unknown"
#endif

#if defined(__clang__)
#define ABI_WORKER_CC "clang " __clang_version__
#elif defined(__GNUC__)
#define ABI_WORKER_CC "gcc " __VERSION__
#elif defined(_MSC_VER)
#define ABI_WORKER_CC "msvc"
#else
#define ABI_WORKER_CC "unknown"
#endif

/*
 * UBSan defines no predefined macro of its own, so the build system states what
 * it turned on rather than the source guessing. Absent the define, the honest
 * answer is the empty list.
 */
#ifndef ABI_WORKER_SANITIZERS
#define ABI_WORKER_SANITIZERS ""
#endif

static int usage(void) {
  fprintf(stderr, "usage: <worker> --cases <path> --results <path> "
                  "[--consumer <name>]\n"
                  "see docs/worker-protocol.md\n");
  return 1;
}

int abi_worker_parse_args(int argc, char **argv, AbiWorkerArgs *out) {
  int i;

  memset(out, 0, sizeof(*out));
  for (i = 1; i < argc; i++) {
    int last = (i + 1 >= argc);
    if (strcmp(argv[i], "--cases") == 0 && !last) {
      out->cases = argv[++i];
    } else if (strcmp(argv[i], "--results") == 0 && !last) {
      out->results = argv[++i];
    } else if (strcmp(argv[i], "--consumer") == 0 && !last) {
      out->consumer = argv[++i];
    } else {
      return usage();
    }
  }
  if (!out->cases || !out->results) return usage();
  return 0;
}

int abi_worker_read_cases(const char *path, AbiCaseList *out) {
  FILE  *f;
  char   line[4096];
  char **grown;
  size_t cap = 0;

  memset(out, 0, sizeof(*out));
  f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "error: cannot read case list %s\n", path);
    return 1;
  }
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    /*
     * A line longer than the buffer would come back split, and both halves
     * would then be treated as paths: one worker failure per fragment, each
     * reporting a `case` string the coordinator never assigned, while the real
     * case is counted not-run. Silently misattributing a result is worse than
     * refusing the list, so this refuses the list.
     */
    if (n > 0 && line[n - 1] != '\n' && !feof(f)) {
      fprintf(stderr,
              "error: %s line %lu is longer than %lu bytes; a truncated path "
              "would be reported as a case that was never assigned\n",
              path, (unsigned long)(out->count + 1),
              (unsigned long)sizeof(line) - 1);
      fclose(f);
      abi_case_list_free(out);
      return 1;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
    if (n == 0 || line[0] == '#') continue;
    if (out->count == cap) {
      cap = cap ? cap * 2 : 64;
      grown = (char **)realloc(out->paths, cap * sizeof(*grown));
      if (!grown) {
        fclose(f);
        abi_case_list_free(out);
        return 1;
      }
      out->paths = grown;
    }
    out->paths[out->count] = (char *)malloc(n + 1);
    if (!out->paths[out->count]) {
      fclose(f);
      abi_case_list_free(out);
      return 1;
    }
    memcpy(out->paths[out->count], line, n + 1);
    out->count++;
  }
  fclose(f);
  return 0;
}

void abi_case_list_free(AbiCaseList *l) {
  size_t i;
  for (i = 0; i < l->count; i++)
    free(l->paths[i]);
  free(l->paths);
  memset(l, 0, sizeof(*l));
}

int abi_worker_open(const AbiWorkerArgs *args, AbiWorker *w) {
  w->out = fopen(args->results, "wb");
  if (!w->out) {
    fprintf(stderr, "error: cannot write results to %s\n", args->results);
    return 1;
  }
  return 0;
}

void abi_worker_close(AbiWorker *w) {
  if (w->out) fclose(w->out);
  w->out = NULL;
}

/* JSON string, quoted and escaped. Control bytes go out as \u00XX. */
static void put_str(FILE *out, const char *s) {
  const unsigned char *p = (const unsigned char *)(s ? s : "");

  fputc('"', out);
  for (; *p; p++) {
    if (*p == '"' || *p == '\\') {
      fputc('\\', out);
      fputc((int)*p, out);
    } else if (*p == '\n') {
      fputs("\\n", out);
    } else if (*p < 0x20) {
      fprintf(out, "\\u%04x", (unsigned)*p);
    } else {
      fputc((int)*p, out);
    }
  }
  fputc('"', out);
}

/* `"key":` -- written this way so the format strings stay readable. */
static void put_key(FILE *out, const char *key, int first) {
  if (!first) fputc(',', out);
  put_str(out, key);
  fputc(':', out);
}

static void put_field(FILE *out, const char *key, const char *value,
                      int first) {
  put_key(out, key, first);
  put_str(out, value);
}

static void put_uint(FILE *out, const char *key, unsigned long long value,
                     int first) {
  put_key(out, key, first);
  fprintf(out, "%llu", value);
}

/*
 * A comma-separated build setting as a JSON array of one string per element.
 * The build hands these over as a single "address,undefined", and emitting that
 * verbatim would give `["address,undefined"]` -- one array element that every
 * reader then has to know to split again.
 */
static void put_str_list(FILE *out, const char *csv) {
  const char *start = csv;
  int         first = 1;

  fputc('[', out);
  while (*start) {
    const char *comma = strchr(start, ',');
    size_t      len = comma ? (size_t)(comma - start) : strlen(start);
    if (len > 0) {
      char item[64];
      if (len >= sizeof(item)) len = sizeof(item) - 1;
      memcpy(item, start, len);
      item[len] = 0;
      if (!first) fputc(',', out);
      put_str(out, item);
      first = 0;
    }
    if (!comma) break;
    start = comma + 1;
  }
  fputc(']', out);
}

static void put_bool(FILE *out, const char *key, int value, int first) {
  put_key(out, key, first);
  fputs(value ? "true" : "false", out);
}

void abi_worker_header(AbiWorker *w, const char *consumer,
                       const char *version) {
  FILE *out = w->out;

  fputc('{', out);
  put_uint(out, "worker_protocol", (unsigned long long)ABI_WORKER_PROTOCOL, 1);
  put_field(out, "consumer", consumer, 0);
  if (version) put_field(out, "consumer_version", version, 0);
  put_field(out, "arch", ABI_WORKER_ARCH, 0);
  put_field(out, "os", ABI_WORKER_OS, 0);
  put_field(out, "compiler", ABI_WORKER_CC, 0);
  put_key(out, "sanitizers", 0);
  put_str_list(out, ABI_WORKER_SANITIZERS);
  fputs("}\n", out);
  fflush(out);
}

/*
 * A release entered at depth 0 was entered from outside our code, which is the
 * only way to tell "the consumer released it" from "we cleaned up after a
 * consumer that did not". A counter cannot make that distinction; see
 * abi/reconstruct.h.
 */
static int released_by_consumer(const AbiObserver *o) {
  uint32_t i;

  for (i = 0; i < o->event_count; i++) {
    const AbiEvent *e = &o->events[i];
    if (!e->by_consumer) continue;
    if (e->kind == ABI_EV_SCHEMA_RELEASE_ENTER ||
        e->kind == ABI_EV_ARRAY_RELEASE_ENTER) {
      return 1;
    }
  }
  return 0;
}

static int harness_released(const AbiObserver *o) {
  uint32_t i;

  for (i = 0; i < o->event_count; i++) {
    if (o->events[i].kind == ABI_EV_HARNESS_RELEASED) return 1;
  }
  return 0;
}

static void put_observer(FILE *out, const AbiObserver *o) {
  put_key(out, "observer", 0);
  fputc('{', out);
  put_uint(out, "violations", (unsigned long long)o->violations, 1);
  put_uint(out, "bytes_allocated", o->alloc.bytes_allocated, 0);
  put_uint(out, "bytes_freed", o->alloc.bytes_freed, 0);
  put_uint(out, "blocks_allocated", o->alloc.blocks_allocated, 0);
  put_uint(out, "blocks_freed", o->alloc.blocks_freed, 0);
  /*
   * `outstanding`, not `leaked`. The counter means "at this instant"; whether
   * outstanding bytes are a leak depends on whether anything is still alive
   * that could release them, which is the consumer's side of the question and
   * not this one's (issue #6).
   */
  put_uint(out, "outstanding", o->alloc.bytes_allocated - o->alloc.bytes_freed,
           0);
  put_bool(out, "released_by_consumer", released_by_consumer(o), 0);
  put_bool(out, "harness_released", harness_released(o), 0);
  put_uint(out, "events", (unsigned long long)o->event_count, 0);
  put_uint(out, "events_dropped", (unsigned long long)o->events_dropped, 0);
  fputc('}', out);
}

static void digest_half(int *have, AbiDigest *dst, char *reason,
                        size_t reason_size, const struct ArrowSchema *s,
                        const struct ArrowArray *a) {
  AbiError  err;
  AbiStatus st;

  memset(&err, 0, sizeof(err));
  *have = 0;
  reason[0] = 0;
  if (!s || !a) {
    /* A schema-only case delivers no array, so there is nothing to digest. */
    snprintf(reason, reason_size, "no array to digest");
    return;
  }
  st = abi_digest(s, a, dst, &err);
  if (st != ABI_OK) {
    snprintf(reason, reason_size, "%s: %s", abi_status_str(st), err.message);
    return;
  }
  *have = 1;
}

void abi_worker_digest_sent(AbiWorkerDigest *d, const struct ArrowSchema *s,
                            const struct ArrowArray *a) {
  digest_half(&d->have_sent, &d->sent, d->sent_error, sizeof(d->sent_error), s,
              a);
}

void abi_worker_digest_received(AbiWorkerDigest *d, const struct ArrowSchema *s,
                                const struct ArrowArray *a) {
  digest_half(&d->have_received, &d->received, d->received_error,
              sizeof(d->received_error), s, a);
}

/* One half: an object when computed, `<key>_error` when it failed, else
   nothing at all. */
static void put_digest_half(FILE *out, const char *key, int have,
                            const AbiDigest *dg, const char *reason) {
  char hex[ABI_DIGEST_HEX_SIZE];
  char error_key[32];

  if (have) {
    put_key(out, key, 0);
    fputc('{', out);
    abi_digest_hex(dg->physical, hex);
    put_field(out, "physical", hex, 1);
    abi_digest_hex(dg->logical, hex);
    put_field(out, "logical", hex, 0);
    fputc('}', out);
  } else if (reason[0]) {
    snprintf(error_key, sizeof(error_key), "%s_error", key);
    put_field(out, error_key, reason, 0);
  }
}

static void put_digest(FILE *out, const AbiWorkerDigest *d) {
  put_key(out, "digest", 0);
  fputc('{', out);
  /* Digests under different versions are not comparable (docs/digest.md 5),
     so every line says which one it carries. */
  put_uint(out, "ver", (unsigned long long)ABI_DIGEST_VER, 1);
  put_digest_half(out, "sent", d->have_sent, &d->sent, d->sent_error);
  put_digest_half(out, "received", d->have_received, &d->received,
                  d->received_error);
  fputc('}', out);
}

void abi_worker_result(AbiWorker *w, const char *case_path, const char *id,
                       const char *status, const char *detail,
                       const AbiObserver *obs, const AbiWorkerDigest *dg) {
  FILE *out = w->out;

  fputc('{', out);
  put_field(out, "case", case_path, 1);
  put_field(out, "id", id ? id : "", 0);
  put_field(out, "status", status, 0);
  put_field(out, "detail", detail, 0);
  if (obs) put_observer(out, obs);
  if (dg) put_digest(out, dg);
  fputs("}\n", out);
  fflush(out);
}
