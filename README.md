# Starfish Runtime

Starfish is a fault-tolerant far-memory runtime from
**Starfish: Fault-Tolerant Far Memory with Low Resource and Performance Overhead**
(ATC 2026).

By Yanwen Xia, Benyong Deng, Hong Huang, Yuzheng Wang, Quanxi Li, Xingda Wei,
Xiaobing Feng, Huimin Cui, and Chenxi Wang.

## Scope

This repository contains the Starfish runtime, its memory server, runtime tests,
and the shared headers required by that code. It does not contain the Non-FT,
Hydra-like or Carbink-like baselines, evaluation applications, datasets, or
paper-level experiment scripts.

For the complete paper artifact and reproduction instructions, see
[ICTPLSys/Starfish](https://github.com/ICTPLSys/Starfish).
The [official AE results](https://sysartifacts.github.io/atc2026/results) apply to
that complete artifact, not to a separate evaluation of this extracted repository.

## Build the runtime

Use Linux with CMake 3.16 or newer, a C++20 compiler, Boost program_options,
libibverbs development files, and ISA-L development files. Build
[libfibre](https://git.uwaterloo.ca/mkarsten/libfibre) separately and set
`LIBFIBRE_DIR` to its source root, containing `src/libfibre.so` and the headers.
PAPI is optional. No application sources or input datasets are needed to build
the runtime libraries and memory server.

```bash
export LIBFIBRE_DIR=/absolute/path/to/libfibre
cmake -S . -B build -DFARLIB_BUILD_APPLICATIONS=OFF
cmake --build build --target libcache libasync server -j
```

Running the system requires separate compute and memory servers with RDMA
connectivity and an appropriate site configuration. A successful build alone
does not establish end-to-end correctness or reproduce the paper's results.

### Optional applications and tests

Applications remain in the complete artifact. To build them against this runtime,
add the following options to the CMake configure command:

```bash
-DFARLIB_BUILD_APPLICATIONS=ON \
  -DFARLIB_APPS_DIR=/absolute/path/to/ICTPLSys-Starfish/artifact-evaluation/apps
```

Application builds also require the HdrHistogram CMake package and OpenSSL.
Existing runtime tests are available with `-DFARLIB_BUILD_TESTS=ON`; some tests
require RDMA hardware, separate physical servers, and a valid site configuration.

## Source and license

The initial source is extracted from
[ICTPLSys/Starfish at bb028eef7878c2abc90c32c7353e58a8b5ade562](https://github.com/ICTPLSys/Starfish/tree/bb028eef7878c2abc90c32c7353e58a8b5ade562/artifact-evaluation/runtime/starfish),
plus the four headers from `artifact-evaluation/runtime/common/`. Only the shared
header include paths and standalone build configuration are adapted. The original
AE source tree remains in the complete artifact.

Starfish source uses [Apache-2.0](LICENSE). External dependencies retain their own
licenses, including GPL-3.0 for libfibre; no dependency binaries are bundled here.
