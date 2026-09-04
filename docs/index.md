# AetherStream

AetherStream compresses finite, one-dimensional `float32` sensor data from C++20 or Python. Choose a maximum serialized rate or a maximum pointwise reconstruction error. Optional block indexes allow random-access slices, and CRC32-C checks detect damaged data. Apache Arrow support is optional.

![Compression architecture](https://raw.githubusercontent.com/animish-sharma/aetherstream/main/assets/architecture_pipeline.svg)

## Start here

- Follow the [installation matrix](INSTALLATION.md).
- Read the [Python and C++ API reference](API_REFERENCE.md).
- Treat the [wire-format specification](WIRE_FORMAT_SPEC.md) as normative for interoperability.
- Run `python scripts/verify_release.py --full` before making release or hardware-specific performance claims.

## Safety contract

Input must be one-dimensional and contain no NaN or infinite values. The decoder checks lengths, arithmetic bounds, entropy tables, final rANS states, checksums, and padding before returning samples. A very short input might not fit a low rate because every non-empty stream has a fixed header. In that case, rate mode raises `RateBudgetError` instead of exceeding the requested rate.
