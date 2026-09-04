#!/usr/bin/env python3
"""Cross-platform AetherStream pre-release verification.

The default run builds and tests with the conservative one-job profile and
checks the SIMD quality gates. ``--full`` additionally downloads the verified
field traces, runs all benchmarks, and regenerates documentation assets.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_VERSION = "0.0.1"
EXPECTED_WIRE_VERSION = 6


def run(command: list[str], env: dict[str, str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def cmake_command() -> list[str]:
    executable = shutil.which("cmake")
    if executable:
        return [executable]
    try:
        subprocess.run(
            [sys.executable, "-m", "cmake", "--version"],
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError as error:
        raise RuntimeError("CMake is required for release verification") from error
    return [sys.executable, "-m", "cmake"]


def verify_metadata() -> None:
    import aetherstream
    import numpy as np

    if aetherstream.__version__ != EXPECTED_VERSION:
        raise AssertionError(f"installed version {aetherstream.__version__} != {EXPECTED_VERSION}")
    samples = np.linspace(-1.0, 1.0, 4096, dtype=np.float32)
    wire = aetherstream.compress(samples, enable_index=True)
    version = int.from_bytes(wire[4:6], "little")
    if version != EXPECTED_WIRE_VERSION:
        raise AssertionError(f"wire version {version} != {EXPECTED_WIRE_VERSION}")
    view = aetherstream.IndexedStreamView(wire)
    np.testing.assert_array_equal(
        view.decompress_slice(2000, 128),
        aetherstream.decompress(wire, samples.size)[2000:2128],
    )


def verify_simd_results() -> None:
    path = ROOT / "benchmarks/results/simd_quantizer.json"
    result = json.loads(path.read_text(encoding="utf-8"))
    disagreement = result["approximation"]["native_vs_exact_symbol_disagreement_fraction"]
    impact = result["psnr_db"]["polynomial_impact_vs_exact"]
    if disagreement >= 0.01:
        raise AssertionError(f"SIMD disagreement {disagreement:.4%} is not below 1.0%")
    if impact < -0.05:
        raise AssertionError(f"SIMD PSNR impact {impact:.6f} dB exceeds 0.05 dB")
    print(
        f"SIMD quality gates passed: disagreement={disagreement:.4%}, "
        f"PSNR impact={impact:.6f} dB"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--full", action="store_true", help="run live-data benchmarks and assets")
    parser.add_argument("--skip-install", action="store_true")
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--build-dir", type=Path, default=Path("build/prerelease"))
    arguments = parser.parse_args()

    env = os.environ.copy()
    env.update(
        {
            "AETHER_LOW_MEMORY": "1",
            "AETHER_NATIVE_OPTIMIZED": "0",
            "CMAKE_BUILD_PARALLEL_LEVEL": "1",
            "CTEST_PARALLEL_LEVEL": "1",
        }
    )
    if not arguments.skip_install:
        run(
            [sys.executable, "-m", "pip", "install", "-e", ".", "--no-build-isolation"],
            env,
        )

    cmake = cmake_command()
    build_dir = str(arguments.build_dir)
    configure = cmake + [
        "-S",
        ".",
        "-B",
        build_dir,
        "-DAETHER_LOW_MEMORY=ON",
        "-DAETHER_LIMIT_BUILD_PARALLELISM=ON",
        "-DAETHER_BUILD_TESTS=ON",
        "-DAETHER_BUILD_PYTHON=OFF",
    ]
    if arguments.sanitizers:
        if os.name == "nt":
            raise RuntimeError("the ASan/UBSan profile requires Clang or GCC, not MSVC")
        configure.append("-DAETHER_ENABLE_SANITIZERS=ON")
    run(configure, env)
    run(cmake + ["--build", build_dir, "--parallel", "1", "--config", "Release"], env)
    ctest = shutil.which("ctest")
    if ctest is None:
        candidates = [Path(sys.executable).with_name("ctest")]
        if len(cmake) == 1:
            candidates.append(Path(cmake[0]).with_name("ctest"))
        ctest = next((str(candidate) for candidate in candidates if candidate.exists()), None)
    if ctest is None:
        raise RuntimeError("CTest is required for release verification")
    run([ctest, "--test-dir", build_dir, "-C", "Release", "--output-on-failure"], env)
    run([sys.executable, "-m", "pytest", "-v"], env)
    verify_metadata()
    run([sys.executable, "benchmarks/bench_simd_quantizer.py"], env)
    verify_simd_results()

    if arguments.full:
        run([sys.executable, "benchmarks/fetch_usgs_data.py", "--strict"], env)
        run([sys.executable, "benchmarks/bench_rigorous.py"], env)
        run([sys.executable, "benchmarks/bench_random_access.py"], env)
        run([sys.executable, "generate_assets.py"], env)

    print("AetherStream 0.0.1 pre-release verification passed.")


if __name__ == "__main__":
    main()
