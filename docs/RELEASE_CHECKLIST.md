# v2.2 Release and Trusted Publishing Checklist

## Local repository

The repository uses `main` as its initial branch. Before adding a remote:

```bash
git status
git remote add origin git@github.com:animish-sharma/aetherstream.git
git push -u origin main
```

On GitHub, enable branch protection for `main`, require pull requests, and make
the `native`, `docs-and-style`, and `sanitizers-and-fuzz` CI checks mandatory.
Enable private vulnerability reporting and Dependabot security updates.

## PyPI OIDC trusted publisher

No long-lived PyPI API token is required. While logged in as a PyPI project
owner, open **aetherstream → Manage → Publishing**, add a GitHub publisher, and
enter exactly:

| Field | Value |
|---|---|
| Owner | `animish-sharma` |
| Repository | `aetherstream` |
| Workflow | `wheels.yml` |
| Environment | `pypi` |

For a first publication, create the same mapping as a pending publisher at
<https://pypi.org/manage/account/publishing/>. In GitHub, create the `pypi`
environment and optionally require manual reviewer approval. Do not add a PyPI
password or token secret.

The `publish` job in `.github/workflows/wheels.yml` already declares
`id-token: write`, uses the `pypi` environment, downloads all wheel/sdist
artifacts, and invokes `pypa/gh-action-pypi-publish`.

## Pre-flight

```bash
python -m pip install -r requirements-dev.txt
AETHER_LOW_MEMORY=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
  python -m pip install -e . --no-build-isolation
python -m pytest -v
cmake -S . -B build-release -DAETHER_BUILD_TESTS=ON -DAETHER_BUILD_PYTHON=OFF
cmake --build build-release --parallel 1
ctest --test-dir build-release --output-on-failure
python benchmarks/fetch_usgs_data.py --strict
python benchmarks/bench_rigorous.py
python benchmarks/bench_random_access.py
python generate_assets.py
git diff --exit-code -- assets
python -m build
python -m twine check dist/*
```

Confirm that version `2.2.0` agrees in CMake, Python metadata, Conda, vcpkg,
`CITATION.cff`, and the native module. Inspect the sdist to ensure it contains
source and tests but no build trees, virtual environments, fetched data, or
benchmark outputs.

## Release

1. Push `main` and wait for every required CI job to pass.
2. Create and push an annotated tag: `git tag -a v2.2.0 -m "AetherStream 2.2.0"`.
3. Create a GitHub release from `v2.2.0` and paste the v2.2 changelog section.
4. Publishing the release triggers `wheels.yml`; approve the `pypi` environment
   if reviewer protection is enabled.
5. Verify files and metadata at <https://pypi.org/project/aetherstream/2.2.0/>.
6. Install one wheel in a clean environment and rerun an indexed round trip.
7. Upload benchmark results only with their matching EarthScope provenance
   manifest and host/toolchain description.
