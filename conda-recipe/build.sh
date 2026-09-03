#!/usr/bin/env bash
set -euxo pipefail
export AETHER_LOW_MEMORY="${AETHER_LOW_MEMORY:-1}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-1}"
"${PYTHON}" -m pip install . -vv --no-deps --no-build-isolation
