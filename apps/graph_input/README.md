# Graph input for BFS and NQ

From the repository root, build the two applications with the usual wrapper:

```sh
bash scripts/build.sh --targets gapbs_bfs_chunked,nhop_graph
```

Set `CONFIG` to the Starfish client configuration for your running memory
server. To read an uncompressed edge list, pass its filename directly:

```sh
build/benchmark/microbenchmarks/gapbs_bfs_chunked "$CONFIG" graph.txt
build/benchmark/microbenchmarks/nhop_graph "$CONFIG" graph.txt
```

Each data line contains two whitespace-separated, unsigned 64-bit decimal
vertex IDs. Blank lines and `#` comments are accepted. For example:

```text
# source destination
10 42
42 91
91 10
```

The loader builds an undirected adjacency list, retaining repeated edges and
both adjacency entries of a self-loop. Vertex IDs may have gaps. The programs
use dense vertex numbers in ascending order of input ID, so the three IDs
above become 0, 1, and 2. Vertices are defined by their presence in an edge.

## Prepare a sharded dataset

Use [prepare_graph.py](../../scripts/prepare_graph.py) with either a plain
edge list or a gzip-compressed one. Choose a new output directory:

```sh
python3 scripts/prepare_graph.py graph.txt.gz graph-data --shards 8
build/benchmark/microbenchmarks/gapbs_bfs_chunked "$CONFIG" graph-data
build/benchmark/microbenchmarks/nhop_graph "$CONFIG" graph-data
```

The script streams the edges and writes `shard-count` plus `edges.0` through
`edges.7` in this example. Pass the directory as the graph argument. Both
programs check it against the recorded shard count, including the last shard.
A dataset must contain at least one edge.

An existing regular file is read directly, even if it has numbered siblings.
Existing 32-part datasets can still be opened by passing the common filename
prefix of `<prefix>.0` through `<prefix>.31`. NQ's optional latency benchmark
retains its full-Friendster workload requirements.
