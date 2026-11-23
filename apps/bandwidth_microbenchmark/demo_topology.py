"""InfiniBand port and physical CPU selection for the bandwidth demo."""

import os
from pathlib import Path
import re


def get_port(device=None, port=None):
    ib = Path("/sys/class/infiniband")
    active = []
    for entry in sorted(ib.glob("*/ports/[0-9]*")):
        name = entry.parent.parent.name
        index = int(entry.name)
        if (device is not None and name != device) or (port is not None and index != port):
            continue
        if (entry / "link_layer").read_text().strip() != "InfiniBand":
            continue
        if (entry / "state").read_text().split(":")[0].strip() == "4":
            active.append((name, index, entry))

    if not active:
        raise ValueError(f"no matching ACTIVE InfiniBand port (device={device}, port={port}); check ibstat")
    if len(active) > 1:
        ports = ", ".join(f"{name}:{i}" for name, i, _ in active)
        raise ValueError(f"multiple ACTIVE InfiniBand ports ({ports}); select --device and --ib-port")
    name, index, entry = active[0]
    rate_text = (entry / "rate").read_text().strip()
    rate = re.match(r"^(\d+(?:\.\d+)?)\s+Gb/sec\b", rate_text)
    if not rate or float(rate[1]) <= 0:
        raise ValueError(f"unknown negotiated rate for {name}:{index}: {rate_text}")
    node = int((ib / name / "device/numa_node").read_text())
    if node < 0:
        nodes = list(Path("/sys/devices/system/node").glob("node[0-9]*"))
        if len(nodes) != 1:
            raise ValueError(f"kernel reports no NIC NUMA node for {name} on this host")
        node = int(nodes[0].name[4:])
    return name, index, node, float(rate[1])


def choose_cpus(numa_node, allow_cross_numa=False, app_count=24, background_count=10):
    """Reserve configurable application and contiguous background CPU pools."""
    if app_count < 1 or background_count < 1:
        raise ValueError("application and background core counts must be positive")
    required_cores = app_count + background_count
    cpus = Path("/sys/devices/system/cpu")
    local, other = [], []
    cores = {}
    for cpu in sorted(os.sched_getaffinity(0)):
        path = cpus / f"cpu{cpu}"
        # All SMT siblings share the same kernel-provided list.
        cores[cpu] = (path / "topology/thread_siblings_list").read_text().strip()
        (local if (path / f"node{numa_node}").exists() else other).append(cpu)

    local_set = set(local)
    local_cores = len({cores[c] for c in local})
    if local_cores < required_cores and not allow_cross_numa:
        raise ValueError(f"NIC NUMA node {numa_node} has {local_cores} allowed physical cores; "
                         f"{required_cores} needed. Use --allow-cross-numa to place remaining application workers on other nodes.")
    app_candidates = local + (other if allow_cross_numa else [])
    # Prefer the lowest local cores for application work, then the lowest
    # eligible background interval. Core IDs can be noncontiguous in a cpuset.
    best = None
    for start in local:
        bg = list(range(start, start + background_count))
        if not local_set.issuperset(bg):
            continue
        used = {cores[c] for c in bg}
        if len(used) != len(bg):
            continue
        app = []
        for cpu in app_candidates:
            if cores[cpu] not in used:
                used.add(cores[cpu])
                app.append(cpu)
                if len(app) == app_count:
                    break
        if len(app) == app_count:
            rank = (sum(c not in local_set for c in app), app, start)
            if best is None or rank < best[0]:
                best = (rank, app, bg)
    if best is None:
        raise ValueError(f"need {app_count} distinct application cores and {background_count} consecutive "
                         "NIC-local CPU IDs on separate physical cores; check lscpu -e and the current CPU affinity")
    return best[1], best[2]
