# Linux build/profiling environment.
#
# Use for: GCC builds, Linux sanitizer runs, and perf. Note that on a macOS
# host Docker runs inside a Linux VM, so `perf` here has no access to hardware
# PMU counters and timings include virtualisation noise. Final benchmark and
# profiling numbers come from a bare-metal Linux box, not this container.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        g++-14 \
        clang-18 \
        libclang-rt-18-dev \
        cmake \
        ninja-build \
        git \
        ca-certificates \
        gdb \
        python3 \
        linux-tools-common \
        linux-tools-generic \
    && rm -rf /var/lib/apt/lists/*

ENV CC=gcc-14 \
    CXX=g++-14

WORKDIR /src
