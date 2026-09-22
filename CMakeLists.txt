cmake_minimum_required(VERSION 3.16)

# C++ is opt-in. The M1 adapters (Arrow C++, DuckDB) are C++, but nothing in the
# tree needs a C++ compiler today, and enabling the language unconditionally
# makes every configure fail on a host that only has a C compiler.
option(ABI_CXX "Enable C++ targets (M1 adapters)" OFF)

project(arrow_abi_doctor C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(ABI_CXX)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(ABI_WERROR      "Treat warnings as errors" OFF)
option(ABI_SANITIZERS  "Build with ASan + UBSan" OFF)
# CALLSEQ's USE_AFTER_RELEASE is a read through a released structure: genuine
# undefined behaviour in the worker's own address space. Off, the executor
# refuses the op by name. On, it performs it -- which belongs only in a worker
# behind the coordinator, ideally under a sanitizer that aborts on it, and
# never in the default CI jobs.
option(ABI_ENABLE_USE_AFTER_RELEASE "Perform CALLSEQ USE_AFTER_RELEASE" OFF)

add_library(abi_flags INTERFACE)
if(MSVC)
  target_compile_options(abi_flags INTERFACE /W4)
  if(ABI_WERROR)
    target_compile_options(abi_flags INTERFACE /WX)
  endif()
else()
  target_compile_options(abi_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wvla -Wpointer-arith -Wcast-qual
    # C-only: GCC and Clang warn that the flag itself does not apply to a C++
    # translation unit, which -Werror then promotes to an error. Guarding them
    # by language keeps one flags target usable for both.
    $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
    $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>)
  if(ABI_WERROR)
    target_compile_options(abi_flags INTERFACE -Werror)
  endif()
endif()

if(ABI_ENABLE_USE_AFTER_RELEASE)
  target_compile_definitions(abi_flags INTERFACE ABI_ENABLE_USE_AFTER_RELEASE)
endif()

if(ABI_SANITIZERS)
  # MinGW ships no sanitizer runtime; these builds run under WSL/Linux CI.
  target_compile_options(abi_flags INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(abi_flags INTERFACE -fsanitize=address,undefined)
endif()

# The compile database stays in the build tree; .clangd points the editor at it.
# Copying or symlinking it to the source root at configure time cannot work:
# CMake writes the file during *generate*, so the source does not exist yet.
# On Linux the symlink is created dangling and resolves a moment later, which is
# why this went unnoticed; on Windows, where symlink creation needs privilege,
# COPY_ON_ERROR falls back to a copy of a file that is not there and configure
# fails outright.

enable_testing()
add_subdirectory(libabi)
add_subdirectory(tools)
add_subdirectory(adapters)
