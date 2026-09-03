# Changelog

All notable changes are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and releases use [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [2.2.0] - 2026-09-03

### Added
- Optional AIDX block footer and compressed-domain `decompress_slice` API for C++ and Python.
- Four-segment cubic quantizer evaluation with AVX2/FMA and ARM64 Neon kernels.
- Standard-library MiniSEED-2 reader with INT16/INT32/float and Steim-1/Steim-2 decoding.
- Optional PyArrow bridge with zero-copy input views and direct-to-Arrow decompression.
- Random-access benchmark and live EarthScope benchmark provenance.

### Changed
- Python import layout is now a package so optional ecosystem bridges can live under `aetherstream`.
- Benchmark acquisition no longer depends on ObsPy.

## [2.1.0] - 2026-09-03

### Added
- Empirical exact wire-rate regulator with explicit infeasible-budget errors.
- Runtime scalar, AVX2/FMA, AVX-512, and ARM64 Neon dispatch.
- rANS-coded zero-run error residuals, sanitizer/fuzzer targets, corruption corpus, concurrency tests, aligned format specification, and wheel CI.

### Changed
- Wire format advanced to version 5 with sparse rANS tables and 64-byte records.

## [2.0.0] - 2026-09-03

### Added
- Autocorrelation-Driven Spectral Predictor (ADSP).
- Dual rate-targeted ECLM and strict absolute-error modes.
- Stateful `StreamEncoder` and `StreamDecoder` chunk APIs.
- CRC32-C protected framing and portable SIMD abstraction.

## [0.2.1] - 2026-08-20

### Fixed
- rANS renormalization overflow for single-symbol distributions.
- Decoder rejection of truncated and trailing payload data.

## [0.2.0] - 2026-08-10

### Added
- GED-informed entropy-constrained Lloyd-Max quantization.
- Sixteen-state interleaved rANS coding and Python bindings.

## [0.1.0] - 2026-07-15

### Added
- Initial C++20 predictive telemetry codec, native tests, and benchmark harness.

[Unreleased]: https://github.com/animish-sharma/aetherstream/compare/v2.2.0...HEAD
[2.2.0]: https://github.com/animish-sharma/aetherstream/releases/tag/v2.2.0
[2.1.0]: https://github.com/animish-sharma/aetherstream/releases/tag/v2.1.0
[2.0.0]: https://github.com/animish-sharma/aetherstream/releases/tag/v2.0.0
[0.2.1]: https://github.com/animish-sharma/aetherstream/releases/tag/v0.2.1
[0.2.0]: https://github.com/animish-sharma/aetherstream/releases/tag/v0.2.0
[0.1.0]: https://github.com/animish-sharma/aetherstream/releases/tag/v0.1.0
