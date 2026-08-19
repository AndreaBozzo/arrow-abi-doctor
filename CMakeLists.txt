cmake_minimum_required(VERSION 3.16)
project(arrow_abi_doctor C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ABI_WERROR      "Treat warnings as errors" OFF)
option(ABI_SANITIZERS  "Build with ASan + UBSan" OFF)

add_library(abi_flags INTERFACE)
if(MSVC)
  target_compile_options(abi_flags INTERFACE /W4)
  if(ABI_WERROR)
    target_compile_options(abi_flags INTERFACE /WX)
  endif()
else()
  target_compile_options(abi_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wvla -Wpointer-arith
    -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual)
  if(ABI_WERROR)
    target_compile_options(abi_flags INTERFACE -Werror)
  endif()
endif()

if(ABI_SANITIZERS)
  # MinGW ships no sanitizer runtime; these builds run under WSL/Linux CI.
  target_compile_options(abi_flags INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(abi_flags INTERFACE -fsanitize=address,undefined)
endif()

enable_testing()
add_subdirectory(libabi)
add_subdirectory(tools)
