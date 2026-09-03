# AetherStream

AetherStream is a C++20 and Python codec for finite, one-dimensional `float32` telemetry. It offers an exact serialized-rate mode and a pointwise absolute-error mode, block-adaptive spectral prediction, optional indexed random access, CRC32-C integrity, SIMD cubic quantization, and direct Apache Arrow buffer integration.

![Compression architecture](https://raw.githubusercontent.com/animish-sharma/aetherstream/main/assets/architecture_pipeline.svg)

## Start here

- Follow the [installation matrix](INSTALLATION.md).
- Read the [Python and C++ API reference](API_REFERENCE.md).
- Treat the [wire-format specification](WIRE_FORMAT_SPEC.md) as normative for interoperability.
- Run `python benchmarks/bench_rigorous.py` before making hardware-specific performance claims.

## Safety contract

Input must be one-dimensional and finite. The decoder validates framing, arithmetic bounds, entropy tables, final rANS states, CRC, and zero padding before exposing reconstructed data. Very short inputs may have an infeasible low-rate budget because no non-empty serialized stream can fit below its fixed framing cost; rate mode reports `RateBudgetError` rather than violating the requested rate.
