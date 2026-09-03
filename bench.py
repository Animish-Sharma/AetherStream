#!/usr/bin/env python3
"""Publication-style AetherStream v2 benchmark.

The benchmark is intentionally not part of the test suite: it processes 1.5M
samples through every codec configuration and is meant to be run explicitly.
"""

import math
import time
from dataclasses import dataclass

import numpy as np

import aether

try:
    import matplotlib.pyplot as plt
except ImportError:
    plt = None

try:
    import zstandard as zstd
except ImportError:
    zstd = None

SAMPLE_COUNT = 500_000
RATES = (2.0, 3.0, 4.0)
ERROR_BOUNDS = (0.01, 0.001)


@dataclass
class Result:
    signal: str
    codec: str
    encode_gbps: float
    decode_gbps: float
    bits_per_sample: float
    ratio: float
    mse: float
    psnr: float
    max_error: float


def make_datasets(sample_count=SAMPLE_COUNT):
    rng = np.random.default_rng(0xA37E)
    fs = 2_000.0
    t = np.arange(sample_count, dtype=np.float64) / fs

    envelope = 0.65 + 0.35 * np.exp(-t / 35.0)
    resonant = envelope * (
        np.sin(2 * np.pi * 50 * t)
        + 0.42 * np.sin(2 * np.pi * 120 * t + 0.3)
        + 0.16 * np.exp(-t / 18.0) * np.sin(2 * np.pi * 240 * t)
    )
    resonant += rng.normal(0.0, 0.002, sample_count)

    quiescent = rng.normal(0.0, 2.0e-4, sample_count)
    shock_locations = rng.choice(sample_count, sample_count // 2500, replace=False)
    shock_amplitudes = rng.uniform(-12.0, 12.0, shock_locations.size)
    quiescent[shock_locations] += shock_amplitudes

    increments = rng.normal(0.0, 1.8e-4, sample_count)
    drift = np.cumsum(increments)
    drift += 0.12 * np.sin(2 * np.pi * 0.08 * t)
    drift += rng.normal(0.0, 4.0e-4, sample_count)

    return {
        "resonant": resonant.astype(np.float32),
        "sparse-shock": quiescent.astype(np.float32),
        "drift": drift.astype(np.float32),
    }


def quality(samples, reconstructed):
    error = samples.astype(np.float64) - reconstructed.astype(np.float64)
    mse = float(np.mean(error * error))
    max_error = float(np.max(np.abs(error), initial=0.0))
    dynamic_range = float(np.ptp(samples.astype(np.float64)))
    psnr = math.inf if mse == 0.0 else 10.0 * math.log10(dynamic_range**2 / mse)
    return mse, psnr, max_error


def timed_aether(samples, label, *, target_rate=4.0, absolute_error=0.0):
    raw_bytes = samples.nbytes
    start = time.perf_counter()
    encoded = aether.compress(samples, target_rate=target_rate,
                              absolute_error=absolute_error)
    encode_time = time.perf_counter() - start
    start = time.perf_counter()
    reconstructed = aether.decompress(encoded, len(samples))
    decode_time = time.perf_counter() - start
    mse, psnr, max_error = quality(samples, reconstructed)
    if absolute_error > 0.0 and max_error > absolute_error * 1.0001:
        raise AssertionError(
            f"L_inf violation: {max_error:.9g} > {absolute_error:.9g}"
        )
    return Result(
        "", label, raw_bytes / encode_time / 1e9,
        raw_bytes / decode_time / 1e9, 8.0 * len(encoded) / len(samples),
        raw_bytes / len(encoded), mse, psnr, max_error,
    )


def byte_shuffle(samples):
    return samples.view(np.uint8).reshape(-1, 4).T.copy().tobytes()


def byte_unshuffle(data, sample_count):
    shuffled = np.frombuffer(data, dtype=np.uint8).reshape(4, sample_count)
    return shuffled.T.copy().reshape(-1).view(np.float32)


def timed_zstd(samples, level, shuffled):
    compressor = zstd.ZstdCompressor(level=level)
    decompressor = zstd.ZstdDecompressor()
    source = byte_shuffle(samples) if shuffled else samples.tobytes()
    start = time.perf_counter()
    encoded = compressor.compress(source)
    encode_time = time.perf_counter() - start
    start = time.perf_counter()
    decoded_bytes = decompressor.decompress(encoded, max_output_size=len(source))
    reconstructed = (byte_unshuffle(decoded_bytes, len(samples)) if shuffled
                     else np.frombuffer(decoded_bytes, dtype=np.float32))
    decode_time = time.perf_counter() - start
    mse, psnr, max_error = quality(samples, reconstructed)
    suffix = "shuffle" if shuffled else "raw"
    return Result(
        "", f"zstd-{level}-{suffix}", samples.nbytes / encode_time / 1e9,
        samples.nbytes / decode_time / 1e9,
        8.0 * len(encoded) / len(samples), samples.nbytes / len(encoded),
        mse, psnr, max_error,
    )


def markdown_table(results):
    print("| Signal | Codec | Enc GB/s | Dec GB/s | bits/sample | Ratio | MSE | PSNR dB | L_inf |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
    for row in results:
        psnr = "inf" if math.isinf(row.psnr) else f"{row.psnr:.2f}"
        print(
            f"| {row.signal} | {row.codec} | {row.encode_gbps:.3f} | "
            f"{row.decode_gbps:.3f} | {row.bits_per_sample:.3f} | "
            f"{row.ratio:.2f} | {row.mse:.4e} | {psnr} | "
            f"{row.max_error:.4e} |"
        )


def save_plot(results):
    if plt is None:
        print("matplotlib unavailable; v2_benchmark_results.png not written")
        return
    figure, (rate_axis, error_axis) = plt.subplots(1, 2, figsize=(12, 5))
    for signal in ("resonant", "sparse-shock", "drift"):
        rate_rows = [r for r in results if r.signal == signal and
                     r.codec.startswith("aether-rate")]
        rate_axis.plot([r.bits_per_sample for r in rate_rows],
                       [r.psnr for r in rate_rows], "o-", label=signal)
        bounded_rows = [r for r in results if r.signal == signal and
                        r.codec.startswith("aether-Linf")]
        bounds = [float(r.codec.split("=")[1]) for r in bounded_rows]
        error_axis.loglog(bounds, [r.max_error for r in bounded_rows],
                          "o-", label=signal)
    error_axis.loglog(ERROR_BOUNDS, ERROR_BOUNDS, "k--", label="required bound")
    rate_axis.set(xlabel="actual bits/sample", ylabel="PSNR (dB)",
                  title="Rate-distortion")
    error_axis.set(xlabel="requested absolute error", ylabel="measured L_inf",
                   title="Strict error-bound verification")
    for axis in (rate_axis, error_axis):
        axis.grid(True, which="both", alpha=0.3)
        axis.legend()
    figure.tight_layout()
    figure.savefig("v2_benchmark_results.png", dpi=180)
    print("Saved v2_benchmark_results.png")


def main():
    datasets = make_datasets()
    results = []
    for signal_name, samples in datasets.items():
        # Warm extension and allocator paths outside timed regions.
        aether.compress(samples[:4096], target_rate=3.0)
        for rate in RATES:
            row = timed_aether(samples, f"aether-rate={rate:g}", target_rate=rate)
            row.signal = signal_name
            results.append(row)
        for bound in ERROR_BOUNDS:
            row = timed_aether(samples, f"aether-Linf={bound:g}",
                               absolute_error=bound)
            row.signal = signal_name
            results.append(row)
        if zstd is not None:
            for level in (3, 19):
                for shuffled in (False, True):
                    row = timed_zstd(samples, level, shuffled)
                    row.signal = signal_name
                    results.append(row)
    if zstd is None:
        print("warning: install zstandard to include Zstandard baselines")
    markdown_table(results)
    save_plot(results)


if __name__ == "__main__":
    main()
