# syntax=docker/dockerfile:1.7
FROM silkeh/clang:22-bookworm AS build

ENV DEBIAN_FRONTEND=noninteractive \
    CC=clang \
    CXX=clang++ \
    AETHER_LOW_MEMORY=1 \
    CMAKE_BUILD_PARALLEL_LEVEL=1
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build python3.11 python3.11-dev python3.11-venv python3-pip ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY requirements*.txt ./
RUN python3.11 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir --upgrade pip \
    && /opt/venv/bin/pip install --no-cache-dir -r requirements-dev.txt \
    && /opt/venv/bin/pip install --no-cache-dir build
COPY . .
RUN /opt/venv/bin/pip install . --no-build-isolation \
    && /opt/venv/bin/python -m pytest -v tests
RUN cmake -S . -B build-test -G Ninja \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release \
      -DAETHER_BUILD_PYTHON=OFF \
      -DAETHER_BUILD_TESTS=ON \
      -DAETHER_LOW_MEMORY=ON \
    && cmake --build build-test --parallel 1 \
    && ctest --test-dir build-test --output-on-failure
RUN MPLBACKEND=Agg /opt/venv/bin/python bench.py \
    && /opt/venv/bin/python generate_assets.py \
    && /opt/venv/bin/python -m build --wheel --no-isolation --outdir /dist

FROM python:3.11-slim-bookworm AS runtime
COPY --from=build /dist /dist
RUN python -m pip install --no-cache-dir /dist/*.whl && rm -rf /dist
WORKDIR /work
ENTRYPOINT ["python"]
CMD ["-c", "import aetherstream as a; print('AetherStream', a.__version__)"]
