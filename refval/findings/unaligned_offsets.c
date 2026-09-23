/*
 * nanoarrow's validator reads an unaligned offsets buffer through an int32_t
 * pointer -- a reproducer that needs nothing but nanoarrow.
 *
 * The C Data Interface permits unaligned buffers: "It is recommended, but not
 * required, that the memory addresses of the buffers be aligned at least
 * according to the type of primitive data that they contain. Consumers MAY
 * decide not to support unaligned memory." (CDataInterface.rst, "Buffers").
 * nanoarrow neither declines nor documents declining it; it validates the
 * array by loading `data.as_int32[i]` from the offsets buffer, which for an
 * address one byte past an int32 boundary is undefined behaviour in C -- a
 * misaligned load x86 tolerates and a strict-alignment target need not.
 *
 * Build it against the bundle with UBSan and run it:
 *
 *   cc -std=c99 -fsanitize=undefined -I nanoarrow/include \
 *      findings/unaligned_offsets.c nanoarrow/src/nanoarrow.c && ./a.out
 *
 * which reports `load of misaligned address ... for type 'const int32_t'`
 * inside ArrowArrayViewValidateDefault(). The array itself is a plain three-
 * element string array built by nanoarrow; the only change is the offsets
 * buffer moved one byte in, which is the `+1` alignment class of the bounded
 * conformance model (docs/coverage-matrix.md 1.6), where the harness found it.
 *
 * Status and evidence are in refval/README.md. When a nanoarrow that no longer
 * does this is vendored, the ctest that runs this program starts failing, and
 * the alignment carve-out in refval/CMakeLists.txt comes out with it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nanoarrow/nanoarrow.h"

int main(void) {
  struct ArrowArray      array;
  struct ArrowArrayView  view;
  struct ArrowError      error;
  struct ArrowStringView values[] = {{"a", 1}, {"bb", 2}, {"ccc", 3}};
  const void            *aligned_offsets;
  size_t                 offsets_size = (3 + 1) * sizeof(int32_t);
  unsigned char         *block;
  int                    i, rc;

  if (ArrowArrayInitFromType(&array, NANOARROW_TYPE_STRING) != NANOARROW_OK ||
      ArrowArrayStartAppending(&array) != NANOARROW_OK) {
    return 2;
  }
  for (i = 0; i < 3; i++) {
    if (ArrowArrayAppendString(&array, values[i]) != NANOARROW_OK) return 2;
  }
  if (ArrowArrayFinishBuildingDefault(&array, &error) != NANOARROW_OK) return 2;

  /* The same offsets, one byte past an int32 boundary. */
  block = (unsigned char *)malloc(offsets_size + 8);
  if (!block) return 2;
  aligned_offsets = array.buffers[1];
  memcpy(block + 1, aligned_offsets, offsets_size);
  array.buffers[1] = block + 1;

  ArrowArrayViewInitFromType(&view, NANOARROW_TYPE_STRING);
  /* SetArray() validates at the default level, which reads the offsets. */
  rc = ArrowArrayViewSetArray(&view, &array, &error);
  printf("ArrowArrayViewSetArray: %d %s\n", rc, rc ? error.message : "(valid)");

  ArrowArrayViewReset(&view);
  array.buffers[1] = aligned_offsets; /* nanoarrow frees its own buffer */
  array.release(&array);
  free(block);
  return 0;
}
