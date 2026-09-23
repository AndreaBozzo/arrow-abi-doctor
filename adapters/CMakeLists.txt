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

# DuckDB is opt-in: it is built from source (duckdb/CMakeLists.txt), which a
# build that does not run it should not pay for. The worker itself is C.
option(ABI_DUCKDB "Build the DuckDB worker, and DuckDB with it" OFF)
if(ABI_DUCKDB)
  add_subdirectory(duckdb)
endif()

# Arrow C++ is opt-in for the same reason, and its worker is C++.
option(ABI_ARROW_CPP "Build the Arrow C++ worker, and Arrow C++ with it" OFF)
if(ABI_ARROW_CPP)
  if(NOT ABI_CXX)
    message(FATAL_ERROR "ABI_ARROW_CPP needs ABI_CXX=ON: its worker is C++")
  endif()
  add_subdirectory(arrow_cpp)
endif()
