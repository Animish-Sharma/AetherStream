#!/usr/bin/env python3
"""Reproducible real-trace benchmark with JSON, CSV, and PNG exports."""

import csv
import json
import math
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any

import aether
import numpy as np
from fetch_usgs_data import fetch_all

plt: Any
zstd: Any
try:
    import matplotlib.pyplot as plt
except ImportError:
    plt = None
try:
    import zstandard as zstd
except ImportError:
    zstd = None

ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data"
OUTPUT = ROOT / "results"


def quality(original, reconstructed):
    difference = original.astype(np.float64) - reconstructed.astype(np.float64)
    mse = float(np.mean(difference * difference))
    maximum = float(np.max(np.abs(difference), initial=0.0))
    span = float(np.ptp(original.astype(np.float64)))
    psnr = math.inf if mse == 0 else 10 * math.log10(span * span / mse)
    return mse, psnr, maximum


def result(dataset, codec, samples, reconstructed, byte_count, enc_time, dec_time, status="ok"):
    mse, psnr, maximum = quality(samples, reconstructed)
    return {
        "dataset": dataset,
        "codec": codec,
        "status": status,
        "samples": int(samples.size),
        "compressed_bytes": int(byte_count),
        "bits_per_sample": 8.0 * byte_count / samples.size,
        "compression_ratio": samples.nbytes / byte_count,
        "encode_gbps": samples.nbytes / enc_time / 1e9,
        "decode_gbps": samples.nbytes / dec_time / 1e9,
        "mse": mse,
        "psnr_db": psnr,
        "max_absolute_error": maximum,
    }


def bench_aether(name, samples, rate=None, error=None):
    label = f"aether-rate-{rate:g}" if rate else f"aether-error-{error:g}"
    start = time.perf_counter()
    encoded = aether.compress(samples, target_rate=rate or 4.0, absolute_error=error or 0.0)
    enc_time = time.perf_counter() - start
    start = time.perf_counter()
    decoded = aether.decompress(encoded, samples.size)
    dec_time = time.perf_counter() - start
    row = result(name, label, samples, decoded, len(encoded), enc_time, dec_time)
    if rate is not None and row["bits_per_sample"] > rate + 1e-12:
        raise AssertionError("statutory wire-rate violation")
    if error is not None and row["max_absolute_error"] > error * 1.0001:
        raise AssertionError("absolute-error violation")
    return row


def shuffled_bytes(samples):
    return samples.view(np.uint8).reshape(-1, 4).T.copy().tobytes()


def unshuffle(data, count):
    return np.frombuffer(data, np.uint8).reshape(4, count).T.copy().view(np.float32).reshape(-1)


def bench_zstd(name, samples, level, shuffle):
    source = shuffled_bytes(samples) if shuffle else samples.tobytes()
    compressor = zstd.ZstdCompressor(level=level)
    decompressor = zstd.ZstdDecompressor()
    start = time.perf_counter()
    encoded = compressor.compress(source)
    enc_time = time.perf_counter() - start
    start = time.perf_counter()
    raw = decompressor.decompress(encoded, max_output_size=len(source))
    decoded = unshuffle(raw, samples.size) if shuffle else np.frombuffer(raw, np.float32)
    dec_time = time.perf_counter() - start
    return result(
        name,
        f"zstd-{level}-{'shuffle' if shuffle else 'raw'}",
        samples,
        decoded,
        len(encoded),
        enc_time,
        dec_time,
    )


def bench_sz3(name, samples, error):
    executable = shutil.which("sz3")
    if executable is None:
        raise RuntimeError("SZ3 executable not found")
    with tempfile.TemporaryDirectory(prefix="aether-sz3-") as directory:
        source = Path(directory) / "input.f32"
        compressed = Path(directory) / "output.sz3"
        restored = Path(directory) / "restored.f32"
        samples.tofile(source)
        compress_command = [
            executable,
            "-f",
            "-i",
            str(source),
            "-z",
            str(compressed),
            "-1",
            str(samples.size),
            "-M",
            "ABS",
            str(error),
        ]
        start = time.perf_counter()
        subprocess.run(compress_command, check=True, capture_output=True)
        enc_time = time.perf_counter() - start
        decompress_command = [
            executable,
            "-f",
            "-z",
            str(compressed),
            "-o",
            str(restored),
            "-1",
            str(samples.size),
        ]
        start = time.perf_counter()
        subprocess.run(decompress_command, check=True, capture_output=True)
        dec_time = time.perf_counter() - start
        decoded = np.fromfile(restored, dtype=np.float32)
        if decoded.size != samples.size:
            raise RuntimeError("SZ3 reconstructed an unexpected sample count")
        row = result(
            name,
            f"sz3-error-{error:g}",
            samples,
            decoded,
            compressed.stat().st_size,
            enc_time,
            dec_time,
        )
        if row["max_absolute_error"] > error * 1.0001:
            raise AssertionError("SZ3 absolute-error violation")
        return row


def bench_uniform(name, samples, bits):
    levels_count = 1 << bits
    start = time.perf_counter()
    levels = np.quantile(samples, np.linspace(0, 1, levels_count)).astype(np.float32)
    boundaries = (levels[:-1] + levels[1:]) * 0.5
    symbols = np.searchsorted(boundaries, samples).astype(np.uint8)
    enc_time = time.perf_counter() - start
    start = time.perf_counter()
    decoded = levels[symbols]
    dec_time = time.perf_counter() - start
    byte_count = math.ceil(samples.size * bits / 8) + levels.nbytes
    return result(
        name, f"uniform-lloyd-{bits}bit", samples, decoded, byte_count, enc_time, dec_time
    )


def finite_json(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


def save_outputs(rows):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    fields = list(rows[0])
    with (OUTPUT / "benchmark_results.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    clean = [{key: finite_json(value) for key, value in row.items()} for row in rows]
    (OUTPUT / "benchmark_results.json").write_text(
        json.dumps(clean, indent=2, allow_nan=False), encoding="utf-8"
    )
    if plt is not None:
        figure, axis = plt.subplots(figsize=(8, 5))
        for dataset in sorted({row["dataset"] for row in rows}):
            selected = [
                row for row in rows if row["dataset"] == dataset and math.isfinite(row["psnr_db"])
            ]
            axis.scatter(
                [row["bits_per_sample"] for row in selected],
                [row["psnr_db"] for row in selected],
                label=dataset,
            )
            for row in selected:
                axis.annotate(
                    row["codec"], (row["bits_per_sample"], row["psnr_db"]), fontsize=6, rotation=20
                )
        axis.set(
            xlabel="actual wire bits/sample",
            ylabel="PSNR (dB)",
            title="AetherStream v2.2 Pareto comparison",
        )
        axis.grid(alpha=0.3)
        axis.legend()
        figure.tight_layout()
        figure.savefig(OUTPUT / "pareto_frontier.png", dpi=180)


def print_table(rows):
    print("| dataset | codec | bits/sample | ratio | enc GB/s | dec GB/s | MSE | PSNR | L_inf |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        psnr = "inf" if math.isinf(row["psnr_db"]) else f'{row["psnr_db"]:.2f}'
        print(
            f'| {row["dataset"]} | {row["codec"]} | {row["bits_per_sample"]:.3f} | '
            f'{row["compression_ratio"]:.2f} | {row["encode_gbps"]:.3f} | '
            f'{row["decode_gbps"]:.3f} | {row["mse"]:.3e} | {psnr} | '
            f'{row["max_absolute_error"]:.3e} |'
        )


def main():
    if not (DATA / "manifest.json").exists():
        fetch_all(DATA)
    rows = []
    for path in sorted(DATA.glob("*.npy")):
        samples = np.load(path).astype(np.float32, copy=False)
        name = path.stem
        aether.compress(samples[:4096], target_rate=4.0)
        for rate in (2.0, 3.0, 4.0):
            rows.append(bench_aether(name, samples, rate=rate))
        for error in (0.01, 0.001):
            rows.append(bench_aether(name, samples, error=error))
            if shutil.which("sz3") is not None:
                rows.append(bench_sz3(name, samples, error))
        for bits in (2, 3, 4):
            rows.append(bench_uniform(name, samples, bits))
        if zstd is not None:
            for level in (1, 3, 19):
                for shuffle in (False, True):
                    rows.append(bench_zstd(name, samples, level, shuffle))
    save_outputs(rows)
    print_table(rows)
    if zstd is None:
        print("Zstandard rows omitted: install the zstandard package.")
    if shutil.which("sz3") is None:
        print("SZ3 rows omitted: install the official sz3 executable; no mock is reported as SZ3.")


if __name__ == "__main__":
    main()
