# DuckDB, built from source at a pinned release by its own CMake build.
#
# Not the release amalgamation: that is one C++ translation unit of 25 MB,
# which took 13.5 minutes and 13.7 GB of compiler memory to build plainly, and
# was still swapping on a 15 GB host after 31 minutes under ASan. DuckDB's own
# build compiles in parallel, in pieces a CI runner can hold.
#
# Pinned twice: the archive is refused unless its SHA-256 is the one below,
# and that archive's embedded commit id is the tag's commit (README.md here has
# how both were checked). ABI_DUCKDB_SOURCE_DIR points at an already extracted
# tree instead, for a host without network access; it is not re-hashed, so
# whoever sets it answers for what is there.
if(WIN32)
  message(FATAL_ERROR "ABI_DUCKDB: the DuckDB worker is built on Linux only")
endif()

include(ExternalProject)

set(ABI_DUCKDB_VERSION "1.5.5")
set(ABI_DUCKDB_COMMIT "d8cdaa33fda8df955cc76ef58a280f68f4cd43fa")
set(ABI_DUCKDB_URL
  "https://github.com/duckdb/duckdb/archive/refs/tags/v${ABI_DUCKDB_VERSION}.tar.gz")
set(ABI_DUCKDB_SHA256
  "f33155ff962e6e1e08fd1e9caffa487d4325aa60999e2eabc76feff534d6558b")
set(ABI_DUCKDB_SOURCE_DIR "" CACHE PATH
  "Extracted DuckDB source tree; the pinned archive is fetched when empty")

if(ABI_DUCKDB_SOURCE_DIR)
  set(duckdb_source SOURCE_DIR "${ABI_DUCKDB_SOURCE_DIR}")
else()
  set(duckdb_source URL "${ABI_DUCKDB_URL}" URL_HASH SHA256=${ABI_DUCKDB_SHA256})
endif()

# The shipped configuration as far as a measurement allows: optimized, its
# assertions as released. Two departures, both only under ABI_SANITIZERS:
# DuckDB is instrumented -- a memory error inside it is what this adapter
# exists to catch -- and jemalloc is off, because DuckDB allocates its vectors
# through it and ASan cannot see into an allocator it did not replace.
set(duckdb_flags "")
set(duckdb_link "")
set(duckdb_jemalloc ON)
if(ABI_SANITIZERS)
  # With one known exception, carved out by name, as refval carves out
  # nanoarrow's. DuckDB imports a primitive buffer without copying it and
  # later loads its values through a typed pointer, which for an unaligned
  # buffer (legal: "Consumers MAY decide not to support unaligned memory") is
  # a misaligned load and undefined behaviour. Left on, UBSan would abort the
  # worker at the first such case and hide everything else it could catch.
  # The finding is not hidden: duckdb_finding_unaligned_int32 below
  # reproduces it on every sanitizer run, and fails the day a pinned DuckDB
  # stops doing it -- which is when this carve-out comes out.
  set(duckdb_flags
    "-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=all -fno-omit-frame-pointer")
  set(duckdb_link "-fsanitize=address,undefined")
  set(duckdb_jemalloc OFF)
endif()

ExternalProject_Add(duckdb_ep
  ${duckdb_source}
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
    -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
    -DCMAKE_C_FLAGS=${duckdb_flags}
    -DCMAKE_CXX_FLAGS=${duckdb_flags}
    -DCMAKE_SHARED_LINKER_FLAGS=${duckdb_link}
    # DuckDB's own sanitizer switches apply UBSan to debug builds only.
    -DENABLE_SANITIZER=OFF
    -DENABLE_UBSAN=OFF
    -DENABLE_JEMALLOC=${duckdb_jemalloc}
    -DSKIP_EXTENSIONS=parquet
    -DBUILD_SHELL=OFF
    -DBUILD_UNITTESTS=OFF
    # A tarball has no .git, and without this DuckDB reports itself as v0.0.1.
    -DOVERRIDE_GIT_DESCRIBE=v${ABI_DUCKDB_VERSION}-0-g${ABI_DUCKDB_COMMIT}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target duckdb
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS <BINARY_DIR>/src/libduckdb.so
  USES_TERMINAL_BUILD TRUE)

ExternalProject_Get_Property(duckdb_ep SOURCE_DIR BINARY_DIR)
file(MAKE_DIRECTORY "${SOURCE_DIR}/src/include")
add_library(abi_duckdb SHARED IMPORTED GLOBAL)
set_target_properties(abi_duckdb PROPERTIES
  IMPORTED_LOCATION "${BINARY_DIR}/src/libduckdb.so"
  INTERFACE_INCLUDE_DIRECTORIES "${SOURCE_DIR}/src/include")
add_dependencies(abi_duckdb duckdb_ep)

add_executable(abi-worker-duckdb duckdb_worker.c)
target_link_libraries(abi-worker-duckdb PRIVATE abi_worker abi_duckdb abi abi_flags)

# The standalone reproducer for the duckdb_arrow_scan() schema leak: DuckDB's C
# API and nothing of this project. The test requires the leak -- the program
# exits 0 when it reproduces -- so it fails the day a pinned DuckDB stops
# leaking, which is when README.md's finding needs revisiting.
add_executable(duckdb_finding_arrow_scan_schema_leak
  findings/arrow_scan_schema_leak.c)
target_link_libraries(duckdb_finding_arrow_scan_schema_leak PRIVATE
  abi_duckdb abi_sanitize)
add_test(NAME duckdb_finding_arrow_scan_schema_leak
         COMMAND duckdb_finding_arrow_scan_schema_leak)
# LeakSanitizer would report the very leak under test and fail the process.
set_tests_properties(duckdb_finding_arrow_scan_schema_leak PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

# The standalone reproducer for the unaligned buffer DuckDB keeps: the
# precondition of the misaligned loads the carve-out above exists for. It
# requires DuckDB's vector to be the caller's unaligned buffer itself, so it
# fails the day a pinned DuckDB copies or refuses one -- and the carve-out
# comes out with it. README.md has the UBSan report from an uncarved build.
add_executable(duckdb_finding_unaligned_int32 findings/unaligned_int32.c)
target_link_libraries(duckdb_finding_unaligned_int32 PRIVATE
  abi_duckdb abi_sanitize)
add_test(NAME duckdb_finding_unaligned_int32
         COMMAND duckdb_finding_unaligned_int32)
