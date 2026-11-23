#!/usr/bin/env python3
"""Run the two-host Starfish NIC bandwidth demo."""

import argparse
import errno
import ipaddress
import json
import os
from pathlib import Path
import resource
import shutil
import socket
import subprocess
import sys
import tempfile

from demo_results import summarize
from demo_topology import choose_cpus, get_port

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
MIB = 1024 ** 2


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    roles = parser.add_subparsers(dest="role", required=True)
    for role in ("server", "client"):
        sub = roles.add_parser(role)
        sub.add_argument("memory_host", help="reachable IPv4 address or hostname of the memory node")
        sub.add_argument("--port", type=int, default=1400, help="TCP bootstrap port (default: 1400)")
        sub.add_argument("--device", help="local InfiniBand device (auto-detected by default)")
        sub.add_argument("--ib-port", type=int, help="local InfiniBand port number")
        sub.add_argument("--build-dir", type=Path, default=ROOT / "build", help="Starfish build directory")
        sub.add_argument("--output-dir", type=Path, help="parent of the new run directory (default: build directory)")
        if role == "client":
            sub.add_argument("--allow-cross-numa", action="store_true",
                             help="allow application workers on another NUMA node when needed")
            sub.add_argument("--rounds", type=int, default=2, help="number of benchmark processes (default: 2)")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    if args.role == "client" and args.rounds < 1:
        parser.error("--rounds must be positive")
    return args


def memory_address(args):
    addr = ipaddress.IPv4Address(socket.gethostbyname(args.memory_host))
    if addr.is_loopback or addr.is_multicast or addr.is_unspecified or int(addr) == 2**32 - 1:
        raise ValueError("use the memory node's reachable unicast IPv4 address")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        if args.role == "server":
            # A TCP connect probe would consume the server's client handshake.
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind((str(addr), args.port))
        else:
            try:
                probe.bind((str(addr), 0))
            except OSError as error:
                if error.errno != errno.EADDRNOTAVAIL:
                    raise
            else:
                raise ValueError("run the client on a separate compute host")
    return str(addr)


def check_memory(role, node, config):
    client = role == "client"
    alignment = 2 * MIB if client else MIB // 2
    size = int(config["client_buffer_size" if client else "server_buffer_size"])
    units = (size + alignment - 1) // alignment
    status = Path("/proc/self/status").read_text().splitlines()
    caps = next(line.split()[1] for line in status if line.startswith("CapEff:"))
    ipc_lock = bool(int(caps, 16) & (1 << 14))
    soft, hard = resource.getrlimit(resource.RLIMIT_MEMLOCK)
    if not ipc_lock and soft != resource.RLIM_INFINITY and soft < units * alignment:
        if hard != resource.RLIM_INFINITY and hard < units * alignment:
            raise ValueError(f"{role} requires a locked-memory limit of at least {units * alignment} bytes")
        resource.setrlimit(resource.RLIMIT_MEMLOCK, (hard, hard))
    if client:
        pages = Path(f"/sys/devices/system/node/node{node}/hugepages/hugepages-2048kB")
        free = int((pages / "free_hugepages").read_text())
        if free < units:
            raise ValueError(f"NUMA node {node} has {free} free 2 MiB HugePages; {units} required. "
                             f"Ask your administrator to reserve {units - free} additional pages at {pages}.")
        gid = int(Path("/proc/sys/vm/hugetlb_shm_group").read_text())
        if not ipc_lock and gid not in [os.getegid()] + os.getgroups():
            raise ValueError(f"client needs HugePage access via hugetlb_shm_group (GID {gid}) or CAP_IPC_LOCK")


def build_target(build, target):
    exe = build / ("server" if target == "server" else "benchmark/microbenchmarks/" + target)
    if not exe.is_file():
        subprocess.run(["bash", str(ROOT / "scripts/build.sh"), "--build-dir", str(build),
                        "--targets", target], check=True)
    if not os.access(exe, os.X_OK):
        raise ValueError(f"no executable at {exe}")
    return exe


def client_settings(app, bg, out):
    # Keep the reference workload and cache policy independent of the login shell.
    return {
        "FibreWorkerCount": str(len(app)),
        "FibreCpuSet": ",".join(map(str, app)),
        "FARLIB_SEPARATE_BACKGROUND_CLUSTER": "1",
        "FARLIB_BACKGROUND_CPU_BASE": str(bg[0]),
        "FARLIB_PIN_RDMA_THREAD": "0",
        "FARLIB_OPT_LEGACY_EXCLUSIVE_PIPELINE": "1",
        "FARLIB_EXCLUSIVE_OWNED_BATCH": "1",
        "FARLIB_ASYNC_FULL_RETURNS": "1",
        "FARLIB_LEGACY_SCAN_CURSORS": "1",
        "FARLIB_REMOTE_USAGE_SHARDS": "1",
        "FARLIB_FIBRE_HEAP": "1",
        "FARLIB_PLANNER_BUDGET_EARLY_EXIT": "1",
        "FARLIB_ALLOC_SCOPE_CHECKPOINT": "1",
        "FARLIB_RDMA_READ_BATCH": "1",
        "FARLIB_RDMA_READ_BATCH_ATTRIBUTION": "none",
        "FARLIB_PROFILE_CQ_TIMING": "1",
        "FARLIB_ALLOC_WAIT_DIAG": "1",
        "FARLIB_ALLOC_RECLAIM_RATE_DIAG": "1",
        "FARLIB_MARK_WORK_TRACE": "1",
        "FARLIB_INCLUSIVE_RECLAIM_DIAG": "1",  # Separate trace file per stage.
        "FARLIB_ROUND_GAP_TRACE": "1",
        "OBJECT_WAIT_TRACE": str(out / "round-gap.tsv"),
        "OBJECT_SIZE_FORMAL_FIBRES": "48",
    }


def report(results, rate, device, port, run):
    stages = []
    print("\n Fibres   Direction   RDMA payload   % of link    M accesses/s")
    for index, fibres in enumerate((24, 48, 72)):
        samples = [result["stages"][index] for result in results]
        seconds = sum(s["elapsed_s"] for s in samples)
        read = 8e-9 * sum(s["read_bytes"] for s in samples) / seconds
        write = 8e-9 * sum(s["write_bytes"] for s in samples) / seconds
        direction = "READ" if read >= write else "WRITE"
        payload = max(read, write)
        percent = 100 * payload / rate
        accesses = sum(s["ops"] for s in samples) / seconds
        stages.append(dict(fibres=fibres, direction=direction, read_gbps=read,
                           write_gbps=write, payload_gbps=payload,
                           percent_of_link=percent, accesses_per_second=accesses))
        print(f"{fibres:7}   {direction:>9}   {payload:6.2f} Gbit/s   {percent:6.1f}%    {accesses/1e6:10.3f}")
    primary = stages[1]
    print(f"\n48 fibres: {primary['payload_gbps']:.2f} Gbit/s {primary['direction']} payload"
          f" / {rate:g} Gbit/s link = {primary['percent_of_link']:.1f}%")
    summary = dict(device=device, ib_port=port, link_gbps=rate, rounds=len(results),
                   primary_fibres=48, stages=stages)
    (run / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"Results: {run / 'summary.json'}")


def main():
    args = arguments()
    if not shutil.which("numactl"):
        raise ValueError("install numactl on this host")
    address = memory_address(args)
    device, port, node, rate = get_port(args.device, args.ib_port)
    base_config = (HERE / "demo.config").read_text()
    config = dict(line.split() for line in base_config.splitlines() if line and not line.startswith("#"))
    check_memory(args.role, node, config)
    if args.role == "client":
        app, bg = choose_cpus(node, args.allow_cross_numa)
    target = "server" if args.role == "server" else "bandwidth_microbenchmark"
    build = args.build_dir.resolve()
    binary = build_target(build, target)
    parent = (args.output_dir or build).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="bandwidth-" + args.role + "-", dir=parent))
    site = run / "site.config"
    site.write_text(base_config + f"server_addr {address}\nserver_port {args.port}\n"
                    f"ib_device_name {device}\nib_port {port}\n")
    print(f"{args.role}: {device} port {port}, {rate:g} Gbit/s, NUMA node {node}. Files: {run}", flush=True)
    env = {
        k: v for k, v in os.environ.items()
        if not k.startswith(("FARLIB_", "OBJECT_SIZE_", "OBJECT_WAIT_", "Fibre"))
        and k not in ("SERVER_PORT", "SCOPE_DIAG")
    }
    prefix = ["numactl", f"--membind={node}"]
    if args.role == "server":
        print(f"On the compute host: python3 apps/bandwidth_microbenchmark/demo.py client {address} "
              f"--port {args.port}\nStop the memory service with Ctrl-C after the client finishes.", flush=True)
        cmd = prefix + [f"--cpunodebind={node}", str(binary), str(site)]
        os.execvpe(cmd[0], cmd, env)

    prefix += ["--physcpubind=" + ",".join(map(str, app + bg))]
    print(f"Application CPUs: {app}\nBackground CPUs: {bg}", flush=True)
    results = []
    for idx in range(1, args.rounds + 1):
        out = run / f"round-{idx}"
        out.mkdir()
        cmd = prefix + [str(binary), str(site), "1", str(out / "latency.hdr"), "0", "0"]
        settings = client_settings(app, bg, out)
        (out / "invocation.json").write_text(json.dumps({"argv": cmd, "settings": settings}, indent=2) + "\n")
        log_path = out / "client.log"
        print(f"Round {idx}/{args.rounds}: warm-up and 3 x 10 s. Log: {log_path}", flush=True)
        with log_path.open("w") as log:
            subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, cwd=out,
                           env={**env, **settings}, timeout=600, check=True)
        result = summarize(log_path, rate)
        results.append(result)
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        primary = result["primary"]
        direction = "READ" if primary["read_gbps"] >= primary["write_gbps"] else "WRITE"
        print(f"  48 fibres: {result['peak_direction_gbps']:.2f} Gbit/s {direction} payload", flush=True)
    report(results, rate, device, port, run)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as err:
        sys.exit(f"Bandwidth demo: {err}")
