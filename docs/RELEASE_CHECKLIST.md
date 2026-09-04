# AetherStream 0.0.1 Pre-release Checklist

A release is ready only when every locally actionable item below is checked and
the remote CI/publishing items are confirmed on their target services.

## 1. Reproducible local verification

All local builds retain the conservative profile: `-O1 -g0 -fno-lto`, one
compile job, and no host-native tuning. The dependency-free runner uses argument
lists rather than shell syntax and therefore works from Linux/macOS shells,
Windows PowerShell, and `cmd.exe`:

```text
python scripts/verify_release.py --full
```

For Clang/GCC sanitizer validation, run separately:

```text
python scripts/verify_release.py --sanitizers --skip-install --build-dir build/prerelease-asan
```

The full command must complete all of the following:

- editable installation without build isolation;
- one-job CMake configure/build and native CTest;
- complete Python test suite;
- installed version and wire-v6 indexed smoke test;
- six-segment SIMD quantizer benchmark and enforced quality gates;
- strict live MiniSEED retrieval, including CI.CCC.HNE Ridgecrest data;
- rigorous, random-access, and asset-generation benchmarks.

The SIMD JSON must report symbol disagreement below `0.01` and polynomial PSNR
impact no worse than `-0.05 dB`. The Ridgecrest validation must report at least
one naturally triggered block and a positive measured triggered-block PSNR gain.

## 2. Wire-format migration

- Confirm newly encoded bytes contain little-endian wire version `6` at header
  offset 4.
- Confirm indexed v6 streams pass full and cached-slice decoding.
- Confirm unindexed v5 fixtures remain readable.
- Confirm indexed v5 streams raise `DeprecatedWireFormatException` with the
  documented re-encode instruction.
- Confirm versions below 5 and above 6 raise
  `UnsupportedWireFormatException`.
- Do not relabel or mutate archived v5 indexed bytes. Decode them with the
  matching historical AetherStream build, then encode the resulting float32
  samples with 0.0.1.

## 3. Cross-platform matrix

Run `python scripts/verify_release.py` in clean environments on:

- Linux x86-64 with GCC and Clang;
- Linux ARM64, confirming the reported Neon backend;
- macOS x86-64 and ARM64 with AppleClang;
- Windows x86-64 using an MSVC developer shell.

On each host record OS, architecture, compiler, Python version, selected SIMD
backend, test totals, and `benchmarks/results/simd_quantizer.json`. Conda and
vcpkg recipes must additionally be built in their native environments. Local
emulation is useful but does not replace execution on the target architecture.

## 4. Package and documentation artifacts

```text
python -m build
python -m twine check dist/*
```

- Confirm version `0.0.1` agrees in CMake, Python metadata, native module,
  Conda, vcpkg, and `CITATION.cff`.
- Inspect the sdist: source, tests, benchmarks, and `scripts/verify_release.py`
  are present; environments, fetched telemetry, results, and build trees are
  absent.
- Install each wheel in a clean environment and run the wire-v6 indexed smoke
  test.
- Build the Docker image and run native/Python tests inside its final stage.
- Build MkDocs in strict mode and verify generated assets have intentional
  diffs only.
- Publish benchmark output only together with `benchmarks/data/manifest.json`
  and host/toolchain provenance. Never report fallback data as field data.

## 5. Repository and trusted publishing

- Push the candidate commit and require successful `native`,
  `docs-and-style`, and `sanitizers-and-fuzz` checks.
- Enable branch protection, pull-request review, Dependabot security updates,
  and private vulnerability reporting.
- Create the GitHub `pypi` environment with reviewer protection.
- Configure the PyPI trusted publisher exactly as owner `animish-sharma`,
  repository `aetherstream`, workflow `wheels.yml`, environment `pypi`.
- Confirm the publish job has `id-token: write` and no long-lived PyPI token.

## 6. Release and post-release smoke test

1. Create `git tag -a v0.0.1 -m "AetherStream 0.0.1"` and push the tag.
2. Create the GitHub release from the 0.0.1 changelog section.
3. Approve the protected publishing environment after all wheels and the sdist
   are present.
4. Verify files and metadata at
   `https://pypi.org/project/aetherstream/0.0.1/`.
5. In a new environment, install from PyPI, assert `__version__ == "0.0.1"`,
   encode an indexed frame, assert wire version 6, and compare cached slice
   output bit-for-bit with full decompression.
6. Record remote CI URLs and artifact hashes in the GitHub release notes.
