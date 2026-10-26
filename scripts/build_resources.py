#!/usr/bin/env python3
"""Run the entire build under verified CPU and aggregate-memory cgroup limits."""

import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

GIB = 1024 ** 3


def ancestors(root, relative):
    root = Path(root).resolve()
    path = (root / relative.lstrip("/")).resolve()
    if path != root and root not in path.parents:
        return
    while True:
        yield path
        if path == root:
            break
        path = path.parent


def read(path):
    try:
        return path.read_text().strip()
    except (OSError, ValueError):
        return ""


def effective_limits():
    groups = {}
    for line in Path("/proc/self/cgroup").read_text().splitlines():
        _, controllers, path = line.split(":", 2)
        for controller in controllers.split(","):
            groups[controller] = path
    cpus, memory, swaps = [], [], []
    swap_supported = False
    legacy_cpu = "cpu" in groups
    if legacy_cpu:
        for root in ("/sys/fs/cgroup/cpu,cpuacct", "/sys/fs/cgroup/cpuacct,cpu", "/sys/fs/cgroup/cpu"):
            for path in ancestors(root, groups["cpu"]):
                quota = read(path / "cpu.cfs_quota_us")
                period = read(path / "cpu.cfs_period_us")
                if quota and period and int(quota) > 0:
                    cpus.append(int(quota) / int(period))
    if "memory" in groups:
        for path in ancestors("/sys/fs/cgroup/memory", groups["memory"]):
            value = read(path / "memory.limit_in_bytes")
            if value and 0 < int(value) < 2 ** 60:
                memory.append(int(value))
    if "" in groups:
        for root in ("/sys/fs/cgroup", "/sys/fs/cgroup/unified"):
            for path in ancestors(root, groups[""]):
                quota = read(path / "cpu.max").split()
                if len(quota) == 2 and quota[0] != "max":
                    cpus.append(int(quota[0]) / int(quota[1]))
                value = read(path / "memory.max")
                if value and value != "max":
                    memory.append(int(value))
                swap_file = path / "memory.swap.max"
                swap_supported = swap_supported or swap_file.exists()
                value = read(swap_file)
                if value and value != "max":
                    swaps.append(int(value))
    return {
        "host": socket.gethostname(),
        "pid": os.getpid(),
        "cpu_limit": min(cpus) if cpus else None,
        "memory_limit_bytes": min(memory) if memory else None,
        "swap_limit_bytes": min(swaps) if swaps else None,
        "swap_control_supported": swap_supported,
        "host_swap_total": next((line.strip() for line in Path("/proc/meminfo").read_text().splitlines()
                                 if line.startswith("SwapTotal:")), "unknown"),
        "swap_note": ("v2 swap controller detected" if swap_supported
                      else "v2 swap controller unavailable; cgroup v1 swap control not verified"),
        "cgroups": groups,
        "legacy_cpu": legacy_cpu,
    }


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--cpus", type=positive, default=os.environ.get("BUILD_CPUS", "2"))
    parser.add_argument("--memory-gib", type=positive, default=os.environ.get("BUILD_MEMORY_GIB", "8"))
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command[:1] == ["--"]:
        args.command.pop(0)
    if sys.platform != "linux":
        parser.error("the build resource launcher requires Linux")
    limits = effective_limits()
    bounded = (
        limits["cpu_limit"] is not None
        and 0 < limits["cpu_limit"] <= args.cpus
        and limits["memory_limit_bytes"] is not None
        and (not limits["swap_control_supported"] or limits["swap_limit_bytes"] == 0)
        and 0 < limits["memory_limit_bytes"] <= args.memory_gib * GIB
    )
    if args.verify:
        print("build resources: " + json.dumps(limits, sort_keys=True), flush=True)
        if not bounded:
            print("CPU/memory limits are missing or exceed the budget, or supported swap control is not zero.", file=sys.stderr)
            return 78
        return 0
    if not args.command:
        parser.error("a build command is required after --")
    if bounded:
        env = os.environ.copy()
        env["STARFISH_BUILD_PAYLOAD"] = "1"
        os.execvpe(args.command[0], args.command, env)
    if not shutil.which("systemd-run"):
        parser.error("provide a CPU/memory-limited cgroup scope; systemd-run is unavailable")
    unit = "starfish-build-" + str(os.getpid()) + "-" + str(time.monotonic_ns())
    uid, gid = os.getuid(), os.getgid()
    if limits["legacy_cpu"]:
        # Legacy user managers often accept properties without enforcing them.
        # Use the system manager, but never run the compiler as root.
        prefix = [] if uid == 0 else ["sudo", "-n"]
        if prefix and not shutil.which("sudo"):
            parser.error("legacy cgroup v1 needs an administrator-provided limited scope")
        launch = prefix + ["systemd-run"]
        properties = ["-p", "User=" + str(uid), "-p", "Group=" + str(gid)]
    else:
        launch = ["systemd-run", "--user"] if uid else ["systemd-run"]
        properties = []
    launch += [
        "--wait", "--pipe", "--collect", "--unit=" + unit,
        "-p", "CPUQuota=" + str(args.cpus * 100) + "%",
        "-p", "MemoryMax=" + str(args.memory_gib * GIB),
        "-p", "MemorySwapMax=0", "-p", "TasksMax=512",
        "-p", "WorkingDirectory=" + os.getcwd(),
    ] + properties
    forward = (
        "PATH", "HOME", "CC", "CXX", "JOBS", "CMAKE_PREFIX_PATH", "CMAKE_GENERATOR",
        "BOOST_ROOT", "BOOST_INCLUDEDIR", "BOOST_LIBRARYDIR", "HDR_HISTOGRAM_PREFIX",
        "BUILD_CPUS", "BUILD_MEMORY_GIB",
    )
    for name in forward:
        if name in os.environ:
            launch.append("--setenv=" + name + "=" + os.environ[name])
    launch += ["--setenv=STARFISH_BUILD_PAYLOAD=1", "--setenv=STARFISH_BUILD_UNIT=" + unit]
    launch += args.command
    print(
        "Creating a temporary limited build unit: " + unit
        + " (CPU=" + str(args.cpus) + ", memory=" + str(args.memory_gib)
        + " GiB, payload uid=" + str(uid) + ").", flush=True,
    )
    print("No packages are installed; an unenforced scope aborts before CMake.", flush=True)
    return subprocess.call(launch)


if __name__ == "__main__":
    sys.exit(main())
