import os
import platform

import pybind11
from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

sources = [
    "src/bindings.cpp",
    "src/common.cpp",
    "src/predictor.cpp",
    "src/ged_estimator.cpp",
    "src/eclm_quantizer.cpp",
    "src/vrans_codec.cpp",
    "src/stream_encoder.cpp",
    "src/table.cpp",
]


def compile_args():
    """Return compiler flags that fit small build machines by default.

    The default profile is intended to build in about 2GB RAM: one compiler job,
    low optimization, no debug info, no LTO, and no native/AVX-512 codegen.
    Opt in to the aggressive profile with AETHER_NATIVE_OPTIMIZED=1 on a larger
    machine.  Override the low-memory optimization level with
    AETHER_BUILD_OPT_LEVEL, for example AETHER_BUILD_OPT_LEVEL=-O2.
    """
    args = ["-std=c++20", "-g0", "-fno-lto"]
    if os.environ.get("AETHER_NATIVE_OPTIMIZED") == "1":
        native_flag = (
            "-mcpu=native"
            if platform.machine().lower() in {"aarch64", "arm64"}
            else "-march=native"
        )
        args.extend(["-O3", "-ffast-math", native_flag])
    else:
        args.append(os.environ.get("AETHER_BUILD_OPT_LEVEL", "-O1"))
    return args


class LowMemoryBuildExt(build_ext):
    def finalize_options(self):
        super().finalize_options()
        # Prevent accidental high parallelism from pip/config-settings from
        # compiling several C++ translation units at once on low-RAM systems.
        if os.environ.get("AETHER_PARALLEL_BUILD") is None:
            self.parallel = 1
        else:
            self.parallel = int(os.environ["AETHER_PARALLEL_BUILD"])

    def build_extensions(self):
        if self.compiler.compiler_type == "msvc":
            native = os.environ.get("AETHER_NATIVE_OPTIMIZED") == "1"
            flags = ["/std:c++20", "/O2" if native else "/O1"]
            if not native:
                flags.append("/GL-")
            if native and platform.machine().lower() in {"amd64", "x86_64"}:
                flags.append("/arch:AVX2")
            for extension in self.extensions:
                extension.extra_compile_args = flags
        super().build_extensions()


extension = Extension(
    "aether",
    sources,
    include_dirs=["include", pybind11.get_include()],
    language="c++",
    extra_compile_args=compile_args(),
)

setup(
    name="aetherstream",
    version="0.0.1",
    description="Dual-mode streaming telemetry compression with strict error and wire-rate bounds",
    long_description=open("README.md", encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    license="MIT",
    python_requires=">=3.9",
    extras_require={"arrow": ["pyarrow>=14"]},
    packages=["aetherstream"],
    package_dir={"": "python"},
    ext_modules=[extension],
    cmdclass={"build_ext": LowMemoryBuildExt},
)
