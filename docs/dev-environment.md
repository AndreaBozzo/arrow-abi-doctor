# Development environment

Four languages, eventually: **C** today (`libabi`, `tools`, the adapter
extension), **C++** for the M1 Arrow C++ and DuckDB adapters, **Python** for the
adapter harnesses, **Rust** for the coordinator and CLI (M1) and the arrow-rs
adapter (M3). Configuration for all four is in the repo; only C and Python have
anything to build yet.

The short version: **Windows builds and runs everything except sanitizers,
Valgrind and big-endian.** Those three need WSL, and they are not optional
extras — the truncation and corruption sweeps mean nothing without a sanitizer,
and the cross-architecture check is the M0 gate.

---

## What lives where

| Capability | Windows | WSL (Ubuntu 24.04) |
|---|---|---|
| `libabi` + `abicase` CLI | MinGW-w64 UCRT, or MSVC | gcc |
| CPython adapter extension | MSVC (see below) | gcc |
| ASan / UBSan | no runtime shipped | yes |
| Valgrind | not available | yes |
| big-endian (s390x) | no | `gcc-s390x-linux-gnu` + `qemu-user-static` |
| ruff / mypy / clang-format | `.venv` | either |
| Rust | stable-msvc, links fine | stable-gnu |

Both hosts see the same checkout: `C:/dev/arrow-abi-doctor` is
`/mnt/c/dev/arrow-abi-doctor` under WSL. Nothing is copied, so a build directory
written by one is visible to the other — which is why they use separate ones.

### Three directories called /tmp

Git Bash `/tmp`, WSL `/tmp` and Windows Python's `%TEMP%` are three different
places, and a Windows binary handed a Git Bash `/tmp/...` path simply cannot open
it. WSL `/tmp` also does not survive between `wsl -e` invocations. For anything
that crosses environments, use a native absolute path.

Keep `CARGO_TARGET_DIR` and any WSL build tree **outside** the checkout, so Linux
artifacts never land in the Windows working tree.

---

## Toolchain configuration in the repo

| File | What it fixes |
|---|---|
| `.clang-format` | C style; applied across the tree once, gated in CI |
| `.clang-format-ignore` | exempts `arrow_abi.h`, which is vendored verbatim |
| `.clangd` | points the editor at `build/`'s compile database |
| `.editorconfig` | LF, indent width, per-language line length |
| `pyproject.toml` | ruff, mypy, and the pinned `dev` dependency group |
| `rust-toolchain.toml` | channel plus `rustfmt` and `clippy`, ahead of any crate |
| `rustfmt.toml` | edition 2024, `max_width = 100`, matching dataprof |

**Formatter versions are pinned exactly** in `pyproject.toml`'s `dev` group.
clang-format's output changes between releases, so an unpinned version turns the
CI format gate into a tripwire that fires when a runner image updates instead of
when someone changes code.

---

## Setting up

### Python tooling (either host)

```sh
python -m venv .venv
.venv/Scripts/python -m pip install --group dev     # Windows
./.venv/bin/python   -m pip install --group dev     # Linux
```

`--group` needs pip >= 25.1. This venv is for lint and format only; the smoke
harness needs consumers, below.

### The smoke harness (WSL)

`~/abienv` is the venv that persists: pyarrow, dataprof, maturin. Everything else
is throwaway and gets cleaned up — the before/after dataprof venvs and their
`CARGO_TARGET_DIR` came to about 4.9 GB.

```sh
python3 -m venv ~/dpX && ~/dpX/bin/pip install pyarrow setuptools
cd /mnt/c/dev/dataprof                    # or a worktree at the commit under test
CARGO_TARGET_DIR=$HOME/dp-target ~/abienv/bin/maturin build --release \
    --interpreter ~/dpX/bin/python --out ~/dp-wheels
~/dpX/bin/pip install --force-reinstall --no-deps ~/dp-wheels/*.whl
```

Cold build of the dataprof workspace is about five minutes, warm about twenty
seconds.

### MSVC on this machine needs one flag

The extension builds on Windows, but the local Visual Studio Build Tools 2022
install has incomplete installer metadata: `cl.exe`, `link.exe` and the Windows
SDK are all present, while `vswhere -property packages` reports nothing and
`-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64` matches no
installation. setuptools probes exactly that way, so it concludes there is no
compiler and stops with "Unable to find a compatible Visual Studio installation."

Run `vcvars64.bat` first and tell setuptools to trust the environment:

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set DISTUTILS_USE_SDK=1
set MSSdk=1
cd adapters\dataprof
..\..\.venv\Scripts\python setup.py build_ext --inplace
```

CI does not need this: the `windows-latest` image registers its components
properly. Repairing the local install through the Visual Studio Installer would
also fix it.

---

## The local verification matrix

What CI runs, and how to run each part by hand.

```sh
# C, Windows or Linux
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DABI_WERROR=ON
cmake --build build && ctest --test-dir build --output-on-failure

# sanitizers (WSL)
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DABI_SANITIZERS=ON -DABI_WERROR=ON
cmake --build build/asan && ctest --test-dir build/asan --output-on-failure

# Valgrind (WSL)
valgrind --error-exitcode=9 --leak-check=full --show-leak-kinds=all \
         --errors-for-leak-kinds=all -q ./build/libabi/abicase_tests

# big-endian (WSL)
./tools/cross-arch-check.sh

# adapter smoke, needs pyarrow and dataprof
cd adapters/dataprof && python setup.py build_ext --inplace && python run_smoke.py
python repro_findings.py          # regression checks against two fixed dataprof bugs

# lint and format
ruff check . && ruff format --check . && mypy .
clang-format --dry-run --Werror libabi/src/*.c libabi/src/*.h \
    libabi/include/abi/*.h libabi/tests/*.c libabi/tests/*.h \
    tools/*.c adapters/dataprof/*.c
```

`-DABI_CXX=ON` enables the C++ language for the M1 adapters. It is off by
default so that a host with only a C compiler can still configure.

---

## Things that have cost time here

- **The compile database cannot be linked at configure time.** CMake writes
  `compile_commands.json` during *generate*, so a `file(CREATE_LINK ...)` at
  configure time has no source to link. On Linux the symlink is created dangling
  and resolves moments later; on Windows, where symlink creation needs
  privilege, the `COPY_ON_ERROR` fallback copies a file that is not there and
  configure fails outright. `.clangd` points at the build tree instead.
- **A quoted heredoc still strips one backslash level** when a script reads it
  on stdin, so a patch pattern containing an escape never matches and reports
  itself as "pattern not found". Use an exact-match editor for those, and always
  assert the replacement count: a bulk edit that does not count its matches
  reports success on zero replacements.
- **Line endings differ between this repo and dataprof.** `.gitattributes` here
  sets `* text=auto eol=lf`; dataprof has none, so `core.autocrlf=true` checks
  its tree out CRLF and multi-line patterns written with LF never match.
- **A git worktree of a Windows repo does not resolve from WSL.** Its `.git`
  file points at a `C:/` path git cannot follow from the other side, so commands
  needing repo metadata fail there even though the files are readable.
