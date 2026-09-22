"""Builds the _abicase extension: a .abicase as an Arrow PyCapsule producer.

libabi sources are compiled straight into the extension rather than linked from
a static library, so this builds with nothing but a C compiler and setuptools
and does not need CMake to have run first.
"""

import os

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

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
    "digest.c",
]

sources = (
    [os.path.join(HERE, "abicase_py.c")]
    + [os.path.join(LIBABI, "src", name) for name in LIBABI_SOURCES]
    + [os.path.join(LIBABI, "tests", "fixture.c")]
)

missing = [s for s in sources if not os.path.exists(s)]
if missing:
    raise SystemExit("missing sources:\n  " + "\n  ".join(missing))

FLAGS = {
    # cl.exe does not understand -std=c11 and only warns (D9002) that it is
    # ignoring an unknown option, so passing the GCC flags on Windows produces a
    # build that silently compiles as C89-with-extensions. libabi is C11 and
    # relies on it, so the flags have to be chosen per compiler rather than
    # handed over and hoped for.
    # _CRT_SECURE_NO_WARNINGS: MSVC deprecates fopen in favour of fopen_s, which
    # is Annex K and not portably available. The call stays standard C11 and the
    # warning would otherwise be the only noise in an otherwise clean build.
    "msvc": ["/std:c11", "/W3", "/D_CRT_SECURE_NO_WARNINGS"],
    "unix": ["-std=c11", "-Wall", "-Wextra"],
}


class BuildExt(build_ext):
    def build_extensions(self):
        flags = FLAGS.get(self.compiler.compiler_type, FLAGS["unix"])
        for ext in self.extensions:
            ext.extra_compile_args = flags
        super().build_extensions()


setup(
    name="abicase-dataprof-adapter",
    version="0.1.0",
    description="Present a .abicase to a Python Arrow consumer (M0.5 adapter)",
    cmdclass={"build_ext": BuildExt},
    ext_modules=[
        Extension(
            "_abicase",
            sources=sources,
            include_dirs=[
                os.path.join(LIBABI, "include"),
                os.path.join(LIBABI, "src"),
                os.path.join(LIBABI, "tests"),
            ],
        )
    ],
)
