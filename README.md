# Starfish Runtime and Applications

Starfish is a fault-tolerant far-memory runtime from
**Starfish: Fault-Tolerant Far Memory with Low Resource and Performance Overhead**
(ATC 2026).

By Yanwen Xia, Benyong Deng, Hong Huang, Yuzheng Wang, Quanxi Li, Xingda Wei,
Xiaobing Feng, Huimin Cui, and Chenxi Wang.

## Scope

We plan to keep developing and maintaining this standalone repository over the
long term. It contains the Starfish runtime, its memory server, existing runtime
tests, application source code under `apps/`, and the pinned libfibre source
with its errnoname dependency under `third_party/`. One normal clone includes
these sources; no recursive clone is needed. It does not contain the Non-FT,
Hydra-like or Carbink-like baselines, input datasets, model weights, or paper-level
experiment scripts.

For the preserved ATC 2026 artifact and complete reproduction instructions, see
[ICTPLSys/Starfish](https://github.com/ICTPLSys/Starfish).
The [official AE results](https://sysartifacts.github.io/atc2026/results) apply to
that complete artifact, not to a separate evaluation of this maintained repository.

## Build the runtime

Use Linux with CMake 3.16 or newer, a C++20 compiler with `std::format` support
(e.g., GCC 13), Boost program_options, libibverbs development files, and ISA-L
development files. PAPI is optional. CMake builds the bundled libfibre source
automatically; neither a separate libfibre checkout nor `LIBFIBRE_DIR` is needed.
Generated headers and libraries stay in the build directory. No input datasets
are needed to build the runtime libraries and memory server.

```bash
cmake -S . -B build \
  -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
  -DFARLIB_BUILD_APPLICATIONS=OFF
cmake --build build --target libcache libasync server -j
```

The source is self-contained, but the compiler, CMake, and the system development
libraries listed above are not bundled. The default configure/build does not
download source code. To use a separately built custom libfibre instead, pass
`-DLIBFIBRE_DIR=/absolute/path/to/libfibre` explicitly. The old environment
variable alone no longer selects an external checkout.

Running the system requires separate compute and memory servers with RDMA
connectivity and an appropriate site configuration. A successful build alone
does not establish end-to-end correctness or reproduce the paper's results.

## Build the applications

Application builds additionally require the HdrHistogram CMake package and OpenSSL.
The bundled `apps/` directory is used by default; no AE checkout is needed.
Applications are disabled by default. Enable them and the optional targets with:

```bash
cmake -S . -B build-apps \
  -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
  -DFARLIB_BUILD_APPLICATIONS=ON \
  -DFARLIB_BUILD_MG=ON \
  -DFARLIB_BUILD_WORDCOUNT=ON \
  -DFARLIB_BUILD_NQ=ON \
  -DFARLIB_BUILD_KVS=ON \
  -DFARLIB_BUILD_BANDWIDTH_MICROBENCHMARK=ON
cmake --build build-apps -j
```

| Source directory | Executable targets |
| --- | --- |
| `apps/llama/` | `run_chat_far` |
| `apps/bfs/` | `gapbs_bfs_chunked` |
| `apps/mg/` | `mg` |
| `apps/wordcount/` | `wordcount_far`, `wordcount_native`, `word_length_stats` |
| `apps/nq/` | `nhop_graph` |
| `apps/kvs/` | `kvs_throughput` |
| `apps/bandwidth_microbenchmark/` | `bandwidth_microbenchmark`, `bandwidth_microbenchmark_512` |

LLaMA and BFS are included whenever application builds are enabled. The other
applications require their corresponding options above. An external application
source tree can still be selected with `-DFARLIB_APPS_DIR=/absolute/path/to/apps`.

Input graphs, text datasets, model weights, and generated correctness fixtures
are not bundled. Wordcount's fixture generator remains in
`apps/wordcount/testdata/generate_small_oracle.py`.
For workload inputs, site configuration, and full paper experiments, use the
original AE repository. Building these targets does not run experiments.

Existing runtime tests are available with `-DFARLIB_BUILD_TESTS=ON`; some tests
require RDMA hardware, separate physical servers, and a valid site configuration.

## Source and license

The initial runtime source is extracted from
[ICTPLSys/Starfish at bb028eef7878c2abc90c32c7353e58a8b5ade562](https://github.com/ICTPLSys/Starfish/tree/bb028eef7878c2abc90c32c7353e58a8b5ade562/artifact-evaluation/runtime/starfish),
plus the four headers from `artifact-evaluation/runtime/common/`. Application
sources come from `artifact-evaluation/apps/` at that same revision, excluding
generated fixtures and the unused chat input file. Include paths, standalone build
configuration, and documentation are adapted for this layout; the application
algorithms and the original AE source tree are unchanged.

Starfish source uses [Apache-2.0](LICENSE). Third-party notices are retained,
including the [MIT license for the LLaMA inference code](apps/llama/LICENSE)
and the NAS Parallel Benchmarks notices in `apps/mg/`. Bundled libfibre retains
its GPL license, and errnoname retains its separate license; see
[third-party sources and licenses](third_party/README.md). External dependencies
retain their own licenses. No dependency binaries are bundled here.
