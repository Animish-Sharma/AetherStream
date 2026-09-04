# API Reference

## Python

The supported package import is `aetherstream`; `aether` is the native implementation module.

### `compress(samples, target_rate=4.0, absolute_error=0.0, deadzone=0.3, enable_index=False, adaptive_tail=True) -> bytes`

Compresses a contiguous or convertible one-dimensional float32 array. Set
`enable_index=True` to append the validated block index required by
`decompress_slice`. `adaptive_tail=True` enables the low-beta impulsive-tail partition in rate mode. `absolute_error > 0` selects strict error mode; otherwise rate mode is used. Raises `ValueError` for non-finite/multidimensional input, `RateBudgetError` when fixed framing cannot satisfy a short rate budget, and standard allocation exceptions on resource exhaustion.

### `decompress(data, length) -> numpy.ndarray`

Decodes one complete frame into a float32 array and verifies that `length` equals the embedded sample count. Malformed input raises `CorruptedStreamError`.

### `decompress_slice(data, start, count) -> numpy.ndarray`

Binary-searches an indexed frame and entropy-decodes only blocks intersecting
the requested sample range. The returned values match the same range from full
`decompress` bit-for-bit. The compressed byte string is borrowed rather than
copied while native decoding runs.

### `IndexedStreamView(data)`

Caches one-time stream-header, footer-CRC, and index-semantic validation while
holding a reference to the Python `bytes` object. Properties `sample_count` and
`block_count` expose validated metadata. Repeated
`view.decompress_slice(start, count)` calls perform O(log N-blocks) lookup and
validate/decode only intersecting block records.

### Arrow bridge

`aetherstream.arrow.compress_arrow_array(array, target_rate=3.0)` accepts a
null-free Arrow float32 `Array` or `ChunkedArray` and returns a `pyarrow.Buffer`.
`decompress_arrow_buffer(buffer, length)` writes directly into an Arrow-owned
value allocation and returns a float32 `PrimitiveArray`. PyArrow is optional.

### `StreamEncoder(target_rate=4.0, absolute_error=0.0, deadzone=0.3)`

`feed(chunk) -> bytes` buffers samples and emits zero or more complete 2048-sample frames. `flush() -> bytes` emits a non-empty tail when its selected mode is representable. Instances are not internally synchronized; use one instance per stream.

### `StreamDecoder()`

`feed(data) -> numpy.ndarray` accepts arbitrary byte fragmentation and may return multiple decoded frames. `flush()` verifies that no truncated frame remains.

### `estimate_ged(residuals) -> tuple[float, float]`

Returns `(alpha, beta)` for finite residuals using moment-ratio inversion.

## C++

Include `<aether/aether.hpp>` and link `aether::aether_core`.

```cpp
aether::CodecConfig config;
config.target_rate = 3.0f;
config.absolute_error_bound = 0.0f;
config.deadzone_factor = 0.3f;
config.enable_index = true;

aether::AetherCodec codec(config);
std::vector<uint8_t> encoded = codec.compress(samples);
std::vector<float> decoded(samples.size());
codec.decompress(encoded, decoded);

aether::IndexedStreamView view(encoded);  // encoded must outlive view
std::vector<float> window(512);
view.decompress_slice(4096, window.size(), window);
```

Include `<aether/table.hpp>` for `IndexEntry`, `IndexedStreamView`, and the
one-shot `decompress_slice` convenience function.

### Core types

- `CodecConfig`: codec-construction parameters, including optional indexing and the adaptive-tail quality guard.
- `AetherCodec`: re-entrant batch codec; const methods can be called concurrently when output buffers do not overlap.
- `StreamEncoder`, `StreamDecoder`: stateful, single-stream objects.
- `IndexedStreamView`: non-owning cached index; its compressed span must remain alive.
- `StreamError`: base class for codec and stream failures (`StreamError` in Python).
- `CorruptedStreamException`: malformed bytes (`CorruptedStreamError` in Python).
- `DeprecatedWireFormatException`: indexed wire-v5 input that must be decoded by its historical release and re-encoded (`DeprecatedWireFormatError` in Python).
- `UnsupportedWireFormatException`: wire versions below 5 or above 6 (`UnsupportedWireFormatError` in Python).
- `RateBudgetExceeded`: an infeasible serialized-rate request (`RateBudgetError` in Python).
- `GedEstimator`, `ECLMQuantizer`, `InterleavedRansEncoder/Decoder`: lower-level research interfaces.

All spans are borrowed for the duration of a call. Returned vectors own their storage. No API accepts NaN or infinity as telemetry input.

## Stable C ABI

Include `<aether/c_api.h>` and link the shared `aether::aether_c` target (or
`aether::aether_c_static` for static linking). `AETHER_C_ABI_VERSION` is `1`.
The ABI exports batch compression, full decompression, indexed slice decoding,
version discovery, and stable status strings. Every output buffer is owned by
the caller, and no C++ exception can cross the ABI boundary.

`aether_compress` writes the required byte count to `bytes_written` when it
returns `AETHER_ERR_BUFFER_TOO_SMALL`. A non-null one-byte probe can therefore
be passed with capacity zero before allocating the final output. Likewise,
`aether_decompress` reports the embedded sample count through `samples_written`.
All pointer parameters must be non-null, even when their corresponding capacity
is zero. Wire format v6 always stores and validates CRC32-C; `enable_crc` is
retained in ABI v1 for source compatibility and must contain either zero or one.

## Go

The module is under `bindings/go`, with package
`github.com/Animish-Sharma/AetherStream/bindings/go/aether`. Build the CMake
`aether_c` target in the repository's `build` directory before running or
linking the cgo package. `DefaultConfig`, `Compress`, `Decompress`, and
`DecompressSlice` map native failures to `*aether.StatusError`. Input and output
slices are passed directly across cgo without staging copies.

## Rust

The crate under `bindings/rust` exposes `Config`, `ConfigBuilder`, `compress`,
`decompress`, and `decompress_slice`. Its build script compiles the C++ core and
C ABI into a private static library, so an external system installation is not
required. Native failures are represented by the exhaustive `AetherError`
enum; no unsafe API is public.

## TSDB bridge

Include `<aether/tsdb/influx_line_codec.hpp>` and link
`aether::aether_tsdb`. `encode_influx_lines` parses ordered InfluxDB
line-protocol points, while `encode_prometheus_chunk` accepts timestamps and
values extracted from one Prometheus remote-write series. Both produce an
`ATDB` payload containing a 4-byte magic, 16-byte timestamp metadata,
delta-of-delta varints, and one AetherStream wire frame. `decode_chunk` validates
and reconstructs both vectors.
