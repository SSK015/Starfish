"""Extract phase-aligned RDMA byte counts from the native bandwidth workload."""

import math
import re


def one(text, pattern):
    matches = re.findall(pattern, text, flags=re.MULTILINE)
    if len(matches) != 1:
        raise ValueError(f"expected one log record matching {pattern!r}, found {len(matches)}")
    return matches[0]


def fields(text, label):
    line = one(text, "^" + label + r" ([^\n]+)$")
    return dict(token.split("=", 1) for token in line.split() if "=" in token)


def parse_stage(text, fibres):
    result = fields(text, "object_size_result")
    diag = fields(text, "rdma_read_batch_diag")
    rng = fields(text, "rng_seed_result")
    ops = int(result["ops"])
    ops_s = float(result["ops_s"])
    if ops <= 0 or not math.isfinite(ops_s) or ops_s <= 0:
        raise ValueError(f"invalid operation count/rate at {fibres} fibres")
    # elapsed_s is rounded to two decimals after the first stage. ops_s
    # preserves more precision from the same wall-clock interval.
    seconds = ops / ops_s
    printed = float(result["elapsed_s"])
    if not (seconds >= 9.9 and math.isclose(printed, seconds, abs_tol=0.011)):
        raise ValueError(f"inconsistent or short elapsed time at {fibres} fibres")
    reads = int(result["rdma_read_count"])
    read_bytes = int(result["rdma_read_bytes"])
    writes = int(one(text, r"^\s*rdma write posts\s+([\d,]+)\s+\(").replace(",", ""))
    write_bytes = int(one(text, r"^\s*rdma write bytes\s+([\d,]+)\s+\(").replace(",", ""))
    if (int(result["thread_cnt"]) != fibres or int(result["object_size"]) != 4000
            or rng["access_distribution"] != "uniform_id_v1" or int(rng["fibre_count"]) != fibres):
        raise ValueError(f"unexpected workload at {fibres} fibres")
    if (int(diag["configured_batch_size"]) != 1 or int(diag["pending_reads"]) != 0
            or not 0 < reads <= ops or reads != int(diag["accepted_reads"])
            or read_bytes != 4000 * reads or read_bytes != int(diag["accepted_bytes"])
            or writes <= 0 or write_bytes <= 0):
        raise ValueError(f"inconsistent RDMA counters at {fibres} fibres")
    return dict(fibres=fibres, elapsed_s=seconds, ops=ops, ops_s=ops_s,
                read_count=reads, read_bytes=read_bytes, write_posts=writes, write_bytes=write_bytes,
                read_gbps=read_bytes * 8e-9 / seconds, write_gbps=write_bytes * 8e-9 / seconds)


def summarize(log_path, link_gbps):
    text = log_path.read_text()
    parts = text.split("\nstart benchmarking...\n")
    if len(parts) != 4:
        raise ValueError(f"expected three benchmark stages in {log_path}")
    for record in ("server_count: 1", "ft_method: none", "behavior_group: 0 effective_ec=0",
                   "runtime.exclusive_owned_batch=1 mark_workers=6 evict_workers=4",
                   "hot_vec_size: 4294967 cold_vec_size: 0 request_object_num: 4294967 total_object_num: 4294967",
                   "warm up done"):
        one(parts[0], "^" + re.escape(record) + "$")
    one(parts[-1], r"^exact used bytes: 0$")
    if not math.isfinite(link_gbps) or link_gbps <= 0:
        raise ValueError("invalid NIC link rate")
    try:
        stages = [parse_stage(block, fibres) for block, fibres in zip(parts[1:], (24, 48, 72))]
    except KeyError as err:
        raise ValueError(f"missing native benchmark counter {err} in {log_path}") from err
    primary = stages[1]
    peak = max(primary["read_gbps"], primary["write_gbps"])
    return dict(mode="nonft_exclusive", payload_bytes=4000, working_set_bytes=2**34,
                link_gbps=link_gbps, stages=stages, primary=primary, peak_direction_gbps=peak,
                link_utilization_fraction=peak / link_gbps)
