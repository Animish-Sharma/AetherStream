# Research artifact

The academic artifact separates analytical bounds, measured codec results,
hardware measurements, and modeled operation counts. Generated benchmark data
and PDFs are intentionally excluded from Git; every figure can be regenerated.

## Numerical metrics

```bash
python benchmarks/theoretical_bounds.py
python benchmarks/spectral_metrics.py
pytest tests/test_research_metrics.py -v
```

The theoretical script uses the normalized GED density

\[
f(x)=\frac{\beta}{2\alpha\Gamma(1/\beta)}
\exp[-(|x|/\alpha)^\beta].
\]

Consequently its entropy is
\(1/\beta-\ln\beta+\ln(2\alpha)+\ln\Gamma(1/\beta)\). The factor two is
already included in the normalization; adding a second `ln(2)` would fail the
Gaussian rate-distortion identity. Entropy is retained in nats until the final
conversion to bits.

The spectral script runs on the downloaded Ridgecrest trace when
`benchmarks/data/ridgecrest_strong_motion.npy` is present and otherwise uses a
deterministic resonance trace. It reports Welch log-spectral distance,
rate-matched uniform-quantizer LSD, normalized cross-correlation delay, and top
one-percent peak-amplitude error.

## Hardware counters

```bash
python benchmarks/profile_hardware.py --peak-gflops 250
```

On Linux, the harness requests cycles, instructions, L1 data-cache accesses and
misses, branches, and branch misses from `perf stat`. It records unavailable
individual counters. When `perf_event_paranoid` or a virtualized runner blocks
access, it reports the restriction and uses monotonic codec timing plus a
working-set memory-copy bandwidth estimate. Arithmetic intensity uses an
explicit operation-count model; change it with `--operations-per-sample`.

## Figures and manuscript

```bash
python benchmarks/generate_paper_plots.py
make -C paper PYTHON="$(pwd)/.venv/bin/python"
```

The plotting command writes three vector PDFs to `paper/figures/` at IEEE
single-column width. Rate-distortion points include standard-error bars across
five seeded traces. Comparator points are read from successful runs in
`benchmarks/results/benchmark_results.json`; absent SZ3 results are identified
rather than fabricated. Use `--strict-comparators` to require an official SZ3
run before producing the rate-distortion figure.

The Makefile uses `latexmk` when available and otherwise executes the complete
`pdflatex`/`bibtex` sequence. It selects `IEEEtran` when installed and retains a
two-column `article` fallback for artifact smoke tests.
