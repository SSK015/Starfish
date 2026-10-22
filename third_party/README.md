# Bundled dependencies

This directory contains ordinary source files, not Git submodules. Building
Starfish does not download dependency source or require an existing libfibre
checkout. Standard Linux development libraries are still required; see the
root [README](../README.md).

## libfibre

- Source: [Crazylqx/libfibre](https://github.com/Crazylqx/libfibre)
- Commit: `885d74dfb6746966a911ff6c821cc4a88fda3b91`
- Imported from the preserved ATC 2026 artifact at
  `bb028eef7878c2abc90c32c7353e58a8b5ade562`.
- License: upstream [GNU GPL](libfibre/LICENSE), with original source notices
  retained. The root Apache-2.0 license does not replace this license.

The core source tree, build configuration, upstream README, and license are
preserved. Upstream examples, documentation tooling, and compiler/kernel patches
are not included. CMake builds the core library and generates its configuration
header under the build directory; no generated library is committed.

CMake keeps the AE snapshot's tracked `src/runtime/testoptions.h` unchanged,
with libfibre assertions, statistics, and debugging disabled. Only
`runtime-glue/testoptions.h` is generated from its bundled template.
Optional tracing, split-stack, and io_uring modes are not enabled. The preserved
upstream README also describes examples that are outside this core-only export.

## errnoname

- Source: [mentalisttraceur/errnoname](https://github.com/mentalisttraceur/errnoname)
- Commit: `610a51162641f21fcac2ba812bc1996da11525f1`
- Included as ordinary files in `libfibre/src/errnoname/`, including its
  separate [license](libfibre/src/errnoname/LICENSE).

`libfibre.vendor.json` records the exported files, their upstream blob IDs,
hashes, and modes. The optional upstream example-only `picohttpparser`
submodule is not needed by the core library and is not included.
