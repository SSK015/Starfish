# Contributing to Starfish

Contributions to runtime performance, applications, testing, and reproducibility
are welcome. Keep each change focused and explain how it was checked.

## Module layout

Canonical headers and inline implementation fragments live under
`runtime/include/`. Compiled implementations use matching module paths under
`runtime/src/`; header-only modules do not need empty source directories.

| Module | Responsibility |
| --- | --- |
| `mm/cache/` | Cache facade, accessors, object state, fetch and completion handling |
| `mm/allocator/` | Object/region allocation, local and remote heaps, return queues |
| `mm/evacuator/` | Marking, eviction, evacuation and commit bookkeeping |
| `mm/placement/` | Resident placement, region heat, grouping and backup budgets |
| `ft/ec/` | Codecs, stripes, layouts, batching, update coordination and protocols |
| `ft/policy/` | Behavior-guided protection selection |
| `ft/recovery/` | Read recovery, scratch state and background rebuilding |
| `ft/recompute/` | Recipe registration and recomputation recovery |
| `transport/rdma/` | RDMA connections, queues and verbs-facing EC adapters |
| `async/` | Fibres, threads, fork/join, signals and yielding locks |
| `data_structure/` | Application data structures and their template implementations |
| `profiling/` | Timing, counters, sampled diagnostics and memory reporting |
| `lib/` | Shared helpers, timers and runtime lifecycle control |

Runtime code and tests use these canonical paths and the `starfish`
namespace. The layout groups files by ownership without changing runtime
behavior or linkage boundaries. Executable targets and output paths are retained.

`.ipp` files stay with the subsystem that owns them. Template/inline fragments
remain included by their headers; the existing EC update/RMW fragments remain
included by their compiled coordinator. Include order and existing guards are
preserved.

Applications and tests include canonical module paths directly. Earlier
include spellings have been replaced as follows:

| Previous include | Canonical include |
| --- | --- |
| `cache/cache.hpp` | `mm/cache/cache.hpp` |
| `rdma/client.hpp` | `transport/rdma/client.hpp` |
| `utils/uthreads.hpp` | `async/uthreads.hpp` |
| `utils/perf.hpp` | `profiling/perf.hpp` |
| `utils/control.hpp` | `lib/control.hpp` |
| `runtime_common/benchmark_memory.hpp` | `profiling/benchmark_memory.hpp` |

The previous forwarding headers have been removed. Macro-expansion lists such as
`transport/rdma/config.def` remain deliberately re-includable.

The Microsoft generator is retained unchanged at `lib/zipfian.hpp` and excluded
from formatting. KVS keeps its application-specific string at
`apps/kvs/include/test/fixed_size_string.hpp`; the app-local include directory
retains precedence.

## Runtime naming

The C++ namespace is `starfish`. Update integrations that used `FarLib`
and rebuild them; the namespace change also changes C++ symbol names. The
profiling wrapper is `starfish::profile::profile_workload`, formerly
`beehive_profile`.

Project-owned compile definitions and CMake options use `STARFISH_*`.
Legacy `-DFARLIB_*=...` cache inputs are accepted with a deprecation warning
and converted to their canonical names. Conflicting old and new values are
rejected. For an existing cache with a conflicting legacy value, remove that
entry (for example, `-UFARLIB_BUILD_TESTS`) or use a fresh build directory
before setting the canonical option.

Runtime environment variables retain their existing `FARLIB_*` spellings.
This keeps current experiment runners and their selected settings working.
Upstream LibFibre names and `LIBFIBRE_DIR` are unchanged.

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
`#include "mm/cache/cache.hpp"` use the module paths above. These are excluded:

- `apps/`: retained application and upstream sources.
- `third_party/`: pinned dependencies and their own licenses.
- `runtime/include/lib/zipfian.hpp`: the Microsoft MIT-licensed upstream generator.
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
  bash scripts/build.sh -- -DSTARFISH_BUILD_TESTS=ON
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
