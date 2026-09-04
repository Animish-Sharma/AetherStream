#!/usr/bin/env python3
"""Rate-distortion lower bounds for generalized-error residual models.

The GED parameterization is

    f(x) = beta / (2 alpha Gamma(1/beta)) exp(-(|x|/alpha)**beta).

All differential entropies are calculated in nats and converted to bits only
at the final rate calculation. This keeps the Gaussian special case exactly
consistent with R(D) = 0.5 log2(sigma**2 / D).
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

import numpy as np

STREAM_HEADER_BYTES = 24
RANS_SCALE_TOTAL = 4096


def ged_variance(alpha: float, beta: float) -> float:
    """Return the variance of a zero-mean GED."""
    _validate_ged(alpha, beta)
    return alpha * alpha * math.exp(math.lgamma(3.0 / beta) - math.lgamma(1.0 / beta))


def ged_differential_entropy(alpha: float, beta: float) -> float:
    """Return GED differential entropy in nats.

    The normalization term already contains the factor two. Adding another
    ``ln(2)`` would double-count the two-sided support and would incorrectly
    give a positive Gaussian rate at D equal to the source variance.
    """
    _validate_ged(alpha, beta)
    return 1.0 / beta - math.log(beta) + math.log(2.0 * alpha) + math.lgamma(1.0 / beta)


def shannon_lower_bound(mse: float, alpha: float, beta: float) -> float:
    """Return the Shannon lower bound in bits/sample for squared error."""
    if not math.isfinite(mse) or mse < 0.0:
        raise ValueError("mse must be finite and non-negative")
    entropy_nats = ged_differential_entropy(alpha, beta)
    if mse == 0.0:
        return math.inf
    gaussian_noise_entropy = 0.5 * math.log(2.0 * math.pi * math.e * mse)
    return max(0.0, (entropy_nats - gaussian_noise_entropy) / math.log(2.0))


def evaluate_theoretical_slack(
    actual_bits_per_sample: float,
    mse: float,
    alpha: float,
    beta: float,
    *,
    sample_count: int = 2048,
    header_bytes: int = STREAM_HEADER_BYTES,
    alphabet_size: int = 256,
) -> dict[str, float]:
    """Decompose measured rate above the Shannon lower bound.

    ``rans_frequency_quantization_bound`` is the conservative K/(M ln 2)
    redundancy bound for an alphabet of K symbols and M=4096 normalization.
    ``residual_codebook_mismatch_slack`` is the signed remainder, so a negative
    value exposes a violated model assumption instead of hiding it by clipping.
    """
    if not math.isfinite(actual_bits_per_sample) or actual_bits_per_sample < 0.0:
        raise ValueError("actual_bits_per_sample must be finite and non-negative")
    if sample_count <= 0 or header_bytes < 0:
        raise ValueError("sample_count must be positive and header_bytes non-negative")
    if alphabet_size <= 0 or alphabet_size > RANS_SCALE_TOTAL:
        raise ValueError("alphabet_size must be in [1, 4096]")

    lower_bound = shannon_lower_bound(mse, alpha, beta)
    total_slack = actual_bits_per_sample - lower_bound
    metadata = 8.0 * header_bytes / sample_count
    frequency_bound = alphabet_size / (RANS_SCALE_TOTAL * math.log(2.0))
    mismatch = total_slack - metadata - frequency_bound
    return {
        "actual_bits_per_sample": actual_bits_per_sample,
        "shannon_lower_bound_bits_per_sample": lower_bound,
        "shannon_inefficiency_slack": total_slack,
        "metadata_overhead_bits_per_sample": metadata,
        "rans_frequency_quantization_bound": frequency_bound,
        "residual_codebook_mismatch_slack": mismatch,
    }


def _validate_ged(alpha: float, beta: float) -> None:
    if not math.isfinite(alpha) or alpha <= 0.0:
        raise ValueError("alpha must be finite and positive")
    if not math.isfinite(beta) or beta <= 0.0:
        raise ValueError("beta must be finite and positive")


def _synthetic_evaluation(samples: int, rate: float) -> dict[str, Any]:
    time_axis = np.arange(samples, dtype=np.float32)
    signal = (
        np.sin(time_axis * np.float32(0.017))
        + np.float32(0.2) * np.sin(time_axis * np.float32(0.071))
    ).astype(np.float32)
    try:
        import aether
    except ImportError:
        mse = 2.0 ** (-2.0 * rate)
        return {
            "source": "unit Gaussian analytical example",
            "note": "native aether module unavailable; using Gaussian equality point",
            **evaluate_theoretical_slack(rate, mse, math.sqrt(2.0), 2.0, sample_count=samples),
        }

    encoded = aether.compress(signal, target_rate=rate)
    decoded = aether.decompress(encoded, signal.size)
    residual = signal.astype(np.float64) - decoded.astype(np.float64)
    mse = float(np.mean(residual * residual))
    predictor_residual = np.diff(signal, prepend=signal[0]).astype(np.float32)
    alpha, beta = aether.estimate_ged(predictor_residual)
    return {
        "source": "deterministic two-tone telemetry, first-difference entropy proxy",
        "samples": samples,
        "alpha": float(alpha),
        "beta": float(beta),
        **evaluate_theoretical_slack(
            8.0 * len(encoded) / samples,
            mse,
            float(alpha),
            float(beta),
            sample_count=samples,
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=120_000)
    parser.add_argument("--rate", type=float, default=3.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.samples <= 0:
        parser.error("--samples must be positive")
    result = _synthetic_evaluation(args.samples, args.rate)
    text = json.dumps(result, indent=2, allow_nan=False)
    print(text)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
