#!/usr/bin/env bash
# Run a command in the Linux build container with the repo mounted at /src.
#
#   docker/run.sh                                  # interactive shell
#   docker/run.sh cmake --workflow --preset release
#   CXX=clang++-18 CC=clang-18 docker/run.sh cmake --workflow --preset asan-ubsan
#
# /src/build is a separate Docker volume, so Linux build trees never collide
# with the macOS ones in ./build on the host.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="lob-engine-dev"

docker build --quiet -t "$image" "$repo_root/docker" >/dev/null

tty_flags=()
[[ -t 0 && -t 1 ]] && tty_flags=(-it)

env_flags=()
[[ -n "${CC:-}" ]] && env_flags+=(-e "CC=$CC")
[[ -n "${CXX:-}" ]] && env_flags+=(-e "CXX=$CXX")

# One build volume per compiler: CMake refuses to switch compilers in an
# existing build tree. ("clang++-18" -> "lob-engine-build-clangpp-18")
cxx_tag="${CXX:-default}"
cxx_tag="${cxx_tag//+/p}"

# The ${arr[@]+...} form keeps empty arrays safe under `set -u` on macOS bash 3.2.
exec docker run --rm ${tty_flags[@]+"${tty_flags[@]}"} ${env_flags[@]+"${env_flags[@]}"} \
    --cap-add SYS_PTRACE \
    -v "$repo_root:/src" \
    -v "lob-engine-build-$cxx_tag:/src/build" \
    "$image" "${@:-bash}"
