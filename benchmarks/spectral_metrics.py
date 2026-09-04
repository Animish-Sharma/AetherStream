#!/usr/bin/env python3
"""Spectral fidelity and seismic phase-arrival metrics."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

import numpy as np

PSD_FLOOR = 1.0e-12
WELCH_FFT_SIZE = 1024


def welch_psd(
    samples: np.ndarray,
    sample_rate_hz: float = 1.0,
    *,
    fft_size: int = WELCH_FFT_SIZE,
) -> tuple[np.ndarray, np.ndarray]:
    """Estimate a one-sided PSD with a Hann window and 50% overlap."""
    values = _finite_vector(samples, "samples").astype(np.float64, copy=False)
    if not math.isfinite(sample_rate_hz) or sample_rate_hz <= 0.0:
        raise ValueError("sample_rate_hz must be finite and positive")
    if fft_size < 2:
        raise ValueError("fft_size must be at least two")

    segment_size = min(fft_size, values.size)
    window = np.hanning(segment_size)
    window_energy = float(np.dot(window, window))
    if window_energy == 0.0:
        window = np.ones(segment_size, dtype=np.float64)
        window_energy = float(segment_size)
    step = max(1, segment_size // 2)
    starts = list(range(0, values.size - segment_size + 1, step))
    if not starts:
        starts = [0]

    accumulated = np.zeros(fft_size // 2 + 1, dtype=np.float64)
    for start in starts:
        segment = values[start : start + segment_size]
        segment = (segment - float(np.mean(segment))) * window
        spectrum = np.fft.rfft(segment, n=fft_size)
        accumulated += np.square(np.abs(spectrum)) / (sample_rate_hz * window_energy)
    accumulated /= len(starts)
    if fft_size % 2 == 0:
        accumulated[1:-1] *= 2.0
    else:
        accumulated[1:] *= 2.0
    frequencies = np.fft.rfftfreq(fft_size, d=1.0 / sample_rate_hz)
    return frequencies, accumulated


def log_spectral_distance(
    original: np.ndarray,
    reconstructed: np.ndarray,
    sample_rate_hz: float = 1.0,
    *,
    fft_size: int = WELCH_FFT_SIZE,
    epsilon: float = PSD_FLOOR,
) -> float:
    """Return integrated PSD log-spectral distance in decibels."""
    first, second = _paired_vectors(original, reconstructed)
    if not math.isfinite(epsilon) or epsilon <= 0.0:
        raise ValueError("epsilon must be finite and positive")
    _, original_psd = welch_psd(first, sample_rate_hz, fft_size=fft_size)
    _, reconstructed_psd = welch_psd(second, sample_rate_hz, fft_size=fft_size)
    difference_db = 10.0 * np.log10(
        np.maximum(original_psd, epsilon) / np.maximum(reconstructed_psd, epsilon)
    )
    # The PSD is symmetric. Integrating over [0, pi] and dividing by pi is
    # equivalent to the specified two-sided integral divided by 2*pi.
    if difference_db.size == 1:
        return float(abs(difference_db[0]))
    normalized_integral = float(
        (
            0.5 * difference_db[0] ** 2
            + np.sum(difference_db[1:-1] ** 2)
            + 0.5 * difference_db[-1] ** 2
        )
        / (difference_db.size - 1)
    )
    return math.sqrt(max(0.0, normalized_integral))


def phase_arrival_jitter(
    original: np.ndarray,
    reconstructed: np.ndarray,
    *,
    max_lag: int = 256,
    sample_rate_hz: float = 1.0,
) -> dict[str, float | int]:
    """Return the lag maximizing normalized cross-correlation.

    Positive lag means that the reconstructed arrival occurs later than the
    original arrival under sum_t x[t] reconstructed[t+lag].
    """
    first, second = _paired_vectors(original, reconstructed)
    if max_lag < 0:
        raise ValueError("max_lag must be non-negative")
    if not math.isfinite(sample_rate_hz) or sample_rate_hz <= 0.0:
        raise ValueError("sample_rate_hz must be finite and positive")
    effective_lag = min(max_lag, first.size - 1)
    best_lag = 0
    best_correlation = -math.inf
    correlations: list[float] = []
    for lag in range(-effective_lag, effective_lag + 1):
        if lag >= 0:
            left = first[: first.size - lag] if lag else first
            right = second[lag:]
        else:
            left = first[-lag:]
            right = second[: second.size + lag]
        denominator = math.sqrt(float(np.dot(left, left)) * float(np.dot(right, right)))
        correlation = 0.0 if denominator == 0.0 else float(np.dot(left, right)) / denominator
        correlations.append(correlation)
        if correlation > best_correlation:
            best_lag = lag
            best_correlation = correlation
    return {
        "delay_samples": best_lag,
        "delay_seconds": best_lag / sample_rate_hz,
        "peak_normalized_correlation": best_correlation,
        "zero_lag_normalized_correlation": correlations[effective_lag],
    }


def peak_acceleration_absolute_error(
    original: np.ndarray,
    reconstructed: np.ndarray,
    *,
    extremal_fraction: float = 0.01,
) -> float:
    """Return L-infinity error over the largest-magnitude source samples."""
    first, second = _paired_vectors(original, reconstructed)
    if not math.isfinite(extremal_fraction) or not 0.0 < extremal_fraction <= 1.0:
        raise ValueError("extremal_fraction must be in (0, 1]")
    selected_count = max(1, math.ceil(first.size * extremal_fraction))
    indices = np.argpartition(np.abs(first), first.size - selected_count)[-selected_count:]
    return float(np.max(np.abs(first[indices] - second[indices])))


def _finite_vector(values: np.ndarray, name: str) -> np.ndarray:
    array = np.asarray(values)
    if array.ndim != 1 or array.size < 2:
        raise ValueError(f"{name} must be a one-dimensional array with at least two samples")
    if not np.isfinite(array).all():
        raise ValueError(f"{name} contains NaN or infinity")
    return array


def _paired_vectors(first: np.ndarray, second: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    left = _finite_vector(first, "original").astype(np.float64, copy=False)
    right = _finite_vector(second, "reconstructed").astype(np.float64, copy=False)
    if left.shape != right.shape:
        raise ValueError("original and reconstructed lengths differ")
    return left, right


def _uniform_reconstruction(samples: np.ndarray, bits: int = 4) -> np.ndarray:
    minimum = float(np.min(samples))
    maximum = float(np.max(samples))
    if maximum == minimum:
        return samples.copy()
    levels = (1 << bits) - 1
    codes = np.rint((samples - minimum) * levels / (maximum - minimum))
    return (minimum + codes * (maximum - minimum) / levels).astype(np.float32)


def _evaluate_trace(samples: np.ndarray, sample_rate_hz: float) -> dict[str, Any]:
    try:
        import aether
    except ImportError as error:
        raise RuntimeError("install AetherStream before running spectral evaluation") from error
    encoded = aether.compress(samples, target_rate=4.0)
    reconstructed = aether.decompress(encoded, samples.size)
    actual_rate = 8.0 * len(encoded) / samples.size
    uniform_bits = max(1, math.floor(actual_rate))
    uniform = _uniform_reconstruction(samples, uniform_bits)
    aether_lsd = log_spectral_distance(samples, reconstructed, sample_rate_hz)
    uniform_lsd = log_spectral_distance(samples, uniform, sample_rate_hz)
    return {
        "samples": int(samples.size),
        "sample_rate_hz": sample_rate_hz,
        "aether_bits_per_sample": actual_rate,
        "aether_lsd_db": aether_lsd,
        "rate_matched_uniform_bits_per_sample": uniform_bits,
        "rate_matched_uniform_lsd_db": uniform_lsd,
        "lsd_improvement_over_uniform_db": uniform_lsd - aether_lsd,
        "phase_arrival": phase_arrival_jitter(
            samples,
            reconstructed,
            max_lag=min(256, samples.size - 1),
            sample_rate_hz=sample_rate_hz,
        ),
        "top_one_percent_peak_absolute_error": peak_acceleration_absolute_error(
            samples, reconstructed
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--sample-rate", type=float, default=100.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    default_trace = Path(__file__).resolve().parent / "data" / "ridgecrest_strong_motion.npy"
    trace_path = args.trace or default_trace
    if trace_path.exists():
        samples = np.load(trace_path).astype(np.float32, copy=False)
        source = str(trace_path)
    else:
        time_axis = np.arange(131_072, dtype=np.float32) / np.float32(args.sample_rate)
        samples = (
            np.sin(2.0 * np.pi * 3.0 * time_axis) + 0.35 * np.sin(2.0 * np.pi * 17.0 * time_axis)
        ).astype(np.float32)
        source = "deterministic 3 Hz + 17 Hz synthetic resonance"
    result = {"source": source, **_evaluate_trace(samples, args.sample_rate)}
    text = json.dumps(result, indent=2, allow_nan=False)
    print(text)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
