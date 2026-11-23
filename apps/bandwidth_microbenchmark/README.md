# NIC bandwidth demo

How much of an InfiniBand link can Starfish use for random object accesses?
This demo runs the standalone `bandwidth_microbenchmark` against one memory
node with fault tolerance off. A reference run on two 100 Gbit/s hosts reached
**94.32 Gbit/s of RDMA READ payload** at 48 application fibres (two rounds).
The client reports the result for your hardware.

[Watch a 19-second client terminal recording](../../assets/nic-bandwidth-demo.mp4)
from a separate run on the same two hosts. The recording abbreviates the
run directory to `build/bandwidth-client/`. Each invocation prints its own
directory for the logs and `summary.json`.

## Run on two hosts

Use two Linux x86-64 hosts on the same InfiniBand fabric. On **both** hosts:

```bash
git clone https://github.com/SSK015/Starfish.git
cd Starfish
```

Install the [build prerequisites](../../README.md#quick-start), Python 3.8 or later, and `numactl`.
The launcher builds the required target on first use with `scripts/build.sh`.
Building on each host keeps the executables compatible with its system libraries.

| Host | Resources for this demo |
| --- | --- |
| Memory | One active InfiniBand port and at least 30,475,288,576 bytes of lockable RAM on its NUMA node, plus OS headroom. |
| Compute | One active InfiniBand port, 24 application cores and 10 separate background cores. Reserve 2,626 **2 MiB HugePages** on the NIC's NUMA node and allow at least 5,507,121,152 bytes of locked memory. Leave RAM for metadata and the OS. |

Check `ulimit -l` on both hosts (reported in KiB). On the compute host, the
runner also needs HugePage access through the system's `hugetlb_shm_group` or
`CAP_IPC_LOCK`. If pages are missing, the launcher prints the NUMA node and
number of additional pages needed. See the [deployment guide](../../docs/DEPLOYMENT.md#memory-permissions-and-nic-selection).

**1. On the memory host**, start the service and leave it running. Replace
`192.0.2.10` with this host's IPv4 address reachable from the compute host.
The address is used for TCP setup. RDMA uses the chosen InfiniBand port.

```bash
python3 apps/bandwidth_microbenchmark/demo.py server 192.0.2.10
```

**2. On the compute host**, use the address printed by the server:

```bash
python3 apps/bandwidth_microbenchmark/demo.py client 192.0.2.10
```

The client first looks for 34 distinct physical cores on the NIC's NUMA node.
On a dual-socket host with fewer local cores, pass `--allow-cross-numa` to
permit application workers on the other socket. The ten background cores and
the client buffer stay on the NIC's node. This was the reference layout.
Inspect the printed CPU lists before comparing machines.

On hosts with multiple active InfiniBand ports, select the local port with
`--device mlx5_1 --ib-port 1` (use the appropriate device name on each host).
Use `--port PORT` on both sides to change TCP port 1400.
Stop the memory service with **Ctrl-C** when the client finishes.

## Reading the result

Each client process initializes a 16 GiB working set of 4,000-byte objects.
It then issues uniform random accesses over the full population with no pacing.
Exclusive caching uses a 5.13 GiB local buffer and a 28.38 GiB remote buffer.
Two fresh client processes each run 24, 48 and 72 application fibres for about
10 seconds per stage. The 48-fibre stage is the designated comparison point.

For each stage the client divides the posted RDMA READ and WRITE payload bytes
by the measured wall time, separately. It reports the larger direction in
decimal Gbit/s. Multiple rounds are combined by pooling bytes and elapsed
seconds. **% of link** divides that rate by the compute port's negotiated
one-direction rate from sysfs. Protocol headers are excluded from payload.
The percentage describes this workload on the selected machines.

Example terminal output from the 100 Gbit/s reference run:

```text
 Fibres   Direction   RDMA payload   % of link    M accesses/s
     24        READ    80.47 Gbit/s     80.5%         3.352
     48        READ    94.32 Gbit/s     94.3%         4.122
     72       WRITE    94.13 Gbit/s     94.1%         4.110
```

The client prints a new run directory containing the generated site config,
the raw log and latency histogram for each round, and `summary.json`.
Use `--output-dir DIR` to choose its parent or `--build-dir DIR` to select a
nondefault Starfish build. Successful rounds check stage completeness, read
accounting and remote allocator cleanup before contributing to the summary.

## Other target

The build also supports a 512-byte object variant:

```bash
bash scripts/build.sh --targets bandwidth_microbenchmark_512
```

The historical `object_size` and `object_size_512` target and executable aliases
remain available. The packaged NIC demo uses the 4,000-byte target.
