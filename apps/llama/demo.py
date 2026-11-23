#!/usr/bin/env python3
"""Launch the two-host Starfish LLaMA chat demo."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BANDWIDTH_TOOLS = ROOT / "apps" / "bandwidth_microbenchmark"
sys.path.insert(0, str(BANDWIDTH_TOOLS))
from demo import check_memory, memory_address  # noqa: E402
from demo_topology import choose_cpus, get_port  # noqa: E402

CHAT_STEPS = 512
SYSTEM_PROMPT = "Answer clearly and briefly in a few sentences."


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    roles = parser.add_subparsers(dest="role", required=True)
    for role in ("server", "client"):
        sub = roles.add_parser(role)
        sub.add_argument(
            "memory_host",
            metavar="memory-address",
            help="reachable IPv4 address or hostname of the memory node",
        )
        sub.add_argument("--port", type=int, default=1400, help="TCP bootstrap port (default: 1400)")
        sub.add_argument("--device", help="local InfiniBand device (auto-detected by default)")
        sub.add_argument("--ib-port", type=int, help="local InfiniBand port number")
        sub.add_argument(
            "--build-dir",
            type=Path,
            default=ROOT / "build",
            help="Starfish build directory (default: repository build directory)",
        )
        if role == "client":
            sub.add_argument("--model", type=Path, required=True, help="legacy FP32 LLaMA .bin checkpoint")
            sub.add_argument("--tokenizer", type=Path, required=True, help="matching tokenizer .bin file")
            sub.add_argument(
                "--allow-cross-numa",
                action="store_true",
                help="allow application workers on another NUMA node when needed",
            )
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    if args.ib_port is not None and args.ib_port < 1:
        parser.error("--ib-port must be positive")
    return args


def read_demo_config():
    base = (HERE / "demo.config").read_text(encoding="utf-8")
    values = {}
    for raw in base.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, value = line.split(None, 1)
        values[key] = value
    return base, values


def require_file(path, option):
    resolved = path.expanduser().resolve(strict=True)
    if not resolved.is_file():
        raise ValueError(f"{option} is not a file: {resolved}")
    return resolved


def build_target(build, target):
    executable = build / ("server" if target == "server" else "benchmark/llama/run_chat_far")
    if not executable.is_file():
        subprocess.run(
            [
                "bash",
                str(ROOT / "scripts" / "build.sh"),
                "--build-dir",
                str(build),
                "--targets",
                target,
            ],
            cwd=ROOT,
            check=True,
        )
    if not os.access(executable, os.X_OK):
        raise ValueError(f"no executable at {executable}")
    return executable


def clean_environment():
    return {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("FARLIB_", "Fibre"))
        and key not in ("SERVER_PORT", "SCOPE_DIAG")
    }


def client_settings(app_cpus, background_cpus):
    # Keep application workers off the separate, contiguous Mark/Evict pool.
    return {
        "FibreWorkerCount": str(len(app_cpus)),
        "FibreCpuSet": ",".join(map(str, app_cpus)),
        "FARLIB_SEPARATE_BACKGROUND_CLUSTER": "1",
        "FARLIB_BACKGROUND_CPU_BASE": str(background_cpus[0]),
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
        "FARLIB_REGION_LIST_YIELD_LOCK": "1",
        "FARLIB_RESIDENT_PROFILE_REQUIRE_WORK_PHASE": "1",
        "FARLIB_THREAD_HEAP_FULL_RETURN_BATCH": "1",
        "FARLIB_ALLOC_WAIT_DIAG": "1",
    }


def main():
    args = arguments()
    if not shutil.which("numactl"):
        raise ValueError("install numactl on this host")
    model = tokenizer = None
    if args.role == "client":
        model = require_file(args.model, "--model")
        tokenizer = require_file(args.tokenizer, "--tokenizer")

    address = memory_address(args)
    device, ib_port, node, rate = get_port(args.device, args.ib_port)
    base_config, config = read_demo_config()
    check_memory(args.role, node, config)
    if args.role == "client":
        app_cpus, background_cpus = choose_cpus(
            node, args.allow_cross_numa, app_count=24, background_count=6
        )

    build = args.build_dir.expanduser().resolve()
    build.mkdir(parents=True, exist_ok=True)
    target = "server" if args.role == "server" else "run_chat_far"
    binary = build_target(build, target)
    run = Path(tempfile.mkdtemp(prefix=f"llama-{args.role}-", dir=build))
    site = run / "site.config"
    site.write_text(
        base_config.rstrip()
        + f"\nserver_addr {address}\nserver_port {args.port}\n"
        + f"ib_device_name {device}\nib_port {ib_port}\n",
        encoding="utf-8",
    )
    print(
        f"{args.role}: {device} port {ib_port}, {rate:g} Gbit/s, NUMA node {node}. "
        f"Site config: {site}",
        flush=True,
    )
    env = clean_environment()
    prefix = ["numactl", f"--membind={node}"]
    if args.role == "server":
        prefix.append(f"--cpunodebind={node}")
        command = prefix + [str(binary), str(site)]
        print(
            f"On the compute host: python3 apps/llama/demo.py client {address} "
            '--model "$MODEL" --tokenizer "$TOKENIZER" '
            f"--port {args.port}",
            flush=True,
        )
        print("Stop the memory service with Ctrl-C after the chat.", flush=True)
        os.execvpe(command[0], command, env)

    cpus = app_cpus + background_cpus
    prefix.append("--physcpubind=" + ",".join(map(str, cpus)))
    settings = client_settings(app_cpus, background_cpus)
    env.update(settings)
    command = prefix + [
        str(binary),
        str(site),
        str(model),
        "-z",
        str(tokenizer),
        "-m",
        "chat",
        "-y",
        SYSTEM_PROMPT,
        "-t",
        "0",
        "-s",
        "1",
        "-n",
        str(CHAT_STEPS),
    ]
    print(f"Application CPUs: {app_cpus}", flush=True)
    print(f"Background CPUs: {background_cpus} (2 Mark, 4 Evict)", flush=True)
    print("Enter a question at each User: prompt; type <end> or press Ctrl-D to exit.", flush=True)
    # Planner diagnostics can arrive between any two generated tokens.
    diagnostics = run / "runtime.log"
    print(f"Runtime diagnostics: {diagnostics}", flush=True)
    with diagnostics.open("w") as log:
        os.dup2(log.fileno(), sys.stderr.fileno())
    os.execvpe(command[0], command, env)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as err:
        sys.exit(f"LLaMA demo: {err}")
