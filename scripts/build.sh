#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ORIGINAL_ARGS=("$@")
BUILD_DIR="$ROOT/build"
JOBS="${JOBS:-2}"
DRY_RUN=0
ENABLE_PAPI=0
TARGETS=(libcache libasync server run_chat_far gapbs_bfs_chunked mg
  wordcount_far wordcount_native word_length_stats kvs_throughput nhop_graph
  bandwidth_microbenchmark bandwidth_microbenchmark_512)
CMAKE_ARGS=()

usage() {
  cat <<'EOF'
Usage: bash scripts/build.sh [options] [-- extra CMake options]

Configure and build the runtime, memory server, and all bundled applications.
This script does not install packages, download inputs, or launch experiments.

  --build-dir DIR   build directory (default: <repository>/build)
  --jobs N         parallel jobs (default: JOBS or 2)
  --targets LIST   comma-separated CMake targets
  --runtime-only   build libcache, libasync, and server without application deps
  --enable-papi    opt in to optional PAPI profiling (off by default)
  --dry-run        print commands; no build directory or service is created
  -h, --help       show this help

Build limits: BUILD_CPUS=2 and BUILD_MEMORY_GIB=8 by default. The launcher
verifies inherited cgroup limits or creates a temporary systemd unit. On legacy
cgroup v1, this needs existing passwordless sudo for systemd-run; compilation
still runs as the invoking user. It never falls back to an unlimited build.

Nonstandard dependency prefixes: BOOST_ROOT, BOOST_LIBRARYDIR,
HDR_HISTOGRAM_PREFIX, or extra -D options after --.
EOF
}

die() { printf 'build: %s\n' "$*" >&2; exit 2; }
need_value() { [[ $# -ge 2 && -n "$2" ]] || die "$1 needs a value"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) need_value "$@"; BUILD_DIR=$2; shift 2 ;;
    --jobs|-j) need_value "$@"; JOBS=$2; shift 2 ;;
    --targets)
      need_value "$@"
      [[ "$2" != ,* && "$2" != *, && "$2" != *,,* ]] || die 'empty target in --targets'
      IFS=',' read -r -a TARGETS <<< "$2"
      shift 2 ;;
    --runtime-only) TARGETS=(libcache libasync server); shift ;;
    --enable-papi) ENABLE_PAPI=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; CMAKE_ARGS=("$@"); break ;;
    *) die "unknown option: $1 (use --help)" ;;
  esac
done

[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die '--jobs must be a positive integer'
(("${#TARGETS[@]}" > 0)) || die 'at least one target is required'
for target in "${TARGETS[@]}"; do
  [[ "$target" =~ ^[A-Za-z0-9_][A-Za-z0-9_+-]*$ ]] || die "invalid target: $target"
done

# Establish and verify process-tree limits before build preparation or CMake.
if [[ "$DRY_RUN" != 1 ]]; then
  command -v python3 >/dev/null || die 'python3 is required for build resource limits'
  if [[ "${STARFISH_BUILD_PAYLOAD:-}" != 1 ]]; then
    exec python3 -B "$ROOT/scripts/build_resources.py" -- \
      bash "$ROOT/scripts/build.sh" "${ORIGINAL_ARGS[@]}"
  fi
  python3 -B "$ROOT/scripts/build_resources.py" --verify
fi

if [[ "$BUILD_DIR" != /* ]]; then BUILD_DIR="$PWD/$BUILD_DIR"; fi
if [[ "$DRY_RUN" != 1 ]]; then
  mkdir -p "$BUILD_DIR"
  BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
  case "$BUILD_DIR/" in
    "$ROOT/"|"$ROOT/src/"*|"$ROOT/include/"*|"$ROOT/apps/"*|"$ROOT/tests/"*|"$ROOT/third_party/"*|"$ROOT/scripts/"*|"$ROOT/cmake/"*)
      die 'choose a separate build directory, not a source directory' ;;
  esac
  exec > >(tee -a "$BUILD_DIR/build.log") 2>&1
  printf '\n=== Starfish build: %s ===\n' "$(date -Is)"
  printf 'invocation:'; printf ' %q' bash "$ROOT/scripts/build.sh" "${ORIGINAL_ARGS[@]}"; printf '\n'
  python3 -B "$ROOT/scripts/build_resources.py" --verify
fi

APP_OPTIONS=(-DFARLIB_BUILD_APPLICATIONS=OFF -DFARLIB_BUILD_TESTS=OFF
  -DFARLIB_BUILD_MG=OFF -DFARLIB_BUILD_WORDCOUNT=OFF -DFARLIB_BUILD_WORDCOUNT_RECOVERY=OFF
  -DFARLIB_BUILD_NQ=OFF -DFARLIB_BUILD_KVS=OFF
  -DFARLIB_BUILD_OBJECT_SIZE=OFF -DFARLIB_BUILD_BANDWIDTH_MICROBENCHMARK=OFF)
APPLICATIONS=OFF
for target in "${TARGETS[@]}"; do
  case "$target" in
    libcache|libasync|server) ;;
    *) APPLICATIONS=ON ;;
  esac
  case "$target" in
    mg) APP_OPTIONS+=(-DFARLIB_BUILD_MG=ON) ;;
    kvs_throughput) APP_OPTIONS+=(-DFARLIB_BUILD_KVS=ON) ;;
    nhop_graph) APP_OPTIONS+=(-DFARLIB_BUILD_NQ=ON) ;;
    wordcount_far|wordcount_native|word_length_stats)
      APP_OPTIONS+=(-DFARLIB_BUILD_WORDCOUNT=ON) ;;
    wordcount_recovery)
      APP_OPTIONS+=(-DFARLIB_BUILD_WORDCOUNT=ON -DFARLIB_BUILD_WORDCOUNT_RECOVERY=ON) ;;
    bandwidth_microbenchmark|bandwidth_microbenchmark_512|object_size|object_size_512)
      APP_OPTIONS+=(-DFARLIB_BUILD_BANDWIDTH_MICROBENCHMARK=ON) ;;
  esac
done
APP_OPTIONS+=(-DFARLIB_BUILD_APPLICATIONS="$APPLICATIONS")

GENERATOR=()
COMPILERS=()
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  if [[ -z "${CMAKE_GENERATOR:-}" ]] && command -v ninja >/dev/null; then GENERATOR=(-G Ninja); fi
  if [[ -n "${CC:-}" ]]; then COMPILERS+=(-DCMAKE_C_COMPILER="$CC")
  elif command -v gcc-13 >/dev/null; then COMPILERS+=(-DCMAKE_C_COMPILER=gcc-13); fi
  if [[ -n "${CXX:-}" ]]; then COMPILERS+=(-DCMAKE_CXX_COMPILER="$CXX")
  elif command -v g++-13 >/dev/null; then COMPILERS+=(-DCMAKE_CXX_COMPILER=g++-13); fi
elif [[ "$DRY_RUN" != 1 ]]; then
  cached_root=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")
  [[ "$cached_root" = "$ROOT" ]] || die 'build directory belongs to another source tree'
fi

DEPENDENCIES=()
[[ -z "${BOOST_ROOT:-}" ]] || DEPENDENCIES+=(-DBOOST_ROOT="$BOOST_ROOT")
[[ -z "${BOOST_LIBRARYDIR:-}" ]] || DEPENDENCIES+=(-DBOOST_LIBRARYDIR="$BOOST_LIBRARYDIR")
if [[ "$APPLICATIONS" = ON && -n "${HDR_HISTOGRAM_PREFIX:-}" ]]; then
  found=0
  for libdir in lib lib64; do
    hdr_dir="$HDR_HISTOGRAM_PREFIX/$libdir/cmake/hdr_histogram"
    if [[ -f "$hdr_dir/hdr_histogram-config.cmake" ]]; then
      DEPENDENCIES+=(-Dhdr_histogram_DIR:PATH="$hdr_dir"); found=1; break
    fi
  done
  [[ "$found" = 1 ]] || die "HdrHistogram CMake package not found under $HDR_HISTOGRAM_PREFIX"
fi
if [[ "$ENABLE_PAPI" = 1 ]]; then
  PAPI_OPTIONS=(-UPAPI_INCLUDE_DIR -UPAPI_LIBRARY)
else
  PAPI_OPTIONS=(-DPAPI_INCLUDE_DIR:PATH=OFF -DPAPI_LIBRARY:FILEPATH=OFF)
fi

configure=(cmake -S "$ROOT" -B "$BUILD_DIR" "${GENERATOR[@]}"
  "${COMPILERS[@]}" "${DEPENDENCIES[@]}" "${PAPI_OPTIONS[@]}"
  "${APP_OPTIONS[@]}" "${CMAKE_ARGS[@]}")
build=(cmake --build "$BUILD_DIR" --target "${TARGETS[@]}" --parallel "$JOBS")
printf 'host: %s\nsource: %s\nbuild: %s\ntargets: %s\njobs: %s\nscope: %s\n' \
  "$(hostname)" "$ROOT" "$BUILD_DIR" "${TARGETS[*]}" "$JOBS" "${STARFISH_BUILD_UNIT:-inherited}"
printf 'configure:'; printf ' %q' "${configure[@]}"; printf '\n'
printf 'build:'; printf ' %q' "${build[@]}"; printf '\n'
[[ "$DRY_RUN" != 1 ]] || exit 0

command -v cmake >/dev/null || die 'CMake is required; install the prerequisites in README.md'
"${configure[@]}"
if [[ "$ENABLE_PAPI" = 1 ]]; then
  grep -Eq '^PAPI_INCLUDE_DIR:PATH=.+$' "$BUILD_DIR/CMakeCache.txt" &&
    ! grep -Eq '^PAPI_(INCLUDE_DIR|LIBRARY):[^=]+=(OFF|.*NOTFOUND)$' "$BUILD_DIR/CMakeCache.txt" ||
    die 'PAPI was requested but not found'
fi
"${build[@]}"
printf 'build: PASS\nBinaries: %s/server and %s/benchmark/\nLog: %s/build.log\n' \
  "$BUILD_DIR" "$BUILD_DIR" "$BUILD_DIR"
