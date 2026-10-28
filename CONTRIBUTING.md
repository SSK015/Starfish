# Contributing to Starfish

Contributions to runtime performance, applications, testing, and reproducibility
are welcome. Keep each change focused and explain how it was checked.

## Header layout

Application includes keep their existing `cache/`, `data_structure/`, `async/`,
`rdma/`, and `utils/` paths. The `runtime/include/internal/` subtree groups
implementation helpers by responsibility:

| Directory | Responsibility |
| --- | --- |
| `cache/` | Allocator and cache paths, completion handling, and cache hooks |
| `placement/` | Remote-backup budgets, resident placement, and region heat/budgets |
| `behavior/` | Object-group classification/refinement and EC phase policy |
| `recovery/` | Read recovery, background rebuilding, and recomputation |
| `profiling/` | Timing, counters, sampled diagnostics, and memory reporting |

Compiled implementations use the corresponding module paths under
`runtime/src/`, including `internal/placement/`, `internal/profiling/`, and
`internal/cache/rdma/`. Header-only modules do not need a source directory.

Runtime code and tests include these implementation headers through
`internal/...` paths. Template and inline implementations remain in headers.
The application-facing `utils/{perf,stats,wait_trace,scope_diag,request_interval_diag}.hpp`
and `runtime_common/benchmark_memory.hpp` remain as forwarding headers.
They preserve existing application includes without duplicating implementation.

KVS keeps its application-specific fixed-size string in
`apps/kvs/include/test/fixed_size_string.hpp`. Its app-local include directory
takes precedence over runtime headers.

## C++ formatting

Use **clang-format 20**, Google style, **2-space indentation**, and an **80-column
limit**. The root `.clang-format` is the single formatting configuration.
Long comments and string literals are preserved rather than rewritten.
Macro bodies are also preserved because paired BEGIN/END macros split braces
across definitions.

```bash
bash scripts/format.sh --list   # Show the maintained C++ scope.
bash scripts/format.sh          # Format it.
bash scripts/format.sh --check  # Check without changing files.
```

Set `CLANG_FORMAT` if the version-20 executable has a different name or path.

The maintained scope is tracked `.h`, `.hpp`, `.cc`, `.cpp`, and `.ipp` files
under `runtime/include/`, `runtime/src/`, and `tests/`. Header include paths
are relative to `runtime/include/`; existing includes such as
`#include "cache/cache.hpp"` stay the same. These are excluded:

- `apps/`: retained application and upstream sources.
- `third_party/`: pinned dependencies and their own licenses.
- `runtime/include/utils/zipfian.hpp`: the Microsoft MIT-licensed upstream generator.
- Build output and generated files.

Keep include order, using-declaration order, comments, and string literals intact.
Macro-driven includes and implementation fragments can depend on their order.
Formatting does not rename APIs, move files, or change algorithms, object layouts,
memory ordering, thread placement, configuration keys, or defaults. Submit such
changes separately.

## Build and test

Install the prerequisites in [README.md](README.md) first. From the repository
root, the following keeps configuration, compilation, and registered CPU tests
inside one verified CPU/memory-limited process tree:

```bash
python3 -B scripts/build_resources.py -- bash -c '
  set -euo pipefail
  bash scripts/build.sh -- -DFARLIB_BUILD_TESTS=ON
  cmake --build build --parallel 2
  ctest --test-dir build --parallel 2 --output-on-failure
'
```

The launcher defaults to a two-CPU quota and an 8 GiB aggregate memory limit.
Use the dependency-prefix options described in the README when needed.

Registered CTests run offline without memory servers. The default configuration
skips the test for disabled eviction-breakdown instrumentation. Networked test
executables are not registered for automatic execution; run them separately with
the physical machines and site configuration described in the
[deployment guide](docs/DEPLOYMENT.md).
