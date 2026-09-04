import math

import numpy as np
import pytest

from benchmarks.profile_hardware import _parse_perf, _safe_ratio
from benchmarks.spectral_metrics import (
    log_spectral_distance,
    peak_acceleration_absolute_error,
    phase_arrival_jitter,
    welch_psd,
)
from benchmarks.theoretical_bounds import (
    evaluate_theoretical_slack,
    ged_differential_entropy,
    ged_variance,
    shannon_lower_bound,
)


def test_ged_entropy_and_gaussian_rate_distortion_identity():
    sigma = 1.7
    alpha = math.sqrt(2.0) * sigma
    entropy = ged_differential_entropy(alpha, 2.0)
    assert entropy == pytest.approx(0.5 * math.log(2.0 * math.pi * math.e * sigma**2))
    assert ged_variance(alpha, 2.0) == pytest.approx(sigma**2)

    distortion = 0.125 * sigma**2
    expected = 0.5 * math.log2(sigma**2 / distortion)
    assert shannon_lower_bound(distortion, alpha, 2.0) == pytest.approx(expected)
    assert shannon_lower_bound(2.0 * sigma**2, alpha, 2.0) == 0.0


def test_laplace_entropy_and_slack_decomposition_are_consistent():
    alpha = 0.75
    assert ged_differential_entropy(alpha, 1.0) == pytest.approx(1.0 + math.log(2 * alpha))
    decomposition = evaluate_theoretical_slack(
        4.0, 0.02, alpha, 1.0, sample_count=4096, header_bytes=24, alphabet_size=64
    )
    subtotal = (
        decomposition["metadata_overhead_bits_per_sample"]
        + decomposition["rans_frequency_quantization_bound"]
        + decomposition["residual_codebook_mismatch_slack"]
    )
    assert subtotal == pytest.approx(decomposition["shannon_inefficiency_slack"])
    assert decomposition["rans_frequency_quantization_bound"] <= 64 / (4096 * math.log(2))


def test_welch_lsd_is_zero_for_identical_signals_and_finite_near_silence():
    time_axis = np.arange(8192, dtype=np.float64) / 200.0
    signal = np.sin(2 * np.pi * 7.0 * time_axis) + 0.2 * np.sin(2 * np.pi * 31.0 * time_axis)
    frequencies, psd = welch_psd(signal, 200.0)
    assert frequencies.shape == psd.shape == (513,)
    assert np.all(psd >= 0.0)
    assert log_spectral_distance(signal, signal, 200.0) == pytest.approx(0.0, abs=1e-12)
    assert math.isfinite(log_spectral_distance(signal, signal + 1e-14, 200.0))


def test_phase_arrival_delay_sign_and_zero_phase_reconstruction():
    signal = np.zeros(4096, dtype=np.float64)
    pulse = np.hanning(41)
    signal[1900:1941] = pulse
    delayed = np.zeros_like(signal)
    delayed[1907:1948] = pulse
    measured = phase_arrival_jitter(signal, delayed, max_lag=20, sample_rate_hz=100.0)
    assert measured["delay_samples"] == 7
    assert measured["delay_seconds"] == pytest.approx(0.07)
    assert measured["peak_normalized_correlation"] == pytest.approx(1.0)
    assert phase_arrival_jitter(signal, signal, max_lag=20)["delay_samples"] == 0


def test_peak_acceleration_error_uses_only_extremal_source_samples():
    original = np.arange(1, 101, dtype=np.float64)
    reconstructed = original.copy()
    reconstructed[-1] -= 2.5
    reconstructed[0] += 20.0
    assert peak_acceleration_absolute_error(original, reconstructed, extremal_fraction=0.01) == 2.5


def test_perf_delimited_counter_parser_and_ratios(tmp_path):
    counter_file = tmp_path / "perf.txt"
    counter_file.write_text(
        "1000;;cycles;1.0;100.0\n"
        "2500;;instructions;1.0;100.0\n"
        "<not supported>;;L1-dcache-loads;1.0;100.0\n",
        encoding="utf-8",
    )
    counters, diagnostics = _parse_perf(counter_file)
    assert counters == {"cycles": 1000, "instructions": 2500}
    assert diagnostics == ["L1-dcache-loads: <not supported>"]
    assert _safe_ratio(counters["instructions"], counters["cycles"]) == 2.5
    assert _safe_ratio(1, 0) is None


def test_aether_spectral_improvement_and_zero_phase_delay():
    aether = pytest.importorskip("aether")
    time_axis = np.arange(32768, dtype=np.float32) / np.float32(200.0)
    signal = np.sin(2 * np.pi * 6.0 * time_axis).astype(np.float32)
    encoded = aether.compress(signal, target_rate=4.0)
    decoded = aether.decompress(encoded, signal.size)

    levels = 15
    minimum, maximum = float(signal.min()), float(signal.max())
    uniform = (
        minimum
        + np.rint((signal - minimum) * levels / (maximum - minimum)) * (maximum - minimum) / levels
    ).astype(np.float32)
    aether_lsd = log_spectral_distance(signal, decoded, 200.0)
    uniform_lsd = log_spectral_distance(signal, uniform, 200.0)
    assert uniform_lsd - aether_lsd >= 3.0
    assert phase_arrival_jitter(signal, decoded, max_lag=32)["delay_samples"] == 0


@pytest.mark.parametrize(
    ("function", "arguments"),
    [
        (ged_differential_entropy, (0.0, 2.0)),
        (shannon_lower_bound, (-1.0, 1.0, 2.0)),
        (log_spectral_distance, (np.ones(8), np.ones(7))),
    ],
)
def test_research_metrics_reject_invalid_inputs(function, arguments):
    with pytest.raises(ValueError):
        function(*arguments)
