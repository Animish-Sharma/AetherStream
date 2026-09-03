# Contributing to AetherStream

Thank you for improving AetherStream. By participating, you agree to the [Code of Conduct](CODE_OF_CONDUCT.md) and certify that your contribution may be distributed under the MIT License.

## Development setup

```bash
git clone https://github.com/animish-sharma/aetherstream.git
cd aetherstream
python3 -m venv .venv
source .venv/bin/activate            # Windows: .venv\Scripts\activate
python -m pip install --upgrade pip
python -m pip install -r requirements-dev.txt
python -m pip install -e . --no-build-isolation
cmake -S . -B build -DAETHER_BUILD_TESTS=ON -DAETHER_BUILD_PYTHON=OFF
cmake --build build --parallel 1
```

Use `AETHER_LOW_MEMORY=1` and one build job on machines with 2 GB RAM. Native optimization is opt-in and must never be used for distributable wheels.

## Before submitting

```bash
python -m ruff check .
python -m black --check .
python -m mypy benchmarks generate_assets.py
python -m pytest -v
ctest --test-dir build --output-on-failure
```

Memory-safety changes must also pass:

```bash
cmake -S . -B build-asan -DAETHER_BUILD_TESTS=ON -DAETHER_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel 1
ctest --test-dir build-asan --output-on-failure
```

## Coding standards

C++ uses C++20, RAII, explicit bounds validation at trust boundaries, and `.clang-format` (Google/LLVM-derived, 100 columns). Do not use type-punning pointer casts for wire data. SIMD changes require scalar parity tests and must preserve runtime feature checks.

Python is formatted with Black, linted with Ruff, and type-checked with mypy. Keep benchmark downloads reproducible and identify synthetic fallback data explicitly. Never insert unmeasured benchmark claims.

## Changes and tests

Create a focused branch and include tests with behavior changes. Wire-format changes require a new format version, updates to `docs/WIRE_FORMAT_SPEC.md`, malformed corpus seeds, and a compatibility note in `CHANGELOG.md`. Public APIs require Python and C++ documentation. Compression changes should report actual wire size, quality, and runtime on representative data.

Commits should explain why a change is needed. Pull requests are squash-merged after CI, review, and resolution of sanitizer findings. Maintainers may request a smaller PR when unrelated refactoring obscures correctness.

## Reporting issues

Use the structured issue forms. Security-sensitive findings must follow [SECURITY.md](SECURITY.md), not the public tracker.
