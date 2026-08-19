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

#include "abi/reconstruct.h"
#include "fixture.h"

/* --- the case object ------------------------------------------------------ */

typedef struct {
  PyObject_HEAD
  AbiReconstruction *rec;
  int                array_taken;  /* __arrow_c_array__ moved the array out */
  int                schema_taken; /* __arrow_c_schema__ moved the schema out */
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

static void schema_capsule_destructor(PyObject *capsule) {
  SchemaCapsule *p =
      (SchemaCapsule *)PyCapsule_GetPointer(capsule, "arrow_schema");
  if (p == NULL) {
    PyErr_Clear();
    return;
  }
  /* Released only if the consumer did not take it: that is the contract. */
  if (p->schema.release != NULL) p->schema.release(&p->schema);
  Py_XDECREF(p->owner);
  free(p);
}

static void array_capsule_destructor(PyObject *capsule) {
  ArrayCapsule *p = (ArrayCapsule *)PyCapsule_GetPointer(capsule, "arrow_array");
  if (p == NULL) {
    PyErr_Clear();
    return;
  }
  if (p->array.release != NULL) p->array.release(&p->array);
  Py_XDECREF(p->owner);
  free(p);
}

static PyObject *make_schema_capsule(AbiCaseObject *self) {
  struct ArrowSchema *src = abi_reconstruction_schema(self->rec);
  SchemaCapsule      *p;
  PyObject           *cap;

  if (src == NULL || src->release == NULL) {
    PyErr_SetString(PyExc_RuntimeError,
                    "schema already moved out of this case");
    return NULL;
  }
  p = (SchemaCapsule *)calloc(1, sizeof(SchemaCapsule));
  if (p == NULL) return PyErr_NoMemory();

  memcpy(&p->schema, src, sizeof(p->schema)); /* bitwise move */
  src->release = NULL;                        /* source marked released */
  p->owner = (PyObject *)self;
  Py_INCREF(self);

  cap = PyCapsule_New(&p->schema, "arrow_schema", schema_capsule_destructor);
  if (cap == NULL) {
    if (p->schema.release != NULL) p->schema.release(&p->schema);
    Py_DECREF(self);
    free(p);
    return NULL;
  }
  self->schema_taken = 1;
  return cap;
}

static PyObject *make_array_capsule(AbiCaseObject *self) {
  struct ArrowArray *src = abi_reconstruction_array(self->rec);
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

  memcpy(&p->array, src, sizeof(p->array));
  src->release = NULL;
  p->owner = (PyObject *)self;
  Py_INCREF(self);

  cap = PyCapsule_New(&p->array, "arrow_array", array_capsule_destructor);
  if (cap == NULL) {
    if (p->array.release != NULL) p->array.release(&p->array);
    Py_DECREF(self);
    free(p);
    return NULL;
  }
  self->array_taken = 1;
  return cap;
}

/* --- the Arrow PyCapsule interface ---------------------------------------- */

static PyObject *AbiCase_arrow_c_schema(PyObject *selfobj, PyObject *Py_UNUSED(a)) {
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

/* --- lifecycle reporting --------------------------------------------------- */

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

  /* "O" rather than "N": N steals the reference, and on a partial failure it
     is unspecified whether it stole before failing, which makes the error path
     a coin-flip between a leak and a double free. */
  result = Py_BuildValue(
      "{s:K,s:K,s:K,s:K,s:O,s:I,s:I,s:O,s:O,s:O}", "bytes_allocated",
      (unsigned long long)o->alloc.bytes_allocated, "bytes_freed",
      (unsigned long long)o->alloc.bytes_freed, "blocks_allocated",
      (unsigned long long)o->alloc.blocks_allocated, "blocks_freed",
      (unsigned long long)o->alloc.blocks_freed, "leaked",
      abi_reconstruction_leaked(self->rec) ? Py_True : Py_False, "violations",
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
static PyObject *AbiCase_release_all(PyObject *selfobj, PyObject *Py_UNUSED(a)) {
  abi_reconstruction_release_all(((AbiCaseObject *)selfobj)->rec);
  Py_RETURN_NONE;
}

static void AbiCase_dealloc(PyObject *selfobj) {
  AbiCaseObject *self = (AbiCaseObject *)selfobj;
  abi_reconstruction_free(self->rec);
  self->rec = NULL;
  Py_TYPE(self)->tp_free(selfobj);
}

static PyMethodDef AbiCase_methods[] = {
    {"__arrow_c_schema__", AbiCase_arrow_c_schema, METH_NOARGS,
     "Arrow PyCapsule interface: export the schema."},
    {"__arrow_c_array__", AbiCase_arrow_c_array, METH_VARARGS,
     "Arrow PyCapsule interface: export (schema, array)."},
    {"lifecycle", AbiCase_lifecycle, METH_NOARGS,
     "Observed lifecycle: event log, allocation delta, violations."},
    {"release_all", AbiCase_release_all, METH_NOARGS,
     "Release anything the consumer left live."},
    {NULL, NULL, 0, NULL}};

static PyTypeObject AbiCaseType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "_abicase.Case",
    .tp_basicsize = sizeof(AbiCaseObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "A reconstructed .abicase, exported as an Arrow PyCapsule producer.",
    .tp_methods = AbiCase_methods,
    .tp_dealloc = AbiCase_dealloc,
};

/* --- module functions ------------------------------------------------------ */

static PyObject *wrap_case(AbiCase *c) {
  AbiCaseObject     *obj;
  AbiReconstruction *rec = NULL;
  AbiError           err;
  AbiStatus          st;

  if (c == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "could not build the case");
    return NULL;
  }
  memset(&err, 0, sizeof(err));
  st = abi_reconstruct(c, &rec, &err);
  abi_case_free(c); /* the reconstruction copies everything it needs */
  if (st != ABI_OK) {
    PyErr_Format(PyExc_RuntimeError, "reconstruct failed: %s (%s)",
                 abi_status_str(st), err.message);
    return NULL;
  }

  obj = PyObject_New(AbiCaseObject, &AbiCaseType);
  if (obj == NULL) {
    abi_reconstruction_free(rec);
    return NULL;
  }
  obj->rec = rec;
  obj->array_taken = 0;
  obj->schema_taken = 0;
  return (PyObject *)obj;
}

static PyObject *mod_load(PyObject *Py_UNUSED(m), PyObject *args) {
  const char *path;
  AbiCase    *c = NULL;
  AbiError    err;
  AbiStatus   st;

  if (!PyArg_ParseTuple(args, "s", &path)) return NULL;
  memset(&err, 0, sizeof(err));
  st = abi_case_read_file(path, &c, &err);
  if (st != ABI_OK) {
    PyErr_Format(PyExc_ValueError, "%s: %s (%s at byte %llu)", path,
                 abi_status_str(st), err.message,
                 (unsigned long long)err.offset);
    return NULL;
  }
  return wrap_case(c);
}

static PyObject *mod_smoke(PyObject *Py_UNUSED(m), PyObject *Py_UNUSED(a)) {
  return wrap_case(abi_fixture_smoke());
}

static PyObject *mod_rich(PyObject *Py_UNUSED(m), PyObject *Py_UNUSED(a)) {
  return wrap_case(abi_fixture_rich());
}

static PyObject *mod_bad_dict_index(PyObject *Py_UNUSED(m),
                                    PyObject *Py_UNUSED(a)) {
  return wrap_case(abi_fixture_bad_dict_index());
}

static PyMethodDef module_methods[] = {
    {"load", mod_load, METH_VARARGS, "Load a .abicase file and reconstruct it."},
    {"smoke", mod_smoke, METH_NOARGS, "The plainly-valid struct<int32, utf8> fixture."},
    {"rich", mod_rich, METH_NOARGS, "The full-feature fixture."},
    {"bad_dict_index", mod_bad_dict_index, METH_NOARGS,
     "Class B1: a dictionary index out of range for its dictionary."},
    {NULL, NULL, 0, NULL}};

static struct PyModuleDef abicase_module = {
    PyModuleDef_HEAD_INIT, "_abicase",
    "Present a .abicase to a Python Arrow consumer via the PyCapsule interface.",
    -1, module_methods, NULL, NULL, NULL, NULL};

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
