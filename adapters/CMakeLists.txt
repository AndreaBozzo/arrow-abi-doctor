# The dataprof adapter is a CPython extension built by its own setup.py and is
# deliberately not wired in here: it needs a Python development environment
# that this build does not require.
add_library(abi_worker STATIC common/worker.c)
target_include_directories(abi_worker PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(abi_worker PUBLIC abi PRIVATE abi_flags)
if(ABI_SANITIZERS)
  # UBSan predefines no macro of its own, so the build states what it enabled
  # rather than the source guessing. A report that says "no leaks" without
  # saying whether a detector was linked in has not measured anything.
  target_compile_definitions(abi_worker PRIVATE
    ABI_WORKER_SANITIZERS="address,undefined")
endif()

add_executable(abi-worker-null null/null_worker.c)
target_link_libraries(abi-worker-null PRIVATE abi_worker abi abi_flags)

# Not a consumer. It exists so the coordinator's crash, timeout and truncation
# paths are exercised by a process that really does those things.
add_executable(abi-worker-faulty faulty/faulty_worker.c)
target_link_libraries(abi-worker-faulty PRIVATE abi_worker abi abi_flags)
