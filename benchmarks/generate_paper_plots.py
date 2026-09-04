#!/usr/bin/env python3
"""Generate reproducible IEEE-column vector figures for the AetherStream paper."""

from __future__ import annotations

import argparse
import json
import math
import shutil
import statistics
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any

import numpy as np
from spectral_metrics import welch_psd
from theoretical_bounds import shannon_lower_bound

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FIGURES = ROOT / "paper" / "figures"
DATA = ROOT / "benchmarks" / "data"
RESULTS = ROOT / "benchmarks" / "results"


def _configure_matplotlib() -> Any:
    try:
        import matplotlib as mpl
        import matplotlib.pyplot as plt
    except ImportError as error:
        raise RuntimeError("matplotlib is required to generate paper figures") from error
    mpl.rcParams.update(
        {
            "font.family": "serif",
            "font.serif": ["Computer Modern Roman", "CMU Serif", "DejaVu Serif"],
            "mathtext.fontset": "cm",
            "font.size": 9,
            "axes.labelsize": 9,
            "legend.fontsize": 7,
            "xtick.labelsize": 8,
            "ytick.labelsize": 8,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
            "figure.dpi": 150,
        }
    )
    return plt


def _load_aether() -> Any:
    try:
        import aether
    except ImportError as error:
        raise RuntimeError("install AetherStream before generating empirical figures") from error
    return aether


def _signals(count: int = 65_536) -> list[np.ndarray]:
    signals = []
    time_axis = np.arange(count, dtype=np.float32)
    for seed in range(5):
        rng = np.random.default_rng(2400 + seed)
        signal = (
            np.sin(time_axis * np.float32(0.011 + seed * 0.0003))
            + np.float32(0.2) * np.cos(time_axis * np.float32(0.071))
            + np.float32(0.005) * rng.standard_normal(count)
        ).astype(np.float32)
        signals.append(signal)
    return signals


def _mean_error(values: list[float]) -> tuple[float, float]:
    mean = statistics.fmean(values)
    if len(values) < 2:
        return mean, 0.0
    return mean, statistics.stdev(values) / math.sqrt(len(values))


def _aether_and_uniform_points() -> tuple[dict[str, list[tuple[float, float]]], float, float]:
    aether = _load_aether()
    results: dict[str, list[tuple[float, float]]] = {"AetherStream": [], "Uniform": []}
    signals = _signals()
    residuals = np.concatenate([np.diff(signal, prepend=signal[0]) for signal in signals])
    alpha, beta = aether.estimate_ged(residuals.astype(np.float32))
    for rate in (2.0, 3.0, 4.0, 6.0):
        aether_trials: list[tuple[float, float]] = []
        uniform_trials: list[tuple[float, float]] = []
        bits = max(1, int(math.floor(rate)))
        for signal in signals:
            encoded = aether.compress(signal, target_rate=rate)
            reconstructed = aether.decompress(encoded, signal.size)
            mse = float(np.mean(np.square(signal.astype(np.float64) - reconstructed)))
            aether_trials.append((mse, 8.0 * len(encoded) / signal.size))

            level_count = 1 << bits
            minimum, maximum = float(np.min(signal)), float(np.max(signal))
            step = (maximum - minimum) / (level_count - 1)
            quantized = minimum + np.rint((signal - minimum) / step) * step
            uniform_mse = float(np.mean(np.square(signal.astype(np.float64) - quantized)))
            uniform_trials.append((uniform_mse, float(bits)))
        results["AetherStream"].extend(aether_trials)
        results["Uniform"].extend(uniform_trials)
    return results, float(alpha), float(beta)


def _sz3_relative_points() -> dict[str, list[tuple[float, float]]]:
    executable = shutil.which("sz3")
    if executable is None:
        return {}
    results: dict[str, list[tuple[float, float]]] = {}
    for tolerance in (1.0e-3, 1.0e-4, 1.0e-5):
        exponent = round(math.log10(tolerance))
        label = rf"SZ3 relative $10^{{{exponent}}}$"
        points: list[tuple[float, float]] = []
        for signal in _signals():
            with tempfile.TemporaryDirectory(prefix="aether-paper-sz3-") as directory:
                source = Path(directory) / "source.f32"
                encoded = Path(directory) / "encoded.sz3"
                restored = Path(directory) / "restored.f32"
                signal.tofile(source)
                subprocess.run(
                    [
                        executable,
                        "-f",
                        "-i",
                        str(source),
                        "-z",
                        str(encoded),
                        "-1",
                        str(signal.size),
                        "-M",
                        "REL",
                        str(tolerance),
                    ],
                    check=True,
                    capture_output=True,
                )
                subprocess.run(
                    [
                        executable,
                        "-f",
                        "-z",
                        str(encoded),
                        "-o",
                        str(restored),
                        "-1",
                        str(signal.size),
                    ],
                    check=True,
                    capture_output=True,
                )
                reconstructed = np.fromfile(restored, dtype=np.float32)
                if reconstructed.size != signal.size:
                    raise RuntimeError("SZ3 reconstructed an unexpected sample count")
                mse = float(np.mean(np.square(signal.astype(np.float64) - reconstructed)))
                points.append((mse, 8.0 * encoded.stat().st_size / signal.size))
        results[label] = points
    return results


def _zstandard_points() -> dict[str, list[tuple[float, float]]]:
    try:
        import zstandard
    except ImportError:
        return {}
    results: dict[str, list[tuple[float, float]]] = {}
    for level in (3, 19):
        points = []
        compressor = zstandard.ZstdCompressor(level=level)
        for signal in _signals():
            shuffled = signal.view(np.uint8).reshape(-1, 4).T.copy().tobytes()
            encoded = compressor.compress(shuffled)
            points.append((1.0e-14, 8.0 * len(encoded) / signal.size))
        results[f"Zstandard-{level}"] = points
    return results


def _external_comparator_points() -> dict[str, list[tuple[float, float]]]:
    path = RESULTS / "benchmark_results.json"
    if not path.exists():
        return {}
    rows = json.loads(path.read_text(encoding="utf-8"))
    selected: dict[str, list[tuple[float, float]]] = {}
    mappings = {
        "zstd-3-shuffle": "Zstandard-3",
        "zstd-19-shuffle": "Zstandard-19",
    }
    for prefix, label in mappings.items():
        points = [
            (max(float(row["mse"]), 1.0e-14), float(row["bits_per_sample"]))
            for row in rows
            if str(row.get("codec", "")).startswith(prefix) and row.get("status") == "ok"
        ]
        if points:
            selected[label] = points
    return selected


def figure_rate_distortion(output: Path, strict_comparators: bool) -> None:
    plt = _configure_matplotlib()
    measured, alpha, beta = _aether_and_uniform_points()
    comparators = {
        **_external_comparator_points(),
        **_zstandard_points(),
        **_sz3_relative_points(),
    }
    has_sz3 = any(label.startswith("SZ3") for label in comparators)
    has_zstandard = "Zstandard-3" in comparators and "Zstandard-19" in comparators
    if strict_comparators and not has_sz3:
        raise RuntimeError("SZ3 is absent; install the official sz3 executable")
    if strict_comparators and not has_zstandard:
        raise RuntimeError("Zstandard is absent; install the zstandard Python package")

    all_distortions = [point[0] for series in measured.values() for point in series]
    lower = max(min(all_distortions) / 4.0, 1.0e-12)
    upper = max(all_distortions) * 4.0
    distortions = np.logspace(math.log10(lower), math.log10(upper), 300)
    theory = [shannon_lower_bound(float(value), alpha, beta) for value in distortions]

    figure, axis = plt.subplots(figsize=(3.5, 2.65))
    axis.semilogx(distortions, theory, color="black", linestyle="--", label="Shannon lower bound")
    styles = {
        "AetherStream": ("o", "#00629b"),
        "Uniform": ("s", "#a23b72"),
        "SZ3": ("^", "#2a9d8f"),
        "Zstandard-3": ("x", "#e76f51"),
        "Zstandard-19": ("+", "#f4a261"),
    }
    for label, points in {**measured, **comparators}.items():
        marker, color = styles["SZ3"] if label.startswith("SZ3") else styles[label]
        grouped: dict[float, list[tuple[float, float]]] = {}
        for point_index, (distortion, rate) in enumerate(points):
            group_key = float(point_index // 5) if label in measured else 0.0
            grouped.setdefault(group_key, []).append((distortion, rate))
        x_values = []
        y_values = []
        x_errors = []
        y_errors = []
        for group in grouped.values():
            x_mean, x_error = _mean_error([point[0] for point in group])
            y_mean, y_error = _mean_error([point[1] for point in group])
            x_values.append(x_mean)
            y_values.append(y_mean)
            x_errors.append(x_error)
            y_errors.append(y_error)
        axis.errorbar(
            x_values,
            y_values,
            xerr=x_errors,
            yerr=y_errors,
            marker=marker,
            color=color,
            capsize=2,
            linewidth=1,
            label=label,
        )
    if not has_sz3:
        axis.text(
            0.02,
            0.02,
            "SZ3 omitted: executable unavailable",
            transform=axis.transAxes,
            fontsize=6,
            color="0.35",
        )
    axis.set_xlabel("Mean-squared distortion $D$")
    axis.set_ylabel("Rate (bits/sample)")
    axis.grid(True, which="both", alpha=0.25)
    axis.legend(loc="best", frameon=False)
    figure.tight_layout()
    figure.savefig(output, format="pdf", bbox_inches="tight")
    plt.close(figure)


def _load_resonance_trace() -> tuple[np.ndarray, float]:
    path = DATA / "ridgecrest_strong_motion.npy"
    if path.exists():
        return np.load(path).astype(np.float32, copy=False), 100.0
    sample_rate = 200.0
    time_axis = np.arange(131_072, dtype=np.float32) / np.float32(sample_rate)
    return np.sin(2 * np.pi * 7.0 * time_axis).astype(np.float32), sample_rate


def figure_log_spectral_distance(output: Path) -> None:
    plt = _configure_matplotlib()
    aether = _load_aether()
    samples, sample_rate = _load_resonance_trace()
    encoded = aether.compress(samples, target_rate=4.0)
    reconstructed = aether.decompress(encoded, samples.size)
    frequencies, source_psd = welch_psd(samples, sample_rate)
    _, reconstructed_psd = welch_psd(reconstructed, sample_rate)

    figure, axis = plt.subplots(figsize=(3.5, 2.45))
    axis.plot(
        frequencies,
        10 * np.log10(np.maximum(source_psd, 1e-12)),
        color="black",
        linewidth=1,
        label="Original",
    )
    axis.plot(
        frequencies,
        10 * np.log10(np.maximum(reconstructed_psd, 1e-12)),
        color="#00629b",
        linewidth=0.9,
        linestyle="--",
        label="AetherStream",
    )
    axis.set_xlabel("Frequency (Hz)")
    axis.set_ylabel("PSD (dB/Hz)")
    axis.grid(True, alpha=0.25)
    axis.legend(frameon=False)
    figure.tight_layout()
    figure.savefig(output, format="pdf", bbox_inches="tight")
    plt.close(figure)


def _timed(callable_object: Any, repeats: int) -> float:
    durations = []
    for _ in range(repeats):
        start = time.perf_counter_ns()
        callable_object()
        durations.append((time.perf_counter_ns() - start) / 1000.0)
    return statistics.median(durations)


def figure_random_access(output: Path, maximum_samples: int) -> None:
    plt = _configure_matplotlib()
    aether = _load_aether()
    sizes = np.unique(np.logspace(3, math.log10(maximum_samples), 5).astype(np.int64))
    linear_latency = []
    indexed_latency = []
    for encoded_size in sizes:
        size = int(encoded_size)
        axis = np.arange(size, dtype=np.float32)
        samples = (np.sin(axis * 0.013) + 0.1 * np.cos(axis * 0.071)).astype(np.float32)
        wire = aether.compress(samples, target_rate=4.0, enable_index=True)
        view = aether.IndexedStreamView(wire)
        start = max(0, size // 2 - 256)
        count = min(512, size - start)
        repeats = 7 if size <= 100_000 else 3
        linear_latency.append(
            _timed(lambda wire=wire, size=size: aether.decompress(wire, size), repeats)
        )
        indexed_latency.append(
            _timed(
                lambda view=view, start=start, count=count: view.decompress_slice(start, count),
                repeats,
            )
        )

    figure, axis = plt.subplots(figsize=(3.5, 2.45))
    axis.loglog(sizes, linear_latency, marker="s", label="Linear full decode", color="#a23b72")
    axis.loglog(sizes, indexed_latency, marker="o", label="IndexedStreamView", color="#00629b")
    axis.set_xlabel("Array size (samples)")
    axis.set_ylabel(r"Median seek latency ($\mu$s)")
    axis.grid(True, which="both", alpha=0.25)
    axis.legend(frameon=False)
    figure.tight_layout()
    figure.savefig(output, format="pdf", bbox_inches="tight")
    plt.close(figure)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-directory", type=Path, default=DEFAULT_FIGURES)
    parser.add_argument("--strict-comparators", action="store_true")
    parser.add_argument("--max-random-access-samples", type=int, default=10_000_000)
    args = parser.parse_args()
    if args.max_random_access_samples < 1000:
        parser.error("--max-random-access-samples must be at least 1000")
    args.output_directory.mkdir(parents=True, exist_ok=True)
    outputs = [
        args.output_directory / "fig_rate_distortion_theory.pdf",
        args.output_directory / "fig_log_spectral_distance.pdf",
        args.output_directory / "fig_random_access_scaling.pdf",
    ]
    figure_rate_distortion(outputs[0], args.strict_comparators)
    figure_log_spectral_distance(outputs[1])
    figure_random_access(outputs[2], args.max_random_access_samples)
    for path in outputs:
        print(f"generated {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
