# Raspberry Pi OS (bookworm) toolchain: gcc 12, clang 14, cmake 3.25, zlib. Used by tools/bench.sh.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends g++ clang cmake make zlib1g-dev python3 procps linux-perf \
    && rm -rf /var/lib/apt/lists/*
