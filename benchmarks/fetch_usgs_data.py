#!/usr/bin/env python3
"""Fetch EarthScope telemetry and decode MiniSEED 2 without ObsPy.

The MiniSEED reader intentionally uses only Python's standard library. NumPy is
used after decoding to normalize telemetry and write the benchmark's .npy files.
Supported payload encodings are INT16, INT32, FLOAT32, FLOAT64, Steim-1, and
Steim-2, which covers the EarthScope channels selected below.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

import numpy as np

FDSN_ENDPOINT = "https://service.earthscope.org/fdsnws/dataselect/1/query"
SCEDC_ENDPOINT = "https://service.scedc.caltech.edu/fdsnws/dataselect/1/query"
DATASETS = {
    "usgs_seismic": {
        "net": "IU",
        "sta": "ANMO",
        "loc": "00",
        "cha": "BHZ",
        "starttime": "2024-01-01T00:00:00",
        "endtime": "2024-01-01T01:00:00",
    },
    "ridgecrest_strong_motion": {
        "net": "CI",
        "sta": "CCC",
        "loc": "",
        "cha": "HNE",
        "starttime": "2019-07-06T03:19:30",
        "endtime": "2019-07-06T03:22:30",
    },
    "usgs_strain": {
        "net": "PB",
        "sta": "B004",
        "loc": "T0",
        "cha": "LS1",
        "starttime": "2024-01-01T00:00:00",
        "endtime": "2024-01-02T00:00:00",
    },
}
DATASET_SOURCES: dict[str, dict[str, Any]] = {
    "ridgecrest_strong_motion": {
        "endpoint": SCEDC_ENDPOINT,
        # StationXML response sensitivity valid at the event time. The source
        # channel reports acceleration in counts per m/s^2.
        "counts_per_mps2": 213979.64220881052,
        "event": "2019 Ridgecrest Mw 7.1 mainshock",
        "response_source": (
            "https://service.earthscope.org/fdsnws/station/1/query?"
            "net=CI&sta=CCC&loc=--&cha=HNE&level=response&format=xml&"
            "starttime=2019-07-06T03:19:30&endtime=2019-07-06T03:22:30"
        ),
    }
}


def _sign_extend(value: int, width: int) -> int:
    sign = 1 << (width - 1)
    return (value ^ sign) - sign


def _packed_differences(word: int, width: int, count: int) -> list[int]:
    mask = (1 << width) - 1
    return [
        _sign_extend((word >> (width * (count - index - 1))) & mask, width)
        for index in range(count)
    ]


def _decode_steim_word(word: int, control: int, encoding: int) -> list[int]:
    if control == 0:
        return []
    if control == 1:
        return _packed_differences(word, 8, 4)
    if encoding == 10:  # Steim-1
        if control == 2:
            return _packed_differences(word, 16, 2)
        if control == 3:
            return [_sign_extend(word, 32)]
    elif encoding == 11:  # Steim-2
        dnib = word >> 30
        if control == 2:
            if dnib == 1:
                return [_sign_extend(word & 0x3FFFFFFF, 30)]
            if dnib == 2:
                return _packed_differences(word & 0x3FFFFFFF, 15, 2)
            if dnib == 3:
                return _packed_differences(word & 0x3FFFFFFF, 10, 3)
        elif control == 3:
            if dnib == 0:
                return _packed_differences(word & 0x3FFFFFFF, 6, 5)
            if dnib == 1:
                return _packed_differences(word & 0x3FFFFFFF, 5, 6)
            if dnib == 2:
                # Seven four-bit differences occupy the low 28 bits; bits
                # 29-28 are unused after the two-bit dnib selector.
                return _packed_differences(word & 0x0FFFFFFF, 4, 7)
    raise ValueError(f"invalid Steim-{encoding - 9} packing code {control}/{word >> 30}")


def _decode_steim(
    record: bytes, data_offset: int, sample_count: int, encoding: int, endian: str
) -> list[int]:
    frame_bytes = len(record) - data_offset
    if data_offset < 48 or frame_bytes < 64 or frame_bytes % 64 != 0:
        raise ValueError("invalid MiniSEED Steim frame region")
    differences: list[int] = []
    x0: int | None = None
    xn: int | None = None
    for frame_index in range(frame_bytes // 64):
        words = struct.unpack_from(endian + "16I", record, data_offset + frame_index * 64)
        control = words[0]
        if frame_index == 0:
            x0 = _sign_extend(words[1], 32)
            xn = _sign_extend(words[2], 32)
        for word_index in range(1, 16):
            if frame_index == 0 and word_index in (1, 2):
                continue
            code = (control >> (30 - 2 * word_index)) & 0x3
            differences.extend(_decode_steim_word(words[word_index], code, encoding))
    if x0 is None or xn is None:
        raise ValueError("MiniSEED Steim record has no integration constants")
    if sample_count == 0:
        return []
    # Steim stores a first difference connecting this record to the preceding
    # record. X0 is authoritative, so that integration difference is skipped.
    required = sample_count - 1
    if len(differences) < required + 1:
        raise ValueError("MiniSEED Steim record contains too few differences")
    values = [x0]
    for difference in differences[1 : required + 1]:
        values.append(values[-1] + difference)
    if values[-1] != xn:
        raise ValueError("MiniSEED Steim reverse integration constant mismatch")
    return values


def _header_endian(payload: bytes, offset: int) -> str:
    for endian in (">", "<"):
        year, day = struct.unpack_from(endian + "HH", payload, offset + 20)
        data_offset, blockette_offset = struct.unpack_from(endian + "HH", payload, offset + 44)
        if (
            1900 <= year <= 2200
            and 1 <= day <= 366
            and data_offset >= 48
            and (blockette_offset == 0 or 48 <= blockette_offset < 65536)
        ):
            return endian
    raise ValueError("invalid MiniSEED fixed section data header byte order")


def _record_layout(payload: bytes, offset: int) -> tuple[int, int, int, int, str]:
    if len(payload) - offset < 56:
        raise ValueError("truncated MiniSEED fixed section data header")
    endian = _header_endian(payload, offset)
    sample_count = struct.unpack_from(endian + "H", payload, offset + 30)[0]
    data_offset, next_blockette = struct.unpack_from(endian + "HH", payload, offset + 44)
    encoding: int | None = None
    data_endian = ">"
    record_length: int | None = None
    visited: set[int] = set()
    while next_blockette:
        if (
            next_blockette in visited
            or next_blockette < 48
            or next_blockette + 4 > len(payload) - offset
        ):
            raise ValueError("invalid MiniSEED blockette chain")
        visited.add(next_blockette)
        blockette_type, following = struct.unpack_from(
            endian + "HH", payload, offset + next_blockette
        )
        if blockette_type == 1000:
            if next_blockette + 8 > len(payload) - offset:
                raise ValueError("truncated MiniSEED blockette 1000")
            encoding, word_order, length_exponent, _ = struct.unpack_from(
                "BBBB", payload, offset + next_blockette + 4
            )
            if word_order not in (0, 1) or not 8 <= length_exponent <= 20:
                raise ValueError("invalid MiniSEED blockette 1000")
            data_endian = ">" if word_order == 1 else "<"
            record_length = 1 << length_exponent
        next_blockette = following
    if encoding is None or record_length is None:
        raise ValueError("MiniSEED 2 record is missing blockette 1000")
    if record_length > len(payload) - offset or data_offset >= record_length:
        raise ValueError("truncated MiniSEED record")
    return record_length, sample_count, encoding, data_offset, data_endian


def decode_mseed2(payload: bytes) -> list[float | int]:
    """Decode a concatenated MiniSEED-2 byte stream into numeric samples."""
    if not payload:
        raise ValueError("empty MiniSEED response")
    samples: list[float | int] = []
    offset = 0
    while offset < len(payload):
        record_length, sample_count, encoding, data_offset, endian = _record_layout(payload, offset)
        record = payload[offset : offset + record_length]
        if encoding in (10, 11):
            decoded: list[float | int] = list(
                _decode_steim(record, data_offset, sample_count, encoding, endian)
            )
        else:
            formats = {1: "h", 3: "i", 4: "f", 5: "d"}
            if encoding not in formats:
                raise ValueError(f"unsupported MiniSEED encoding {encoding}")
            width = struct.calcsize(formats[encoding])
            if sample_count > (record_length - data_offset) // width:
                raise ValueError("MiniSEED sample count exceeds record payload")
            decoded = list(
                struct.unpack_from(
                    endian + str(sample_count) + formats[encoding], record, data_offset
                )
            )
        if len(decoded) != sample_count:
            raise ValueError("MiniSEED decoder produced the wrong sample count")
        samples.extend(decoded)
        offset += record_length
    return samples


def _normalize_telemetry(values: list[float | int]) -> np.ndarray:
    samples = np.asarray(values, dtype=np.float64)
    if samples.size < 64 or not np.isfinite(samples).all():
        raise RuntimeError("FDSN service returned insufficient or non-finite samples")
    samples -= np.mean(samples)
    scale = float(np.std(samples))
    if not math.isfinite(scale) or scale <= np.finfo(np.float64).tiny:
        scale = float(np.max(np.abs(samples), initial=0.0))
    if not math.isfinite(scale) or scale <= np.finfo(np.float64).tiny:
        scale = 1.0
    return np.asarray(samples / scale, dtype=np.float32)


def _fetch_mseed_details(
    query: dict[str, str],
    timeout: int = 60,
    endpoint: str = FDSN_ENDPOINT,
    counts_per_mps2: float | None = None,
) -> tuple[np.ndarray, str, dict[str, float]]:
    request_query = {key: value for key, value in query.items() if value != ""}
    url = endpoint + "?" + urllib.parse.urlencode(request_query)
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": "AetherStream/2.3 (+https://github.com/animish-sharma/aetherstream)"
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = response.read()
    decoded = decode_mseed2(payload)
    details: dict[str, float] = {}
    if counts_per_mps2 is not None:
        peak_counts = max(abs(float(value)) for value in decoded)
        peak_ground_acceleration_g = peak_counts / counts_per_mps2 / 9.80665
        if peak_ground_acceleration_g <= 0.5:
            raise RuntimeError(
                "Ridgecrest strong-motion trace did not meet the verified 0.5g threshold"
            )
        details["counts_per_mps2"] = counts_per_mps2
        details["peak_ground_acceleration_g"] = peak_ground_acceleration_g
    return _normalize_telemetry(decoded), url, details


def fetch_mseed(query: dict[str, str], timeout: int = 60) -> tuple[np.ndarray, str]:
    """Fetch a default EarthScope query, preserving the public two-value API."""
    samples, url, _ = _fetch_mseed_details(query, timeout)
    return samples, url


def synthetic_fallback(name: str, count: int = 500_000) -> np.ndarray:
    rng = np.random.default_rng(0x2251 if name.endswith("seismic") else 0x2252)
    time = np.arange(count, dtype=np.float64) / 100.0
    if name.endswith("seismic"):
        trace = np.cumsum(rng.normal(0.0, 8e-5, count))
        trace += np.exp(-time / 1800.0) * np.sin(2 * np.pi * 1.7 * time)
        trace += rng.normal(0.0, 0.002, count)
    else:
        trace = 0.04 * np.sin(2 * np.pi * 0.002 * time)
        trace += rng.normal(0.0, 1e-4, count)
        locations = rng.choice(count, max(1, count // 10_000), replace=False)
        trace[locations] += rng.uniform(-2.0, 2.0, locations.size)
    return trace.astype(np.float32)


def fetch_all(
    output_dir: str | Path = "benchmarks/data",
    strict: bool = False,
    sample_count: int = 500_000,
) -> dict[str, dict[str, Any]]:
    destination = Path(output_dir)
    destination.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, dict[str, Any]] = {}
    for name, query in DATASETS.items():
        source_config = DATASET_SOURCES.get(name, {})
        details: dict[str, Any] = {}
        try:
            samples, source, details = _fetch_mseed_details(
                query,
                endpoint=str(source_config.get("endpoint", FDSN_ENDPOINT)),
                counts_per_mps2=source_config.get("counts_per_mps2"),
            )
            source_kind = (
                "SCEDC FDSN MiniSEED 2 service"
                if name == "ridgecrest_strong_motion"
                else "EarthScope FDSN MiniSEED 2 service"
            )
        except Exception as error:
            if strict:
                raise
            samples = synthetic_fallback(name, sample_count)
            source = f"offline deterministic fallback: {error}"
            source_kind = "synthetic fallback"
        if samples.size > sample_count:
            samples = samples[:sample_count]
        np.save(destination / f"{name}.npy", samples)
        manifest[name] = {
            "samples": int(samples.size),
            "dtype": "float32",
            "normalization": "zero mean and unit standard deviation",
            "source_kind": source_kind,
            "source": source,
            "query": query,
            **({"event": source_config["event"]} if "event" in source_config else {}),
            **(
                {"response_source": source_config["response_source"]}
                if "response_source" in source_config
                else {}
            ),
            **details,
        }
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default="benchmarks/data")
    parser.add_argument("--samples", type=int, default=500_000)
    parser.add_argument(
        "--strict",
        action="store_true",
        help="fail instead of generating deterministic offline traces",
    )
    arguments = parser.parse_args()
    print(
        json.dumps(fetch_all(arguments.output_dir, arguments.strict, arguments.samples), indent=2)
    )


if __name__ == "__main__":
    main()
