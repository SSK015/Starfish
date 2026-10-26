# Hardware and deployment

## Minimum setup

Running Starfish requires **at least two physical Linux x86-64 machines connected
by InfiniBand**:

- One compute machine runs the application and Starfish runtime.
- At least one separate memory machine runs the memory-server processes.

Both sides need active RDMA-capable NICs, matching drivers/userspace verbs support,
and a working InfiniBand fabric or direct connection. The configured memory-server
TCP addresses and ports must also be reachable for connection setup; that control
connection can use a separate management network. A build machine does not need
an active InfiniBand link just to compile the code.

The AE used 100 Gb/s InfiniBand; this is a reference configuration, not a minimum
link-speed requirement. Required CPU count and RAM depend on the workload, local
cache, worker counts, and remote capacity. There is no single RAM minimum that
fits all bundled applications.

## One memory machine, multiple processes

The minimum two-machine setup can run several memory-server processes on its one
memory machine. Give each process its own listening port and server configuration.
The client treats each unique address/port pair as a separate logical endpoint.

For example, these are three logical memory servers on one physical host:

| Endpoint index | Physical host | TCP address | TCP port |
| --- | --- | --- | --- |
| 0 | memory-host-A | 192.0.2.10 | 1893 |
| 1 | memory-host-A | 192.0.2.10 | 1894 |
| 2 | memory-host-A | 192.0.2.10 | 1895 |

The addresses above are documentation examples; replace them with reachable
addresses in your deployment. For a multi-endpoint client configuration,
`server_count` is the number of processes/endpoints, not the number of physical
hosts. Set `server_addrs` and `server_ports` to comma-separated lists of that
length, in matching order. Do not repeat an identical address/port pair.

Start each memory-server process with a **single-endpoint configuration**:

```bash
# Run on the memory machine; each file specifies its own server_addr/server_port.
./build/server /path/to/server-0.config
# In another session:
./build/server /path/to/server-1.config
```

The executable accepts one configuration-file argument; there is no positional
port argument. Use a different config file for each process. This also avoids
ambiguity in the optional `SERVER_PORT` environment override, which matches only
the port when given a multi-endpoint config.

## Multiple physical memory machines

Memory endpoints may instead be spread across several physical machines, or use
multiple processes on each machine. Add each process's address and port to the
same ordered client lists. The compute machine connects to every listed endpoint;
there is no requirement that they share an IP address.

**Co-located processes are not independent physical failure domains.** They are
useful for basic deployment and process-failure checks, but a failed physical
memory machine loses all processes on it together. Testing physical-machine
fault tolerance requires placing the protected replicas/erasure-code fragments
across independent physical memory machines, with enough endpoints and placement
for the selected protection scheme. The two-machine minimum alone does not
demonstrate tolerance of the only memory machine failing.

Build on each host, or share binaries only after checking CPU instruction-set,
runtime-library, and dependency compatibility. A new CPU or software environment
may require rebuilding locally.

## Memory, permissions, and NIC selection

- Reserve enough **2 MiB HugePages on the compute machine** for the runtime's
  registered client buffers. Also leave RAM for the local cache, application
  inputs, and the OS.
- `server_buffer_size` is the allocation **per memory-server process**, aligned
  by the runtime to 512 KiB. With a uniform configuration, remote capacity is
  `server_buffer_size * server_count`. Add together all process allocations
  on each physical memory host and leave headroom for the OS and RDMA resources.
- The memory server allocates ordinary aligned memory and registers it for RDMA;
  it does not require the compute-side HugePages allocation for that buffer.
  Ensure the process has sufficient locked-memory permission for registration.
- Set `ib_device_name` and `ib_port` for each host's active InfiniBand interface.
  These select the RDMA device/port, not the TCP address/port used for bootstrap.
- For performance work, record CPU/NUMA placement, NIC locality, worker counts,
  cache size, process placement, and other traffic on the fabric. Logical endpoint
  count alone does not describe the physical deployment.

## Full paper experiments

This maintained repository contains the runtime and applications, not the
paper-level orchestration or input datasets. Use the preserved
[AE installation guide](https://github.com/ICTPLSys/Starfish/blob/main/artifact-evaluation/docs/INSTALL.md)
and [multi-server guide](https://github.com/ICTPLSys/Starfish/blob/main/artifact-evaluation/docs/MULTI-SERVER.md)
for site configuration, inputs, and the full experiment workflow. Check paths
when adapting those instructions to this standalone repository.

The build script only configures and compiles. A successful build does not
establish RDMA connectivity, workload correctness, or reproduction of paper results.
