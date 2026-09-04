# Changelog

All notable changes are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and releases use [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.0.1] - 2026-09-04

### Added
- C++20 and Python APIs for rate-targeted and absolute-error-bounded telemetry compression.
- Incremental stream encoding and decoding with CRC32-C validation.
- Wire format v6 with aligned block records and an optional CRC-protected random-access index.
- Cached indexed slicing through `IndexedStreamView`.
- Runtime scalar, AVX2/FMA, AVX-512, and ARM64 Neon dispatch.
- Six-segment quantizer evaluation with exact dead-zone and tail handling.
- Adaptive codebooks for impulsive, low-beta residuals.
- Optional Apache Arrow integration and a dependency-free MiniSEED-2 reader.
- Native, Python, sanitizer, concurrency, malformed-input, fuzz, packaging, and benchmark tooling.
- CMake exports, wheels, Conda and vcpkg recipes, and a reproducible container build.

### Compatibility
- Unindexed wire-v5 streams remain readable.
- Indexed wire-v5 streams require their matching development decoder before re-encoding as wire v6.
- Wire versions below 5 and above 6 are rejected.

[Unreleased]: https://github.com/animish-sharma/aetherstream/compare/v0.0.1...HEAD
[0.0.1]: https://github.com/animish-sharma/aetherstream/releases/tag/v0.0.1
