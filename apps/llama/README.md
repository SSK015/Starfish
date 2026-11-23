# Llama 2 chat with far memory

Run Llama 2 7B Chat on a compute node with a 6 GiB Starfish cache. A second host
provides the backing memory over InfiniBand. Replies stream into the terminal,
and the model keeps the conversation across turns.
[Watch a 19-second terminal recording](../../assets/llama-chat-demo.mp4).

## Start a conversation

Use two Linux x86-64 machines on the same InfiniBand fabric. Install the
[build prerequisites](../../README.md#quick-start) and `numactl` on both.
The launcher builds the required target on first use.

| Host | Requirements for the 7B demo |
| --- | --- |
| Compute | 3,072 free 2 MiB HugePages on the NIC's NUMA node, permission to lock 6 GiB, and RAM for inference state and file-backed model pages. The client uses 24 application cores and six separate background cores. |
| Memory | At least 40 GiB of lockable RAM on the NIC's NUMA node, plus headroom for the operating system. |

Follow the [deployment guide](../../docs/DEPLOYMENT.md#memory-permissions-and-nic-selection)
for HugePage access. On **both** hosts, set `MEMORY_IP` to the memory host's
IPv4 address reachable from the compute host. Run these commands from the
Starfish repository root.

**Memory host** — leave the service running during the conversation.

```bash
python3 apps/llama/demo.py server "$MEMORY_IP"
```

**Compute host** — set `MODEL` and `TOKENIZER` to your two local files as
described below. The memory host needs neither file.

```bash
python3 apps/llama/demo.py client "$MEMORY_IP" --model "$MODEL" --tokenizer "$TOKENIZER"
```

On a dual-socket compute host with fewer than 30 physical cores on the NIC's
NUMA node, add `--allow-cross-numa`. The six background cores and memory
allocation stay on the NIC's node. If a host has several active InfiniBand
ports, select one with `--device` and `--ib-port` on that host.

At `User:`, ask a question and wait for the reply. In the recording, the first
prompt names a submarine, and the follow-up asks for a message from its first
dive without repeating the name. Press Ctrl-D or type `<end>` at the next
prompt to finish. Stop the memory service with Ctrl-C.

## Prepare the model on the compute host

Use the original [Meta Llama 2 7B Chat checkpoint](https://huggingface.co/meta-llama/Llama-2-7b-chat)
and its matching tokenizer. Starfish reads the legacy version-0 FP32 format
from [llama2.c](https://github.com/karpathy/llama2.c). If you already
have the two converted files, set `MODEL` and `TOKENIZER` to their full paths.

For conversion, set `CHECKPOINT_DIR` to the directory containing
`params.json` and the original weight shard, and `TOKENIZER_MODEL` to the
full path of the accompanying `tokenizer.model`. From an environment with
the [exporter's dependencies](https://github.com/karpathy/llama2.c/blob/master/requirements.txt):

```bash
git clone https://github.com/karpathy/llama2.c.git
cd llama2.c
export MODEL="$PWD/llama2-7b-chat-v0.bin"
python3 export.py "$MODEL" --meta-llama "$CHECKPOINT_DIR" --version 0 --dtype fp32
python3 tokenizer.py --tokenizer-model "$TOKENIZER_MODEL"
export TOKENIZER="${TOKENIZER_MODEL%.model}.bin"
```

The converted checkpoint occupies about 25.1 GiB of disk space. Return to the
Starfish repository root before starting the client.

## Verify far-memory access

After the chat exits, the client prints `profile rdma read bytes` and
`profile rdma write bytes` for the chat phase, excluding model initialization.
The client prints the path to `runtime.log` at startup. This file records
placement diagnostics while the terminal displays the conversation.

This configuration uses one memory server, an exclusive cache with a 4.8 GiB
resident budget, and no fault tolerance or backup. Greedy decoding uses a
shared limit of 512 positions for prompts and replies together.
