#!/usr/bin/env python3
"""Measure polynomial-quantizer scalar/SIMD speed and quality.

Results are measurements from the current machine and build; the script makes
no cross-platform throughput guarantee.  The exact boundary search is used as
the approximation-quality reference.
"""

from __future__ import annotations

import argparse
import json
import math
import platform
import statistics
import time
from pathlib import Path

import aether
import numpy as np


def timed(call, repeats: int) -> tuple[object, list[float]]:
    call()  # warm dispatch and caches
    durations: list[float] = []
    result: object = None
    for _ in range(repeats):
        started = time.perf_counter_ns()
        result = call()
        durations.append((time.perf_counter_ns() - started) * 1e-9)
    return result, durations


def psnr(reference: np.ndarray, reconstructed: np.ndarray) -> float:
    mse = float(np.mean((reference.astype(np.float64) - reconstructed) ** 2))
    peak = float(np.max(reference) - np.min(reference))
    if mse == 0.0:
        return math.inf
    return 20.0 * math.log10(peak / math.sqrt(mse))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", type=int, default=1_000_000)
    parser.add_argument("--repeats", type=int, default=9)
    parser.add_argument("--rate", type=float, default=4.0)
    parser.add_argument(
        "--output", type=Path, default=Path("benchmarks/results/simd_quantizer.json")
    )
    args = parser.parse_args()

    rng = np.random.default_rng(2200)
    values = rng.normal(0.0, 1.0, args.samples).astype(np.float32)
    # Deterministic sparse impulses exercise the logarithmic-tail design.
    values[::997] *= np.float32(24.0)
    alpha, beta = aether.estimate_ged(values)
    peak = float(np.max(np.abs(values)))
    quantizer = aether._QuantizerBenchmark(args.rate, alpha, beta, peak)
    quantizer.fit(values)

    exact = np.asarray(quantizer.quantize(values, "exact"))
    baseline_quantizer = aether._QuantizerBenchmark(args.rate, alpha, beta, 0.0)
    baseline_quantizer.fit(values)
    baseline_symbols = np.asarray(baseline_quantizer.quantize(values, "exact"))
    baseline_reconstruction = np.asarray(baseline_quantizer.reconstruct(baseline_symbols))
    scalar, scalar_times = timed(lambda: quantizer.quantize(values, "scalar"), args.repeats)
    native_backend = aether._simd_backend()
    native, native_times = timed(lambda: quantizer.quantize(values, "native"), args.repeats)
    scalar = np.asarray(scalar)
    native = np.asarray(native)

    exact_reconstruction = np.asarray(quantizer.reconstruct(exact))
    native_reconstruction = np.asarray(quantizer.reconstruct(native))
    approximation = native_reconstruction.astype(np.float64) - exact_reconstruction
    scalar_median = statistics.median(scalar_times)
    native_median = statistics.median(native_times)
    baseline_psnr = psnr(values, baseline_reconstruction)
    exact_psnr = psnr(values, exact_reconstruction)
    native_psnr = psnr(values, native_reconstruction)

    disagreement = float(np.mean(native != exact))
    polynomial_impact = native_psnr - exact_psnr
    if disagreement >= 0.01:
        raise AssertionError(
            f"native/exact symbol disagreement {disagreement:.4%} is not below 1.0%"
        )
    if polynomial_impact < -0.05:
        raise AssertionError(
            f"polynomial PSNR impact {polynomial_impact:.6f} dB exceeds the 0.05 dB limit"
        )

    result = {
        "verification": {
            "symbol_disagreement_below_1_percent": True,
            "psnr_loss_below_0_05_db": True,
        },
        "provenance": {
            "measured": True,
            "platform": platform.platform(),
            "python": platform.python_version(),
            "native_backend": native_backend,
            "signal": "synthetic_impulsive_strain_like",
            "samples": args.samples,
            "repeats": args.repeats,
            "seed": 2200,
            "rate_bits_per_sample": args.rate,
            "ged_alpha": alpha,
            "ged_beta": beta,
            "observed_peak": peak,
        },
        "throughput_msamples_s": {
            "scalar_polynomial": args.samples / scalar_median / 1e6,
            "native_polynomial": args.samples / native_median / 1e6,
            "speedup": scalar_median / native_median,
        },
        "approximation": {
            "native_vs_exact_symbol_disagreement_fraction": disagreement,
            "scalar_vs_native_symbol_disagreement_fraction": float(np.mean(scalar != native)),
            "reconstruction_rmse": float(np.sqrt(np.mean(approximation**2))),
            "reconstruction_max_abs_error": float(np.max(np.abs(approximation))),
        },
        "psnr_db": {
            "ordinary_ged_codebook": baseline_psnr,
            "impulsive_tail_exact_boundary": exact_psnr,
            "impulsive_tail_native_polynomial": native_psnr,
            "tail_codebook_gain": exact_psnr - baseline_psnr,
            "polynomial_impact_vs_exact": polynomial_impact,
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    print(
        f"PASS: index disagreement {disagreement:.4%} < 1.0%; "
        f"PSNR loss {max(0.0, -polynomial_impact):.6f} dB < 0.05 dB"
    )


if __name__ == "__main__":
    main()
