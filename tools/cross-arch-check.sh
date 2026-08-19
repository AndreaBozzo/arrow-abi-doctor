#!/usr/bin/env bash
#
# M0 cross-architecture verification.
#
# The .abicase format claims to be portable between hosts of differing
# endianness. That claim is worth nothing until a big-endian machine actually
# runs the code, so this script builds libabi for s390x (big-endian) alongside
# the native little-endian build and checks four things:
#
#   1. the full test suite passes on the big-endian host
#   2. both hosts encode the same case to the same bytes
#   3. each host decodes and re-encodes the other host's file byte-identically
#   4. both hosts report the same structure and topology for the same file
#
# Check 2 is the one that catches a native integer serialized by accident: such
# a bug is invisible on x86 and passes every little-endian test ever written.
#
# Requires: gcc, cmake, ninja, gcc-s390x-linux-gnu, qemu-user-static.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="${TMPDIR:-/tmp}/abicase-cross-$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

native_dir="$root/build/native"
cross_dir="$root/build/s390x"

step() { printf '\n=== %s ===\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }

for tool in cmake ninja s390x-linux-gnu-gcc qemu-s390x-static; do
  command -v "$tool" >/dev/null 2>&1 || fail "missing required tool: $tool"
done

step "building native ($(uname -m), little-endian)"
cmake -S "$root" -B "$native_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DABI_WERROR=ON >/dev/null
cmake --build "$native_dir" >/dev/null
echo "ok"

step "building s390x (big-endian, cross)"
cmake -S "$root" -B "$cross_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$root/cmake/s390x-linux-gnu.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DABI_WERROR=ON >/dev/null
cmake --build "$cross_dir" >/dev/null
echo "ok"

native_cli="$native_dir/tools/abicase"
cross_cli="qemu-s390x-static $cross_dir/tools/abicase"

# Confirm the cross binary really is big-endian rather than a silently native
# build: an ELF header check is cheap and the whole exercise is void without it.
if ! file "$cross_dir/tools/abicase" | grep -q 'MSB'; then
  fail "cross binary is not big-endian (MSB); toolchain file not applied?"
fi
echo "cross binary: $(file -b "$cross_dir/tools/abicase" | cut -d, -f1-2)"

step "1. test suite on the big-endian host"
qemu-s390x-static "$cross_dir/libabi/abicase_tests" | tail -3
echo "ok"

step "2. both hosts encode the same case to the same bytes"
$native_cli selftest -o "$work/native.abicase" | sed 's/^/  native: /'
# shellcheck disable=SC2086
$cross_cli selftest -o "$work/s390x.abicase" | sed 's/^/  s390x:  /'
cmp "$work/native.abicase" "$work/s390x.abicase" \
  || fail "encoded bytes differ between architectures"
echo "ok: byte-identical encoding across endianness"

step "3. each host replays the other host's file"
$native_cli verify "$work/s390x.abicase" | sed 's/^/  native reads s390x: /'
# shellcheck disable=SC2086
$cross_cli verify "$work/native.abicase" | sed 's/^/  s390x reads native: /'
echo "ok"

step "4. structure and topology agree"
$native_cli dump "$work/native.abicase" > "$work/native.dump"
# shellcheck disable=SC2086
$cross_cli dump "$work/native.abicase" > "$work/s390x.dump"
if ! diff -u "$work/native.dump" "$work/s390x.dump" > "$work/dump.diff"; then
  cat "$work/dump.diff"
  fail "structural/topological dumps differ between architectures"
fi
echo "ok: identical dump ($(wc -l < "$work/native.dump") lines)"
grep -c 'ALIASED' "$work/native.dump" | sed 's/^/  aliased allocations: /'

step "also: the committed golden fixture replays on big-endian"
# shellcheck disable=SC2086
$cross_cli verify "$root/libabi/tests/fixtures/rich-v1.abicase" \
  | sed 's/^/  /'

printf '\nM0 cross-architecture check: PASS\n'
