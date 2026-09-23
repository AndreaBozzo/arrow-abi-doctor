# nanoarrow, vendored as its bundler produced it (README.md here). Built without
# abi_flags' warning set: its warnings are upstream's to fix, and -Werror on them
# would make this build fail the day a compiler learns a new one, for a reason
# unrelated to anything here.
add_library(refval_nanoarrow STATIC nanoarrow/src/nanoarrow.c)
target_include_directories(refval_nanoarrow PUBLIC nanoarrow/include)
set_target_properties(refval_nanoarrow PROPERTIES C_STANDARD 99)

if(ABI_SANITIZERS)
  # Instrumented like everything else -- a memory error inside the reference
  # validator is a finding -- with one known exception, carved out by name.
  # nanoarrow validates a utf8/binary array by loading its offsets through an
  # int32_t pointer, which for an unaligned offsets buffer (legal: "Consumers
  # MAY decide not to support unaligned memory") is a misaligned load and
  # undefined behaviour. Left on, UBSan would abort the validator at the first
  # such case and hide everything else it could catch. The finding is not
  # hidden: refval_finding_unaligned_offsets below reproduces it on every
  # sanitizer run, and fails the day a vendored nanoarrow stops doing it --
  # which is when this carve-out comes out. refval/README.md has the evidence.
  target_compile_options(refval_nanoarrow PRIVATE
    -fsanitize=address,undefined -fno-sanitize=alignment
    -fno-omit-frame-pointer -fno-sanitize-recover=all)

  # The standalone reproducer, against a fully instrumented copy of nanoarrow.
  # It needs nothing of this project; the test requires UBSan's report.
  add_executable(refval_finding_unaligned_offsets
    findings/unaligned_offsets.c nanoarrow/src/nanoarrow.c)
  target_include_directories(refval_finding_unaligned_offsets PRIVATE
    nanoarrow/include)
  set_target_properties(refval_finding_unaligned_offsets PROPERTIES C_STANDARD 99)
  target_link_libraries(refval_finding_unaligned_offsets PRIVATE abi_sanitize)
  add_test(NAME refval_finding_unaligned_offsets
           COMMAND refval_finding_unaligned_offsets)
  set_tests_properties(refval_finding_unaligned_offsets PROPERTIES
    PASS_REGULAR_EXPRESSION "load of misaligned address .* for type 'const int32_t'")
endif()

add_executable(abi-worker-refval refval_worker.c)
target_link_libraries(abi-worker-refval PRIVATE abi_worker refval_nanoarrow abi abi_flags)
