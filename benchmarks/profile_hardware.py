#!/usr/bin/env python3
"""Linux perf counter and Roofline profiling harness for AetherStream."""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

import numpy as np

PERF_EVENTS = (
    "cycles",
    "instructions",
    "L1-dcache-loads",
    "L1-dcache-load-misses",
    "branch-instructions",
    "branch-misses",
)


def _worker(sample_count: int, iterations: int) -> dict[str, float | int]:
    try:
        import aether
    except ImportError as error:
        raise RuntimeError("install AetherStream before profiling") from error
    time_axis = np.arange(sample_count, dtype=np.float32)
    samples = (np.sin(time_axis * 0.013) + 0.2 * np.cos(time_axis * 0.071)).astype(np.float32)
    encoded = aether.compress(samples, target_rate=4.0)
    start = time.perf_counter()
    total_encoded_bytes = 0
    for _ in range(iterations):
        encoded = aether.compress(samples, target_rate=4.0)
        decoded = aether.decompress(encoded, sample_count)
        if decoded.size != sample_count:
            raise RuntimeError("native codec returned an unexpected sample count")
        total_encoded_bytes += len(encoded)
    elapsed = time.perf_counter() - start
    return {
        "elapsed_seconds": elapsed,
        "sample_count": sample_count,
        "iterations": iterations,
        "encoded_bytes_per_iteration": total_encoded_bytes / iterations,
    }


def _parse_perf(path: Path) -> tuple[dict[str, int], list[str]]:
    counters: dict[str, int] = {}
    diagnostics: list[str] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        fields = [field.strip() for field in line.split(";")]
        if len(fields) < 3 or fields[2] not in PERF_EVENTS:
            continue
        raw_value = fields[0].replace(",", "")
        if raw_value.startswith("<"):
            diagnostics.append(f"{fields[2]}: {raw_value}")
            continue
        try:
            counters[fields[2]] = int(float(raw_value))
        except ValueError:
            diagnostics.append(f"{fields[2]}: unparseable value {fields[0]!r}")
    return counters, diagnostics


def _perf_allowed() -> tuple[bool, str]:
    if sys.platform != "linux":
        return False, "perf hardware counters are available only on Linux"
    if shutil.which("perf") is None:
        return False, "perf executable was not found"
    paranoid_path = Path("/proc/sys/kernel/perf_event_paranoid")
    if paranoid_path.exists():
        try:
            level = int(paranoid_path.read_text(encoding="ascii").strip())
        except ValueError:
            return False, "perf_event_paranoid is not an integer"
        if level > 1 and os.geteuid() != 0:
            return False, f"perf_event_paranoid={level} restricts hardware counters"
    return True, "hardware counters permitted"


def _cache_size_bytes() -> int | None:
    path = Path("/sys/devices/system/cpu/cpu0/cache/index3/size")
    if not path.exists():
        return None
    text = path.read_text(encoding="ascii").strip().upper()
    multipliers = {"K": 1024, "M": 1024**2, "G": 1024**3}
    try:
        if text[-1:] in multipliers:
            return int(text[:-1]) * multipliers[text[-1]]
        return int(text)
    except ValueError:
        return None


def estimate_memory_bandwidth() -> dict[str, float | int | str | None]:
    """Measure sustained copy bandwidth using a working set larger than LLC."""
    cache_bytes = _cache_size_bytes()
    working_set = max(32 * 1024**2, 4 * (cache_bytes or 0))
    working_set = min(working_set, 128 * 1024**2)
    elements = working_set // np.dtype(np.float64).itemsize
    source = np.linspace(0.0, 1.0, elements, dtype=np.float64)
    destination = np.empty_like(source)
    np.copyto(destination, source)
    repeats = max(3, math.ceil(512 * 1024**2 / working_set))
    start = time.perf_counter()
    for _ in range(repeats):
        np.copyto(destination, source)
    elapsed = time.perf_counter() - start
    # A copy reads and writes the full working set.
    transferred = 2 * working_set * repeats
    return {
        "estimated_memory_bandwidth_gb_s": transferred / elapsed / 1.0e9,
        "last_level_cache_bytes": cache_bytes,
        "bandwidth_working_set_bytes": working_set,
        "bandwidth_method": "NumPy sequential copy, source plus destination traffic",
    }


def _safe_ratio(numerator: int | float | None, denominator: int | float | None) -> float | None:
    if numerator is None or denominator is None or denominator == 0:
        return None
    return float(numerator) / float(denominator)


def collect_profile(
    sample_count: int,
    iterations: int,
    *,
    operations_per_sample: float = 24.0,
    peak_gflops: float | None = None,
) -> dict[str, Any]:
    if sample_count <= 0 or iterations <= 0:
        raise ValueError("sample_count and iterations must be positive")
    if not math.isfinite(operations_per_sample) or operations_per_sample <= 0.0:
        raise ValueError("operations_per_sample must be finite and positive")

    bandwidth = estimate_memory_bandwidth()
    allowed, permission_note = _perf_allowed()
    counters: dict[str, int] = {}
    diagnostics = [permission_note]
    worker_result: dict[str, Any]
    if allowed:
        with tempfile.TemporaryDirectory(prefix="aether-perf-") as directory:
            perf_output = Path(directory) / "counters.txt"
            command = [
                "perf",
                "stat",
                "-x",
                ";",
                "--no-big-num",
                "-e",
                ",".join(PERF_EVENTS),
                "-o",
                str(perf_output),
                "--",
                sys.executable,
                str(Path(__file__).resolve()),
                "--worker",
                "--samples",
                str(sample_count),
                "--iterations",
                str(iterations),
            ]
            completed = subprocess.run(command, text=True, capture_output=True, check=False)
            if completed.returncode == 0:
                worker_result = json.loads(completed.stdout)
                counters, parse_notes = _parse_perf(perf_output)
                diagnostics.extend(parse_notes)
            else:
                diagnostics.append(
                    "perf failed; using wall-clock fallback: "
                    + (completed.stderr.strip() or f"exit status {completed.returncode}")
                )
                worker_result = _worker(sample_count, iterations)
    else:
        worker_result = _worker(sample_count, iterations)

    total_samples = sample_count * iterations
    encoded_bytes = float(worker_result["encoded_bytes_per_iteration"])
    # Codec traffic model: one input read, one encoded write/read, and one output write.
    modeled_bytes = iterations * (2 * sample_count * 4 + 2 * encoded_bytes)
    modeled_operations = operations_per_sample * total_samples
    intensity = modeled_operations / modeled_bytes
    elapsed = float(worker_result["elapsed_seconds"])
    throughput_gops = modeled_operations / elapsed / 1.0e9
    bandwidth_value = bandwidth["estimated_memory_bandwidth_gb_s"]
    if not isinstance(bandwidth_value, (int, float)):
        raise RuntimeError("memory bandwidth estimator returned a non-numeric value")
    measured_bandwidth = float(bandwidth_value)
    if peak_gflops is None:
        # This is a measured application point, not a claim about processor peak.
        peak_gflops = max(throughput_gops, 1.0)
        diagnostics.append(
            "compute ceiling defaults to measured throughput; pass --peak-gflops for a CPU peak"
        )
    roofline_limit = min(peak_gflops, measured_bandwidth * intensity)

    return {
        "mode": "perf" if counters else "wall-clock-fallback",
        "diagnostics": diagnostics,
        "counters": counters,
        "ipc": _safe_ratio(counters.get("instructions"), counters.get("cycles")),
        "l1_data_cache_miss_rate": _safe_ratio(
            counters.get("L1-dcache-load-misses"), counters.get("L1-dcache-loads")
        ),
        "branch_misprediction_rate": _safe_ratio(
            counters.get("branch-misses"), counters.get("branch-instructions")
        ),
        "elapsed_seconds": elapsed,
        "samples_processed": total_samples,
        "modeled_operations": modeled_operations,
        "modeled_bytes_transferred": modeled_bytes,
        "arithmetic_intensity_operations_per_byte": intensity,
        "measured_application_throughput_gops": throughput_gops,
        "memory_bandwidth_gb_s": measured_bandwidth,
        "compute_ceiling_gops": peak_gflops,
        "roofline_limit_gops": roofline_limit,
        **bandwidth,
    }


def plot_roofline(profile: dict[str, Any], output: Path) -> None:
    try:
        import matplotlib.pyplot as plt
    except ImportError as error:
        raise RuntimeError("matplotlib is required to plot the Roofline figure") from error
    output.parent.mkdir(parents=True, exist_ok=True)
    intensity = float(profile["arithmetic_intensity_operations_per_byte"])
    bandwidth = float(profile["memory_bandwidth_gb_s"])
    ceiling = float(profile["compute_ceiling_gops"])
    x_values = np.logspace(-3, 3, 400)
    y_values = np.minimum(ceiling, bandwidth * x_values)
    figure, axis = plt.subplots(figsize=(3.5, 2.5))
    axis.loglog(x_values, y_values, color="black", label="CPU Roofline")
    axis.scatter(
        [intensity],
        [profile["measured_application_throughput_gops"]],
        marker="o",
        color="#00629b",
        label="AetherStream",
        zorder=3,
    )
    axis.set_xlabel("Arithmetic intensity (operations/byte)")
    axis.set_ylabel("Throughput (Gop/s)")
    axis.grid(True, which="both", alpha=0.25)
    axis.legend(fontsize=8)
    figure.tight_layout()
    figure.savefig(output, format="pdf", bbox_inches="tight")
    plt.close(figure)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=262_144)
    parser.add_argument("--iterations", type=int, default=8)
    parser.add_argument("--operations-per-sample", type=float, default=24.0)
    parser.add_argument("--peak-gflops", type=float)
    parser.add_argument(
        "--output", type=Path, default=Path("benchmarks/results/hardware_profile.json")
    )
    parser.add_argument("--plot", type=Path, default=Path("paper/figures/fig_roofline.pdf"))
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        print(json.dumps(_worker(args.samples, args.iterations), allow_nan=False))
        return
    profile = collect_profile(
        args.samples,
        args.iterations,
        operations_per_sample=args.operations_per_sample,
        peak_gflops=args.peak_gflops,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(profile, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    try:
        plot_roofline(profile, args.plot)
    except RuntimeError as error:
        profile["diagnostics"].append(str(error))
        args.output.write_text(
            json.dumps(profile, indent=2, allow_nan=False) + "\n", encoding="utf-8"
        )
    print(json.dumps(profile, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
