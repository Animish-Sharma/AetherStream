"""Optional zero-copy Apache Arrow integration for AetherStream.

PyArrow is imported lazily and remains an optional dependency. Uncompressed
Arrow values are exposed to the native compressor through Arrow's buffer
protocol without materializing Python lists. Decompression writes directly into
a mutable Arrow allocation that becomes the PrimitiveArray's value buffer.
"""

from __future__ import annotations

import struct
from typing import Any

import aether

_MAGIC = b"AARW"
_VERSION = 1
_HEADER = struct.Struct("<4sHHQf")
_ENTRY = struct.Struct("<QI")


def _pyarrow() -> Any:
    try:
        import pyarrow as pa
    except ImportError as error:
        raise ImportError("PyArrow integration requires the optional 'pyarrow' package") from error
    return pa


def compress_arrow_array(chunked_array: Any, target_rate: float = 3.0) -> Any:
    """Compress a float32 Arrow Array or ChunkedArray into a ``pa.Buffer``.

    Each physical Arrow chunk remains an independently framed AetherStream
    payload. The wrapper header records chunk sample counts and compressed byte
    lengths, allowing direct validation and direct-to-Arrow decompression.
    Null values are rejected because AetherStream stores dense telemetry; callers
    should persist a separate Arrow validity bitmap when nulls are required.
    """
    pa = _pyarrow()
    if not isinstance(chunked_array, (pa.Array, pa.ChunkedArray)):
        raise TypeError("chunked_array must be a PyArrow Array or ChunkedArray")
    if chunked_array.type != pa.float32():
        raise TypeError("AetherStream Arrow bridge requires float32 input")
    chunks = chunked_array.chunks if isinstance(chunked_array, pa.ChunkedArray) else [chunked_array]
    if any(chunk.null_count for chunk in chunks):
        raise ValueError("AetherStream Arrow bridge does not encode null values")

    payloads: list[bytes] = []
    sample_counts: list[int] = []
    for chunk in chunks:
        values = chunk.to_numpy(zero_copy_only=True, writable=False)
        encoded = aether.compress(values, target_rate=target_rate)
        payloads.append(encoded)
        sample_counts.append(len(chunk))

    total_length = sum(sample_counts)
    header = bytearray(_HEADER.pack(_MAGIC, _VERSION, len(payloads), total_length, target_rate))
    for payload, sample_count in zip(payloads, sample_counts):
        header += _ENTRY.pack(len(payload), sample_count)
    header += b"".join(payloads)
    # pa.py_buffer retains the immutable bytes object instead of copying it.
    return pa.py_buffer(bytes(header))


def decompress_arrow_buffer(buffer: Any, length: int) -> Any:
    """Decompress an AetherStream Arrow buffer directly into a PrimitiveArray."""
    pa = _pyarrow()
    if length < 0:
        raise ValueError("length must be non-negative")
    view = memoryview(buffer)
    if view.nbytes < _HEADER.size:
        raise ValueError("truncated AetherStream Arrow header")
    magic, version, chunk_count, total_length, target_rate = _HEADER.unpack_from(view)
    if magic != _MAGIC or version != _VERSION:
        raise ValueError("unsupported AetherStream Arrow buffer")
    if total_length != length:
        raise ValueError("Arrow output length does not match compressed metadata")
    if not 2.0 <= target_rate <= 8.0:
        raise ValueError("invalid Arrow target-rate metadata")
    table_bytes = chunk_count * _ENTRY.size
    payload_offset = _HEADER.size + table_bytes
    if payload_offset > view.nbytes:
        raise ValueError("truncated AetherStream Arrow chunk table")

    entries = []
    samples_seen = 0
    bytes_seen = payload_offset
    for index in range(chunk_count):
        compressed_bytes, sample_count = _ENTRY.unpack_from(
            view, _HEADER.size + index * _ENTRY.size
        )
        if compressed_bytes == 0 and sample_count != 0:
            raise ValueError("invalid empty Arrow compressed chunk")
        if compressed_bytes > view.nbytes - bytes_seen:
            raise ValueError("truncated AetherStream Arrow chunk")
        entries.append((bytes_seen, compressed_bytes, sample_count))
        bytes_seen += compressed_bytes
        samples_seen += sample_count
    if bytes_seen != view.nbytes or samples_seen != length:
        raise ValueError("inconsistent AetherStream Arrow framing")

    values_buffer = pa.allocate_buffer(length * 4)
    writable_values = memoryview(values_buffer).cast("f")
    output_offset = 0
    for byte_offset, compressed_bytes, sample_count in entries:
        encoded = view[byte_offset : byte_offset + compressed_bytes]
        destination = writable_values[output_offset : output_offset + sample_count]
        aether._decompress_into(encoded, sample_count, destination)
        output_offset += sample_count
    return pa.Array.from_buffers(pa.float32(), length, [None, values_buffer])


__all__ = ["compress_arrow_array", "decompress_arrow_buffer"]
