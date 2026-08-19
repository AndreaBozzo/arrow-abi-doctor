# Big-endian cross target for the M0 cross-architecture check.
#
# s390x is the most convenient big-endian target with a packaged Debian/Ubuntu
# cross toolchain (gcc-s390x-linux-gnu) and qemu-user support. Built static so
# qemu-user needs no sysroot loader.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR s390x)

set(CMAKE_C_COMPILER s390x-linux-gnu-gcc)
set(CMAKE_C_FLAGS_INIT "-static")

set(CMAKE_FIND_ROOT_PATH /usr/s390x-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
