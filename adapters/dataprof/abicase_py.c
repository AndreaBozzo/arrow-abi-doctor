/*
 * _abicase -- a CPython extension exposing a .abicase as an Arrow PyCapsule
 * producer.
 *
 * This is the M0.5 adapter. dataprof consumes any object implementing
 * __arrow_c_array__, so presenting a reconstructed case that way puts it
 * through dataprof's real production import path -- PyCapsule -> arrow-rs
 * from_ffi -> RecordBatch -> profiling -- rather than through a bespoke test
 * hook that would prove nothing about the instrument.
 *
 * Ownership follows the Arrow PyCapsule interface: the capsule takes the
 * structure by bitwise move, the source is marked released, and the capsule's
 * destructor releases it only if the consumer never did. That means the smoke
 * test exercises move semantics for real, against a real consumer.
 *
 * The subtlety worth naming: our release callbacks reach back into the
 * AbiReconstruction for the event log and the allocation counters, so the
 * reconstruction has to outlive every capsule cut from it. Each capsule
 * therefore holds a counted reference to the Python object that owns it.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <stdlib.h>
#include <string.h>

#include "abi/callseq.h"
#include "abi/digest.h"
#include "abi/lifecycle.h"
#include "abi/reconstruct.h"
#include "fixture.h"

/* --- the case object ------------------------------------------------------ */

typedef struct {
  PyObject_HEAD AbiReconstruction *rec;
  int array_taken;  /* __arrow_c_array__ moved the array out */
  int schema_taken; /* __arrow_c_schema__ moved the schema out */
  /*
   * Capsules cut from this case that have not yet been destroyed. A consumer
   * that takes a capsule and then fails without consuming it leaves a live
   * structure the capsule owns; release_all() cannot reach it, because the
   * structure was moved out. The bytes are outstanding at that instant and
   * nothing has leaked -- Python releases them when it collects the capsule.
   * Without this count the two states are indistinguishable and the harness
   * reports a leak that is not there (issue #6).
   */
  int capsules_outstanding;
  /* The payload digest, taken before the case itself is freed. */
  char id[ABICASE_ID_HEX_SIZE];
  /*
   * Where the base structures live now. The reconstruction's own copies until
   * move(), then storage this object owns (moved_*): after a move the
   * reconstruction holds released shells, and exporting or digesting those
   * would describe nothing that is handed over.
   */
  struct ArrowSchema *schema_at;
  struct ArrowArray  *array_at;
  struct ArrowSchema *moved_schema;
  struct ArrowArray  *moved_array;
  /* The case's CALLSEQ, kept past the case itself for the worker to run. */
  AbiOp   *ops;
  uint32_t op_count;
} AbiCaseObject;

/*
 * Capsule payloads. The Arrow structure is the FIRST member, so the capsule
 * pointer and the payload address are the same and the destructor can cast
 * back. `owner` keeps the reconstruction alive for as long as the capsule is.
 */
typedef struct {
  struct ArrowSchema schema;
  PyObject          *owner;
} SchemaCapsule;

typedef struct {
  struct ArrowArray array;
  PyObject         *owner;
} ArrayCapsule;

typedef struct {
  struct ArrowArrayStream stream;
  PyObject               *owner;
} StreamCapsule;

static void schema_capsule_destructor(PyObject *capsule) {
  SchemaCapsule *p =
      (SchemaCapsule *)PyCapsule_GetPointer(capsule, "arrow_schema");
  if (p == NULL) {
    PyErr_Clear();
    return;
  }
  /* Released only if the consumer did not take it: that is the contract. */
  if (p->schema.release != NULL) p->schema.release(&p->schema);
  /* `owner` is set before the capsule exists, so a destructor always has one.
     Decrement before the DECREF, which may be the owner's last reference. */
  ((AbiCaseObject *)p->owner)->capsules_outstanding--;
  Py_XDECREF(p->owner);
  free(p);
}

static void array_capsule_destructor(PyObject *capsule) {
  ArrayCapsule *p =
      (ArrayCapsule *)PyCapsule_GetPointer(capsule, "arrow_array");
  if (p == NULL) {
    PyErr_Clear();
    return;
  }
  if (p->array.release != NULL) p->array.release(&p->array);
  ((AbiCaseObject *)p->owner)->capsules_outstanding--;
  Py_XDECREF(p->owner);
  free(p);
}

/*
 * The capsule takes the structure by an Arrow move -- logged, so the lifecycle
 * state machine knows where it went -- and the capsule going to the consumer is
 * the handoff. Not a strict location from there on: a consumer moves the
 * structure into its own storage when it imports, and tells nobody.
 */
static PyObject *make_schema_capsule(AbiCaseObject *self) {
  struct ArrowSchema *src = self->schema_at;
  SchemaCapsule      *p;
  PyObject           *cap;

  if (src == NULL || src->release == NULL) {
    PyErr_SetString(PyExc_RuntimeError,
                    "schema already moved out of this case");
    return NULL;
  }
  p = (SchemaCapsule *)calloc(1, sizeof(SchemaCapsule));
  if (p == NULL) return PyErr_NoMemory();

  abi_reconstruction_move_schema(self->rec, &p->schema, src);
  p->owner = (PyObject *)self;
  Py_INCREF(self);

  cap = PyCapsule_New(&p->schema, "arrow_schema", schema_capsule_destructor);
  if (cap == NULL) {
    abi_reconstruction_harness_release_schema(self->rec, &p->schema);
    Py_DECREF(self);
    free(p);
    return NULL;
  }
  abi_reconstruction_note_schema_import(self->rec, &p->schema);
  self->schema_taken = 1;
  self->capsules_outstanding++;
  return cap;
}

static PyObject *make_array_capsule(AbiCaseObject *self) {
  struct ArrowArray *src = self->array_at;
  ArrayCapsule      *p;
  PyObject          *cap;

  if (src == NULL) {
    PyErr_SetString(PyExc_RuntimeError,
                    "this case has no array (schema-only case)");
    return NULL;
  }
  if (src->release == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "array already moved out of this case");
    return NULL;
  }
  p = (ArrayCapsule *)calloc(1, sizeof(ArrayCapsule));
  if (p == NULL) return PyErr_NoMemory();

  abi_reconstruction_move_array(self->rec, &p->array, src);
  p->owner = (PyObject *)self;
  Py_INCREF(self);

  cap = PyCapsule_New(&p->array, "arrow_array", array_capsule_destructor);
  if (cap == NULL) {
    abi_reconstruction_harness_release_array(self->rec, &p->array);
    Py_DECREF(self);
    free(p);
    return NULL;
  }
  abi_reconstruction_note_array_import(self->rec, &p->array);
  self->array_taken = 1;
  self->capsules_outstanding++;
  return cap;
}

static void stream_capsule_destructor(PyObject *capsule) {
  StreamCapsule *p =
      (StreamCapsule *)PyCapsule_GetPointer(capsule, "arrow_array_stream");
  if (p == NULL) {
    PyErr_Clear();
    return;
  }
  if (p->stream.release != NULL) p->stream.release(&p->stream);
  ((AbiCaseObject *)p->owner)->capsules_outstanding--;
  Py_XDECREF(p->owner);
  free(p);
}

/* --- the Arrow PyCapsule interface ---------------------------------------- */

static PyObject *AbiCase_arrow_c_schema(PyObject *selfobj,
                                        PyObject *Py_UNUSED(a)) {
  return make_schema_capsule((AbiCaseObject *)selfobj);
}

/*
 * __arrow_c_array__(requested_schema=None) -> (schema_capsule, array_capsule)
 *
 * requested_schema is accepted and ignored: the specification lets a producer
 * ignore it, and a case exists precisely to be delivered exactly as written.
 */
static PyObject *AbiCase_arrow_c_array(PyObject *selfobj, PyObject *args) {
  AbiCaseObject *self = (AbiCaseObject *)selfobj;
  PyObject      *requested = Py_None;
  PyObject      *schema_cap, *array_cap, *tuple;

  if (!PyArg_ParseTuple(args, "|O", &requested)) return NULL;

  schema_cap = make_schema_capsule(self);
  if (schema_cap == NULL) return NULL;
  array_cap = make_array_capsule(self);
  if (array_cap == NULL) {
    Py_DECREF(schema_cap);
    return NULL;
  }
  tuple = PyTuple_Pack(2, schema_cap, array_cap);
  Py_DECREF(schema_cap);
  Py_DECREF(array_cap);
  return tuple;
}

/* --- lifecycle reporting ---------------------------------------------------
 */

static PyObject *event_to_dict(const AbiEvent *e) {
  return Py_BuildValue("{s:I,s:s,s:i,s:O,s:s}", "seq", (unsigned int)e->seq,
                       "kind", abi_event_kind_str((AbiEventKind)e->kind),
                       "depth", (int)e->depth, "by_consumer",
                       e->by_consumer ? Py_True : Py_False, "path", e->path);
}

static PyObject *AbiCase_lifecycle(PyObject *selfobj, PyObject *Py_UNUSED(a)) {
  AbiCaseObject     *self = (AbiCaseObject *)selfobj;
  const AbiObserver *o = abi_reconstruction_observer(self->rec);
  PyObject          *events, *result;
  uint32_t           i;

  events = PyList_New(0);
  if (events == NULL) return NULL;
  for (i = 0; i < o->event_count; i++) {
    PyObject *d = event_to_dict(&o->events[i]);
    if (d == NULL || PyList_Append(events, d) != 0) {
      Py_XDECREF(d);
      Py_DECREF(events);
      return NULL;
    }
    Py_DECREF(d);
  }

  /*
   * `outstanding` is what the allocator has not seen freed at this instant. It
   * is deliberately not called "leaked": while capsules_outstanding is
   * non-zero, some of it is owned by a live capsule and will be released when
   * Python collects it. Only the caller, which knows whether the capsules are
   * gone, can turn these two numbers into a verdict -- see issue #6.
   *
   * libabi keeps the name `abi_reconstruction_leaked` because there it is
   * accurate: nothing can move a structure out of a reconstruction in libabi's
   * own tests. The ambiguity is created by this adapter, so the renaming
   * belongs here.
   */
  /* "O" rather than "N": N steals the reference, and on a partial failure it
     is unspecified whether it stole before failing, which makes the error path
     a coin-flip between a leak and a double free. */
  result = Py_BuildValue(
      "{s:K,s:K,s:K,s:K,s:O,s:i,s:I,s:I,s:O,s:O,s:O}", "bytes_allocated",
      (unsigned long long)o->alloc.bytes_allocated, "bytes_freed",
      (unsigned long long)o->alloc.bytes_freed, "blocks_allocated",
      (unsigned long long)o->alloc.blocks_allocated, "blocks_freed",
      (unsigned long long)o->alloc.blocks_freed, "outstanding",
      abi_reconstruction_leaked(self->rec) ? Py_True : Py_False,
      "capsules_outstanding", self->capsules_outstanding, "violations",
      (unsigned int)o->violations, "events_dropped",
      (unsigned int)o->events_dropped, "schema_taken",
      self->schema_taken ? Py_True : Py_False, "array_taken",
      self->array_taken ? Py_True : Py_False, "events", events);
  Py_DECREF(events);
  return result;
}

/*
 * Releases anything the consumer left live. Called before reading the final
 * lifecycle report, so that "the consumer released it" and "we cleaned up after
 * a consumer that did not" stay distinguishable in the log.
 */
static void release_everything(AbiCaseObject *self) {
  /* Moved storage first: the reconstruction cannot see it, and after a move
     its own copies are released shells that release_all() passes over. */
  if (self->moved_array)
    abi_reconstruction_harness_release_array(self->rec, self->moved_array);
  if (self->moved_schema)
    abi_reconstruction_harness_release_schema(self->rec, self->moved_schema);
  abi_reconstruction_release_all(self->rec);
}

static PyObject *AbiCase_release_all(PyObject *selfobj,
                                     PyObject *Py_UNUSED(a)) {
  release_everything((AbiCaseObject *)selfobj);
  Py_RETURN_NONE;
}

/* --- the call sequence ------------------------------------------------------
 */

/* [(op name, arg0, arg1), ...] -- empty for a case with no CALLSEQ. */
static PyObject *AbiCase_callseq(PyObject *selfobj, PyObject *Py_UNUSED(a)) {
  AbiCaseObject *self = (AbiCaseObject *)selfobj;
  PyObject      *list = PyList_New(0);
  uint32_t       i;

  if (list == NULL) return NULL;
  for (i = 0; i < self->op_count; i++) {
    const AbiOp *op = &self->ops[i];
    PyObject *t = Py_BuildValue("(sII)", abi_op_str((AbiOpCode)op->code),
                                (unsigned int)op->arg0, (unsigned int)op->arg1);
    if (t == NULL || PyList_Append(list, t) != 0) {
      Py_XDECREF(t);
      Py_DECREF(list);
      return NULL;
    }
    Py_DECREF(t);
  }
  return list;
}

/*
 * MOVE_STRUCT: both base structures to storage this object owns, per Arrow
 * move semantics, logged. Refused once either has been handed over -- moving a
 * structure out from under the consumer that holds it would be the harness
 * breaking the handoff.
 */
static PyObject *AbiCase_move(PyObject *selfobj, PyObject *Py_UNUSED(a)) {
  AbiCaseObject      *self = (AbiCaseObject *)selfobj;
  struct ArrowSchema *s;
  struct ArrowArray  *arr = NULL;

  if (self->schema_taken || self->array_taken) {
    PyErr_SetString(PyExc_RuntimeError,
                    "move after export: the consumer already holds it");
    return NULL;
  }
  s = (struct ArrowSchema *)calloc(1, sizeof(*s));
  if (self->array_at) arr = (struct ArrowArray *)calloc(1, sizeof(*arr));
  if (s == NULL || (self->array_at && arr == NULL)) {
    free(s);
    free(arr);
    return PyErr_NoMemory();
  }
  abi_reconstruction_move_schema(self->rec, s, self->schema_at);
  if (arr) abi_reconstruction_move_array(self->rec, arr, self->array_at);
  free(self->moved_schema);
  free(self->moved_array);
  self->schema_at = self->moved_schema = s;
  if (arr) self->array_at = self->moved_array = arr;
  Py_RETURN_NONE;
}

/*
 * The lifecycle state machine over this case's log (abi/lifecycle.h):
 * {"strict": bool, "incomplete": bool, "count": n, "violations": [str, ...]}.
 * Read it after every owner is gone -- the consumer's objects collected, and
 * release_all() run.
 */
static PyObject *AbiCase_lifecycle_verdict(PyObject *selfobj, PyObject *args) {
  AbiCaseObject      *self = (AbiCaseObject *)selfobj;
  AbiLifecycleVerdict v;
  PyObject           *list, *result;
  int                 strict = 0;
  uint32_t            i;
  char                item[96];

  if (!PyArg_ParseTuple(args, "|p", &strict)) return NULL;
  abi_lifecycle_verify(abi_reconstruction_observer(self->rec), strict, &v);
  list = PyList_New(0);
  if (list == NULL) return NULL;
  for (i = 0; i < v.count && i < ABI_LC_MAX_FINDINGS; i++) {
    const AbiLifecycleFinding *f = &v.findings[i];
    PyObject                  *s;
    snprintf(item, sizeof(item), "%s %s %s",
             abi_lifecycle_rule_str((AbiLifecycleRule)f->rule),
             abi_lifecycle_object_str((AbiLifecycleObject)f->object), f->path);
    s = PyUnicode_FromString(item);
    if (s == NULL || PyList_Append(list, s) != 0) {
      Py_XDECREF(s);
      Py_DECREF(list);
      return NULL;
    }
    Py_DECREF(s);
  }
  result =
      Py_BuildValue("{s:O,s:O,s:I,s:O}", "strict", strict ? Py_True : Py_False,
                    "incomplete", v.incomplete ? Py_True : Py_False, "count",
                    (unsigned int)v.count, "violations", list);
  Py_DECREF(list);
  return result;
}

/* --- digests ---------------------------------------------------------------
 */

/* {"physical": hex, "logical": hex}, or NULL with ValueError set. */
static PyObject *digest_dict(const struct ArrowSchema *schema,
                             const struct ArrowArray  *array) {
  AbiDigest d;
  AbiError  err;
  AbiStatus st;
  char      physical[ABI_DIGEST_HEX_SIZE], logical[ABI_DIGEST_HEX_SIZE];

  memset(&err, 0, sizeof(err));
  st = abi_digest(schema, array, &d, &err);
  if (st != ABI_OK) {
    PyErr_Format(PyExc_ValueError, "digest: %s (%s)", abi_status_str(st),
                 err.message);
    return NULL;
  }
  abi_digest_hex(d.physical, physical);
  abi_digest_hex(d.logical, logical);
  return Py_BuildValue("{s:s,s:s}", "physical", physical, "logical", logical);
}

/*
 * The digest of what this case will hand over -- the `sent` half of a worker
 * result line. Only while the structures are still here: once a consumer has
 * taken them, what is left is a released shell, and digesting that would
 * describe nothing that was sent.
 */
static PyObject *AbiCase_digest(PyObject *selfobj, PyObject *Py_UNUSED(a)) {
  AbiCaseObject      *self = (AbiCaseObject *)selfobj;
  struct ArrowSchema *schema = self->schema_at;
  struct ArrowArray  *array = self->array_at;

  if (array == NULL) {
    PyErr_SetString(PyExc_ValueError,
                    "this case has no array (schema-only case)");
    return NULL;
  }
  if (schema->release == NULL || array->release == NULL) {
    PyErr_SetString(PyExc_RuntimeError,
                    "digest before export: the structures have been moved out "
                    "or released");
    return NULL;
  }
  return digest_dict(schema, array);
}

/* The case id: a digest over the payload, not the file name it came from. */
static PyObject *AbiCase_case_id(PyObject *selfobj, PyObject *Py_UNUSED(a)) {
  return PyUnicode_FromString(((AbiCaseObject *)selfobj)->id);
}

static void AbiCase_dealloc(PyObject *selfobj) {
  AbiCaseObject *self = (AbiCaseObject *)selfobj;
  release_everything(self);
  abi_reconstruction_free(self->rec);
  self->rec = NULL;
  free(self->moved_schema);
  free(self->moved_array);
  free(self->ops);
  Py_TYPE(self)->tp_free(selfobj);
}

/*
 * __arrow_c_stream__(requested_schema=None) -> stream capsule, for a case
 * loaded with load(path, True). Moved into the capsule and handed over, both
 * logged, as for the schema and the array.
 */
static PyObject *AbiCase_arrow_c_stream(PyObject *selfobj, PyObject *args) {
  AbiCaseObject           *self = (AbiCaseObject *)selfobj;
  PyObject                *requested = Py_None, *cap;
  struct ArrowArrayStream *src = abi_reconstruction_stream(self->rec);
  StreamCapsule           *p;

  if (!PyArg_ParseTuple(args, "|O", &requested)) return NULL;
  if (src == NULL) {
    PyErr_SetString(PyExc_RuntimeError,
                    "not loaded as a stream: load(path, True)");
    return NULL;
  }
  if (src->release == NULL) {
    PyErr_SetString(PyExc_RuntimeError,
                    "stream already moved out of this case");
    return NULL;
  }
  p = (StreamCapsule *)calloc(1, sizeof(StreamCapsule));
  if (p == NULL) return PyErr_NoMemory();
  abi_reconstruction_move_stream(self->rec, &p->stream, src);
  p->owner = (PyObject *)self;
  Py_INCREF(self);
  cap = PyCapsule_New(&p->stream, "arrow_array_stream",
                      stream_capsule_destructor);
  if (cap == NULL) {
    abi_reconstruction_harness_release_stream(self->rec, &p->stream);
    Py_DECREF(self);
    free(p);
    return NULL;
  }
  abi_reconstruction_note_stream_import(self->rec, &p->stream);
  self->capsules_outstanding++;
  return cap;
}

static PyMethodDef AbiCase_methods[] = {
    {"__arrow_c_stream__", AbiCase_arrow_c_stream, METH_VARARGS,
     "Arrow PyCapsule interface: export the stream (a load(path, True) case)."},
    {"__arrow_c_schema__", AbiCase_arrow_c_schema, METH_NOARGS,
     "Arrow PyCapsule interface: export the schema."},
    {"__arrow_c_array__", AbiCase_arrow_c_array, METH_VARARGS,
     "Arrow PyCapsule interface: export (schema, array)."},
    {"lifecycle", AbiCase_lifecycle, METH_NOARGS,
     "Observed lifecycle: event log, allocation delta, violations."},
    {"release_all", AbiCase_release_all, METH_NOARGS,
     "Release anything the consumer left live."},
    {"digest", AbiCase_digest, METH_NOARGS,
     "Physical and logical digest of what this case hands over."},
    {"case_id", AbiCase_case_id, METH_NOARGS,
     "The case id: 32 hex digits of the payload digest."},
    {"callseq", AbiCase_callseq, METH_NOARGS,
     "The case's CALLSEQ as [(op, arg0, arg1)]; empty when it has none."},
    {"move", AbiCase_move, METH_NOARGS,
     "MOVE_STRUCT: move both base structures, per Arrow move semantics."},
    {"lifecycle_verdict", AbiCase_lifecycle_verdict, METH_VARARGS,
     "The lifecycle state machine's findings; lifecycle_verdict(strict)."},
    {NULL, NULL, 0, NULL}};

static PyTypeObject AbiCaseType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "_abicase.Case",
    .tp_basicsize = sizeof(AbiCaseObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc =
        "A reconstructed .abicase, exported as an Arrow PyCapsule producer.",
    .tp_methods = AbiCase_methods,
    .tp_dealloc = AbiCase_dealloc,
};

/* --- module functions ------------------------------------------------------
 */

static PyObject *wrap_case(AbiCase *c, int stream) {
  AbiCaseObject     *obj;
  AbiReconstruction *rec = NULL;
  AbiError           err;
  AbiStatus          st;
  char               id[ABICASE_ID_HEX_SIZE];
  AbiOp             *ops = NULL;
  uint32_t           op_count;

  if (c == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "could not build the case");
    return NULL;
  }
  memset(&err, 0, sizeof(err));
  abi_case_id(c, id);
  /* The CALLSEQ outlives the case: the reconstruction does not carry it. */
  op_count = c->op_count;
  if (op_count) {
    ops = (AbiOp *)malloc(op_count * sizeof(*ops));
    if (ops == NULL) {
      abi_case_free(c);
      return PyErr_NoMemory();
    }
    memcpy(ops, c->ops, op_count * sizeof(*ops));
  }
  st = stream ? abi_reconstruct_stream(c, &rec, &err)
              : abi_reconstruct(c, &rec, &err);
  abi_case_free(c); /* the reconstruction copies everything it needs */
  if (st != ABI_OK) {
    free(ops);
    PyErr_Format(PyExc_RuntimeError, "reconstruct failed: %s (%s)",
                 abi_status_str(st), err.message);
    return NULL;
  }

  obj = PyObject_New(AbiCaseObject, &AbiCaseType);
  if (obj == NULL) {
    free(ops);
    abi_reconstruction_free(rec);
    return NULL;
  }
  obj->rec = rec;
  obj->array_taken = 0;
  obj->schema_taken = 0;
  obj->capsules_outstanding = 0;
  memcpy(obj->id, id, sizeof(obj->id));
  obj->schema_at = abi_reconstruction_schema(rec);
  obj->array_at = abi_reconstruction_array(rec);
  obj->moved_schema = NULL;
  obj->moved_array = NULL;
  obj->ops = ops;
  obj->op_count = op_count;
  return (PyObject *)obj;
}

static PyObject *mod_load(PyObject *Py_UNUSED(m), PyObject *args) {
  const char *path;
  AbiCase    *c = NULL;
  AbiError    err;
  AbiStatus   st;
  int         stream = 0;

  if (!PyArg_ParseTuple(args, "s|p", &path, &stream)) return NULL;
  memset(&err, 0, sizeof(err));
  st = abi_case_read_file(path, &c, &err);
  if (st != ABI_OK) {
    PyErr_Format(PyExc_ValueError, "%s: %s (%s at byte %llu)", path,
                 abi_status_str(st), err.message,
                 (unsigned long long)err.offset);
    return NULL;
  }
  return wrap_case(c, stream);
}

static PyObject *mod_smoke(PyObject *Py_UNUSED(m), PyObject *Py_UNUSED(a)) {
  return wrap_case(abi_fixture_smoke(), 0);
}

static PyObject *mod_rich(PyObject *Py_UNUSED(m), PyObject *Py_UNUSED(a)) {
  return wrap_case(abi_fixture_rich(), 0);
}

static PyObject *mod_bad_dict_index(PyObject *Py_UNUSED(m),
                                    PyObject *Py_UNUSED(a)) {
  return wrap_case(abi_fixture_bad_dict_index(), 0);
}

/*
 * digest(schema_capsule, array_capsule) -- the `received` half: a digest of
 * structures a consumer handed back through the PyCapsule interface, e.g.
 * `pyarrow.RecordBatch.__arrow_c_array__()`. The capsules are read, not
 * consumed; each still releases its structure when it is collected.
 */
static PyObject *mod_digest(PyObject *Py_UNUSED(m), PyObject *args) {
  PyObject           *schema_cap, *array_cap;
  struct ArrowSchema *schema;
  struct ArrowArray  *array;

  if (!PyArg_ParseTuple(args, "OO", &schema_cap, &array_cap)) return NULL;
  schema =
      (struct ArrowSchema *)PyCapsule_GetPointer(schema_cap, "arrow_schema");
  if (schema == NULL) return NULL;
  array = (struct ArrowArray *)PyCapsule_GetPointer(array_cap, "arrow_array");
  if (array == NULL) return NULL;
  if (schema->release == NULL || array->release == NULL) {
    PyErr_SetString(PyExc_ValueError,
                    "digest: a capsule holds a released structure");
    return NULL;
  }
  return digest_dict(schema, array);
}

static PyMethodDef module_methods[] = {
    {"digest", mod_digest, METH_VARARGS,
     "Digest (schema_capsule, array_capsule) without consuming them."},
    {"load", mod_load, METH_VARARGS,
     "load(path, stream=False): reconstruct a .abicase; as an ArrowArrayStream "
     "when stream is true."},
    {"smoke", mod_smoke, METH_NOARGS,
     "The plainly-valid struct<int32, utf8> fixture."},
    {"rich", mod_rich, METH_NOARGS, "The full-feature fixture."},
    {"bad_dict_index", mod_bad_dict_index, METH_NOARGS,
     "Class B1: a dictionary index out of range for its dictionary."},
    {NULL, NULL, 0, NULL}};

static struct PyModuleDef abicase_module = {
    PyModuleDef_HEAD_INIT,
    "_abicase",
    "Present a .abicase to a Python Arrow consumer via the PyCapsule "
    "interface.",
    -1,
    module_methods,
    NULL,
    NULL,
    NULL,
    NULL};

PyMODINIT_FUNC PyInit__abicase(void) {
  PyObject *m;
  if (PyType_Ready(&AbiCaseType) < 0) return NULL;
  m = PyModule_Create(&abicase_module);
  if (m == NULL) return NULL;
  Py_INCREF(&AbiCaseType);
  if (PyModule_AddObject(m, "Case", (PyObject *)&AbiCaseType) < 0) {
    Py_DECREF(&AbiCaseType);
    Py_DECREF(m);
    return NULL;
  }
  if (PyModule_AddStringConstant(m, "__abi_doctor_version__",
                                 ABI_DOCTOR_VERSION) < 0) {
    Py_DECREF(m);
    return NULL;
  }
  return m;
}
