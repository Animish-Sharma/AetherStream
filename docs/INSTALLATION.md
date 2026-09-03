# Installation Matrix

## PyPI wheels

```bash
python -m pip install --upgrade aetherstream
# Optional Apache Arrow bridge
python -m pip install --upgrade "aetherstream[arrow]"
```

Release automation builds CPython 3.9–3.13 wheels for manylinux 2.28 x86-64/aarch64, macOS x86-64/arm64, and Windows AMD64. If no compatible wheel exists, pip invokes the CMake source build.

## Conda and conda-forge

When the feedstock is published:

```bash
conda install -c conda-forge aetherstream
```

Maintainers can validate the in-repository recipe with `conda build conda-recipe`. The recipe uses the low-memory profile and does not emit host-native instructions.

## Source build

```bash
git clone https://github.com/animish-sharma/aetherstream.git
cd aetherstream
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
AETHER_LOW_MEMORY=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
  python -m pip install . --no-build-isolation
```

Set `AETHER_NATIVE_OPTIMIZED=1` only for a local artifact that will run on the build host. For a final fallback, configure CMake with `-DAETHER_LOW_MEMORY=ON`, one build job, and `CXXFLAGS=-O0`.

## CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(aetherstream
  GIT_REPOSITORY https://github.com/animish-sharma/aetherstream.git
  GIT_TAG v2.2.0)
set(AETHER_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(aetherstream)
target_link_libraries(my_app PRIVATE aether::aether_core)
```

## Installed CMake package

```bash
cmake -S . -B build -DAETHER_BUILD_PYTHON=OFF -DAETHER_BUILD_TESTS=OFF
cmake --build build --parallel 1
cmake --install build --prefix "$HOME/.local"
```

Consumers use `find_package(AetherStream 2.2 REQUIRED CONFIG)` and link `aether::aether_core`.

## vcpkg

Use the repository as an overlay port:

```bash
vcpkg install aetherstream --overlay-ports=ports
```

Enable host-native code only for local deployments with `aetherstream[native]`.

## Docker

```bash
docker build -t aetherstream:2.2 .
docker run --rm aetherstream:2.2
```

The build stage uses Clang 16, Ninja, Python 3.11, ASan/UBSan tests, the synthetic benchmark, and deterministic SVG generation. The runtime stage contains only Python and the built wheel.

## Platform notes

| Platform | Toolchain | SIMD selection |
|---|---|---|
| Linux x86-64 | GCC 11+ or Clang 14+ | Runtime AVX-512, AVX2/FMA, scalar |
| Linux aarch64 | GCC 11+ or Clang 14+ | Runtime Neon/scalar via `getauxval` |
| macOS Intel | Apple Clang | Runtime AVX2 when available |
| macOS Apple Silicon | Apple Clang | Neon with `sysctlbyname` capability check |
| Windows AMD64 | MSVC 2022 | Scalar wheel baseline; AVX2 local native profile |

Set `AETHER_SIMD=scalar` to diagnose numerical or virtual-machine CPU-feature issues.
