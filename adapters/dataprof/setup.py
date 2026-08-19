"""Builds the _abicase extension: a .abicase as an Arrow PyCapsule producer.

libabi sources are compiled straight into the extension rather than linked from
a static library, so this builds with nothing but a C compiler and setuptools
and does not need CMake to have run first.
"""

import os

from setuptools import Extension, setup

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
LIBABI = os.path.join(REPO, "libabi")

LIBABI_SOURCES = [
    "arena.c",
    "bytes.c",
    "sha256.c",
    "fill.c",
    "case.c",
    "validate.c",
    "encode.c",
    "decode.c",
    "reconstruct.c",
]

sources = (
    [os.path.join(HERE, "abicase_py.c")]
    + [os.path.join(LIBABI, "src", name) for name in LIBABI_SOURCES]
    + [os.path.join(LIBABI, "tests", "fixture.c")]
)

missing = [s for s in sources if not os.path.exists(s)]
if missing:
    raise SystemExit("missing sources:\n  " + "\n  ".join(missing))

setup(
    name="abicase-dataprof-adapter",
    version="0.1.0",
    description="Present a .abicase to a Python Arrow consumer (M0.5 adapter)",
    ext_modules=[
        Extension(
            "_abicase",
            sources=sources,
            include_dirs=[
                os.path.join(LIBABI, "include"),
                os.path.join(LIBABI, "src"),
                os.path.join(LIBABI, "tests"),
            ],
            extra_compile_args=["-std=c11", "-Wall", "-Wextra"],
        )
    ],
)
