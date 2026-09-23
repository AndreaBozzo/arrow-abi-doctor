# Arrow C++, built from a pinned source release by its own CMake build.
#
# The archive is the Apache release itself, refused unless its SHA-256 is the
# one Apache publishes beside it (README.md here has how that and the release
# signature were checked). ABI_ARROW_CPP_SOURCE_DIR points at an already
# extracted tree instead, for a host without network access; it is not
# re-hashed, so whoever sets it answers for what is there.
if(WIN32)
  message(FATAL_ERROR "ABI_ARROW_CPP: the Arrow C++ worker is built on Linux only")
endif()

include(ExternalProject)

set(ABI_ARROW_CPP_VERSION "25.0.1")
set(ABI_ARROW_CPP_URL
  "https://archive.apache.org/dist/arrow/arrow-${ABI_ARROW_CPP_VERSION}/apache-arrow-${ABI_ARROW_CPP_VERSION}.tar.gz")
set(ABI_ARROW_CPP_SHA256
  "43d5de0a581f43cf63a2c06b4dcf13b9ff6fcd800f023324596e5781093bc500")
set(ABI_ARROW_CPP_SOURCE_DIR "" CACHE PATH
  "Extracted Apache Arrow source tree; the pinned release is fetched when empty")

if(ABI_ARROW_CPP_SOURCE_DIR)
  set(arrow_source SOURCE_DIR "${ABI_ARROW_CPP_SOURCE_DIR}")
else()
  set(arrow_source URL "${ABI_ARROW_CPP_URL}"
    URL_HASH SHA256=${ABI_ARROW_CPP_SHA256})
endif()

# The core library and nothing else: the C Data Interface bridge is in it, and
# every optional component is build time and third-party code that no case
# reaches. Under ABI_SANITIZERS Arrow is instrumented with this project's
# flags rather than ARROW_USE_UBSAN, which leaves out UBSan's alignment and
# vptr checks by design; and without mimalloc, which ASan cannot see into.
set(arrow_flags "")
set(arrow_link "")
if(ABI_SANITIZERS)
  set(arrow_flags
    "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer")
  set(arrow_link "-fsanitize=address,undefined")
endif()

set(arrow_install "${CMAKE_CURRENT_BINARY_DIR}/arrow_ep-install")
ExternalProject_Add(arrow_ep
  ${arrow_source}
  SOURCE_SUBDIR cpp
  INSTALL_DIR "${arrow_install}"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
    -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
    -DCMAKE_C_FLAGS=${arrow_flags}
    -DCMAKE_CXX_FLAGS=${arrow_flags}
    -DCMAKE_SHARED_LINKER_FLAGS=${arrow_link}
    -DARROW_USE_ASAN=OFF
    -DARROW_USE_UBSAN=OFF
    -DARROW_BUILD_SHARED=ON
    -DARROW_BUILD_STATIC=OFF
    -DARROW_BUILD_TESTS=OFF
    -DARROW_MIMALLOC=OFF
    -DARROW_JEMALLOC=OFF
    -DARROW_COMPUTE=OFF
    -DARROW_CSV=OFF
    -DARROW_JSON=OFF
    -DARROW_IPC=OFF
    -DARROW_FILESYSTEM=OFF
    -DARROW_WITH_UTF8PROC=OFF
    -DARROW_WITH_RE2=OFF
    -DARROW_DEPENDENCY_SOURCE=BUNDLED
  BUILD_BYPRODUCTS "${arrow_install}/lib/libarrow.so"
  USES_TERMINAL_BUILD TRUE)

file(MAKE_DIRECTORY "${arrow_install}/include")
add_library(abi_arrow_cpp SHARED IMPORTED GLOBAL)
set_target_properties(abi_arrow_cpp PROPERTIES
  IMPORTED_LOCATION "${arrow_install}/lib/libarrow.so"
  INTERFACE_INCLUDE_DIRECTORIES "${arrow_install}/include")
add_dependencies(abi_arrow_cpp arrow_ep)

add_executable(abi-worker-arrow-cpp arrow_cpp_worker.cc)
target_link_libraries(abi-worker-arrow-cpp PRIVATE
  abi_worker abi_arrow_cpp abi abi_flags)
# Arrow 25's headers need C++20; the project's default for C++ is 17.
set_target_properties(abi-worker-arrow-cpp PROPERTIES CXX_STANDARD 20)
