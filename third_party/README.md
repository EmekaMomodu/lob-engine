# Third-party sources

Unmodified upstream release archives, used only to build the tests and
benchmarks. `cmake/Dependencies.cmake` unpacks them into the build tree and
checks each SHA-256 first. Nothing here is compiled into the `lob` library.

| File | Upstream | Version | License | SHA-256 |
|---|---|---|---|---|
| `googletest-1.17.0.tar.gz` | https://github.com/google/googletest | v1.17.0 | BSD-3-Clause | `65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c` |
| `benchmark-1.9.4.tar.gz` | https://github.com/google/benchmark | v1.9.4 | Apache-2.0 | `b334658edd35efcf06a99d9be21e4e93e092bd5f95074c1673d5c8705d95c104` |

Each archive carries its own `LICENSE` file. To upgrade, replace the archive
and update the file name and `URL_HASH` in `cmake/Dependencies.cmake`.
