<div align="center">

# ⚡ AetherStream

**Production-oriented compression for streaming float32 telemetry**

[![PyPI](https://img.shields.io/pypi/v/aetherstream.svg?color=2563eb)](https://pypi.org/project/aetherstream/)
[![CI and Sanitizers](https://github.com/animish-sharma/aetherstream/actions/workflows/ci.yml/badge.svg)](https://github.com/animish-sharma/aetherstream/actions/workflows/ci.yml)
[![Wheels](https://github.com/animish-sharma/aetherstream/actions/workflows/wheels.yml/badge.svg)](https://github.com/animish-sharma/aetherstream/actions/workflows/wheels.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-facc15.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-38bdf8.svg)](https://en.cppreference.com/w/cpp/20)
[![Python 3.9–3.13](https://img.shields.io/badge/python-3.9%E2%80%933.13-a78bfa.svg)](https://pypi.org/project/aetherstream/)

[Quickstart](#quickstart) · [Installation](#installation) · [Features](#key-features) · [Benchmarks](#benchmarks) · [Theory](#mathematical-foundation) · [C++ API](#c-api) · [Troubleshooting](#troubleshooting)

![AetherStream compression pipeline](assets/architecture_pipeline.svg)

</div>

---

## Overview

AetherStream is an open-source C++20 and Python codec for finite, one-dimensional `float32` telemetry: seismic monitoring, strain gauges, rotating machinery, industrial sensors, and constrained edge links.

It combines an **Autocorrelation-Driven Spectral Predictor (ADSP)**, online **Generalized Error Distribution (GED)** modeling, dead-zone **Entropy-Constrained Lloyd–Max (ECLM)** quantization, and a **16-state interleaved rANS** coder. Applications select either an enforced serialized-rate budget or a deterministic pointwise absolute-error bound. Stateful encoders accept micro-batches while preserving block-equivalent reconstruction.

AetherStream does not publish hardware-independent throughput promises. Run the reproducible benchmark on deployment hardware and report its generated provenance with performance claims.

## Key features

- **Dual modes**
  - Rate-targeted: empirical re-encoding accounts for metadata, codebooks, entropy tables, CRC, and alignment. An infeasible short stream raises `RateBudgetError` instead of exceeding its budget.
  - Error-bounded: enforces `|x[t] - reconstructed[t]| ≤ tolerance` up to documented float32 rounding tolerance; zero runs and non-zero quanta are rANS-coded.
- **ADSP prediction:** selects constant, harmonic, or linear prediction for each 2048-sample block.
- **Closed-loop state:** encoder and decoder predict from reconstructed history, preventing integrated predictor drift.
- **Indexed random access:** optional AIDX block tables let `decompress_slice()` seek to and decode only intersecting blocks without copying the compressed stream.
- **Runtime SIMD dispatch:** scalar, x86 AVX2/FMA, x86 AVX-512, and ARM64 Neon implementations, including a four-segment cubic quantizer evaluator.
- **Arrow interoperability:** optional PyArrow input views are compressed without copying primitive values, and decoding writes directly into Arrow-owned buffers.
- **Dependency-free MiniSEED reader:** benchmark acquisition decodes EarthScope Steim-1/Steim-2 data without ObsPy.
- **Incremental framing:** `StreamEncoder` and `StreamDecoder` handle sample micro-batches and arbitrary byte fragmentation.
- **Defensive decoding:** little-endian wire format, 64-byte block alignment, CRC32-C, bounded allocation, sparse frequency-table validation, and final rANS-state checks.
- **Operational tooling:** ASan/UBSan, libFuzzer, malformed corpus, 16-thread stress tests, CMake exports, PyPI wheels, Conda/vcpkg recipes, and a reproducible container.

![Closed-loop predictor behavior](assets/spectral_predictor.svg)

## Quickstart

### Python

```bash
python -m pip install --upgrade aetherstream
```

```python
import numpy as np
import aetherstream as aether

samples = np.sin(np.linspace(0, 100, 100_000, dtype=np.float32))

# Exact serialized-rate mode. May reject very short infeasible inputs.
compressed = aether.compress(samples, target_rate=3.0)
reconstructed = aether.decompress(compressed, length=len(samples))

# Pointwise absolute-error mode.
bounded = aether.compress(samples, absolute_error=0.005)
bounded_reconstruction = aether.decompress(bounded, length=len(samples))
assert np.max(np.abs(samples - bounded_reconstruction)) <= 0.005001

# Optional block index for compressed-domain slicing.
indexed = aether.compress(samples, target_rate=3.0, enable_index=True)
window = aether.decompress_slice(indexed, start=40_000, count=512)
```

### Real-time packet streaming

```python
encoder = aether.StreamEncoder(target_rate=3.0)
decoder = aether.StreamDecoder()

for packet in sensor_packets:       # one-dimensional float32-compatible arrays
    wire = encoder.feed(packet)
    if wire:
        restored = decoder.feed(wire)

final_wire = encoder.flush()
if final_wire:
    final_samples = decoder.feed(final_wire)
decoder.flush()                     # rejects an incomplete final byte frame
```

## Installation

Complete commands and platform caveats are in the **[installation matrix](docs/INSTALLATION.md)**.

### PyPI wheels

```bash
python -m pip install aetherstream
```

The wheel matrix targets CPython 3.9–3.13, manylinux 2.28 x86-64/aarch64, macOS x86-64/arm64, and Windows AMD64.

### Low-memory source build

```bash
git clone https://github.com/animish-sharma/aetherstream.git
cd aetherstream
python3 -m venv .venv && source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install scikit-build-core pybind11 numpy
AETHER_LOW_MEMORY=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
  python -m pip install -e . --no-build-isolation
```

The low-memory profile uses `-O1 -g0`, disables LTO/native code generation, and limits compilation to one job. For a local host-specific build:

```bash
AETHER_NATIVE_OPTIMIZED=1 CMAKE_BUILD_PARALLEL_LEVEL=4 python -m pip install -e .
```

Never distribute a host-native wheel.

### Conda

```bash
conda install -c conda-forge aetherstream
```

Until the feedstock is accepted, build the included recipe with `conda build conda-recipe`.

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(aetherstream
  GIT_REPOSITORY https://github.com/animish-sharma/aetherstream.git
  GIT_TAG v2.2.0)
set(AETHER_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(aetherstream)
target_link_libraries(your_application PRIVATE aether::aether_core)
```

### System CMake install

```bash
cmake -S . -B build -DAETHER_BUILD_PYTHON=OFF -DAETHER_BUILD_TESTS=OFF
cmake --build build --parallel 1
sudo cmake --install build
```

Consumers can then use `find_package(AetherStream 2.2 REQUIRED CONFIG)` and `aether::aether_core`.

### vcpkg and Docker

```bash
vcpkg install aetherstream --overlay-ports=ports
docker build -t aetherstream:2.2 .
docker run --rm aetherstream:2.2
```

## C++ API

```cpp
#include <aether/aether.hpp>
#include <aether/table.hpp>

#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::vector<float> telemetry = acquire_finite_float32_samples();

    aether::CodecConfig config;
    config.target_rate = 3.0f;
    config.absolute_error_bound = 0.0f;
    config.deadzone_factor = 0.3f;
    config.enable_index = true;

    const aether::AetherCodec codec(config);
    const std::vector<uint8_t> wire = codec.compress(telemetry);

    std::vector<float> restored(telemetry.size());
    codec.decompress(wire, restored);
    std::vector<float> window(512);
    aether::decompress_slice(wire, 4096, window.size(), window);
    std::cout << telemetry.size() * sizeof(float) << " input bytes -> "
              << wire.size() << " wire bytes\n";
}
```

See the **[API reference](docs/API_REFERENCE.md)** for exceptions, ownership, and streaming semantics.

## Mathematical foundation

### Closed-loop stability

Let `e[t] = x[t] - prediction[t]`, and define quantization error `q[t] = reconstructed_residual[t] - e[t]`. Because both sides update predictor state from the same reconstructed history,

```math
x[t] - \widetilde{x}[t] = e[t] - \widetilde{e}[t] = -q[t].
```

The reconstruction error is instantaneous rather than recursively integrated.

### GED moment inversion

The block shape parameter is recovered from

```math
R(\beta)=\frac{m_1^2}{m_2}
=\frac{\Gamma(2/\beta)^2}{\Gamma(1/\beta)\Gamma(3/\beta)}.
```

### Entropy-constrained boundaries

```math
b_i=\frac{y_i+y_{i+1}}{2}
+\frac{\lambda}{2(y_{i+1}-y_i)}\log_2\!\left(\frac{p_i}{p_{i+1}}\right).
```

The analytical entropy target initializes the codebook; serialized-byte measurements close the rate-control loop.

## Benchmarks

![Illustrative rate-distortion chart](assets/rate_distortion_curve.svg)

The SVG above is labelled illustrative unless generated from benchmark output. It is not evidence for a throughput claim.

```bash
python benchmarks/fetch_usgs_data.py --strict
python benchmarks/bench_rigorous.py
python benchmarks/bench_random_access.py
```

The fetcher uses `urllib.request` and an in-tree MiniSEED-2/Steim decoder; ObsPy is not required. The rigorous suite records data provenance and compares AetherStream with Zstandard raw/shuffled modes, uniform Lloyd–Max, and the official SZ3 executable when installed. It writes:

- `benchmarks/results/benchmark_results.csv`
- `benchmarks/results/benchmark_results.json`
- `benchmarks/results/pareto_frontier.png`
- `benchmarks/results/random_access.json`

Report CPU, compiler, SIMD backend, source trace, sample count, and command line with results. Synthetic fallback traces are marked as synthetic in `benchmarks/data/manifest.json`.

## Testing and security

```bash
python -m pytest -v
cmake -S . -B build -DAETHER_BUILD_TESTS=ON -DAETHER_BUILD_PYTHON=OFF
cmake --build build --parallel 1
ctest --test-dir build --output-on-failure
```

Sanitizer and fuzzing procedures are documented in [CONTRIBUTING.md](CONTRIBUTING.md). Report vulnerabilities privately according to [SECURITY.md](SECURITY.md). The normative interoperability contract is [Wire Format v2.2](docs/WIRE_FORMAT_SPEC.md).

## Troubleshooting

<details>
<summary><strong>The compiler was killed or ran out of memory</strong></summary>

Use `AETHER_LOW_MEMORY=1 CMAKE_BUILD_PARALLEL_LEVEL=1`. If necessary, add `CXXFLAGS=-O0`. Avoid build isolation on a constrained host after installing build dependencies.
</details>

<details>
<summary><strong>RateBudgetError on a short input</strong></summary>

A non-empty stream has fixed framing and integrity bytes. No codec can serialize below that minimum. Buffer more samples, use `StreamEncoder`, raise the rate, or select absolute-error mode.
</details>

<details>
<summary><strong>NaN or infinity is rejected</strong></summary>

Non-finite values violate predictor and GED assumptions. Preserve a separate validity mask and impute finite values before compression.
</details>

<details>
<summary><strong>Unexpected SIMD behavior in a VM</strong></summary>

Set `AETHER_SIMD=scalar` and rerun parity tests. Verify that the hypervisor exposes OSXSAVE and the advertised instruction set consistently.
</details>

## Governance, license, and citation

Contributions follow [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md). AetherStream is distributed under the [MIT License](LICENSE).

```bibtex
@software{sharma2026aetherstream,
  author  = {Animish Sharma and AetherStream contributors},
  title   = {AetherStream: Streaming Telemetry Compression},
  year    = {2026},
  version = {2.2.0},
  url     = {https://github.com/animish-sharma/aetherstream}
}
```
