#!/usr/bin/env python3
"""Measure indexed slice latency against whole-frame decompression."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import aetherstream
import numpy as np


def median_seconds(operation, repetitions: int) -> float:
    timings = []
    for _ in range(repetitions):
        start = time.perf_counter_ns()
        operation()
        timings.append((time.perf_counter_ns() - start) * 1e-9)
    return float(np.median(np.asarray(timings, dtype=np.float64)))


def benchmark(
    samples: np.ndarray, start: int, count: int, repetitions: int
) -> dict[str, float | int]:
    encoded = aetherstream.compress(samples, target_rate=4.0, enable_index=True)
    full = aetherstream.decompress(encoded, samples.size)
    expected = full[start : start + count]
    actual = aetherstream.decompress_slice(encoded, start, count)
    np.testing.assert_array_equal(actual, expected)

    full_seconds = median_seconds(
        lambda: aetherstream.decompress(encoded, samples.size), repetitions
    )
    slice_seconds = median_seconds(
        lambda: aetherstream.decompress_slice(encoded, start, count), repetitions
    )
    return {
        "samples": int(samples.size),
        "slice_start": start,
        "slice_samples": count,
        "compressed_bytes": len(encoded),
        "full_decompression_us": full_seconds * 1e6,
        "slice_decompression_us": slice_seconds * 1e6,
        "speedup": full_seconds / slice_seconds,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, default=Path("benchmarks/data/usgs_seismic.npy"))
    parser.add_argument("--slice-samples", type=int, default=512)
    parser.add_argument("--repetitions", type=int, default=101)
    arguments = parser.parse_args()
    samples = np.load(arguments.input).astype(np.float32, copy=False)
    if not 0 < arguments.slice_samples <= samples.size:
        raise ValueError("slice-samples must be within the input")
    start = (samples.size - arguments.slice_samples) // 2
    result = benchmark(samples, start, arguments.slice_samples, arguments.repetitions)
    output = Path("benchmarks/results/random_access.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
