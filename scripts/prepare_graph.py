#!/usr/bin/env python3
"""Split a text or gzip edge list into a directory accepted by BFS and NQ."""

import argparse
from collections import defaultdict
import ctypes
import gzip
import os
from pathlib import Path
import tempfile
import zlib


def vertex_id(token):
    digits = token.lstrip(b"0") or b"0"
    if (not token.isdigit() or len(digits) > 20 or
            (len(digits) == 20 and digits > b"18446744073709551615")):
        raise ValueError("expected an unsigned 64-bit decimal vertex ID")
    return digits


def edges(path):
    opener = gzip.open if path.suffix.lower() == ".gz" else open
    with opener(path, "rb") as stream:
        for number, line in enumerate(stream, 1):
            fields = line.partition(b"#")[0].split()
            if not fields:
                continue
            try:
                if len(fields) != 2:
                    raise ValueError("expected two vertex IDs")
                yield vertex_id(fields[0]) + b" " + vertex_id(fields[1]) + b"\n"
            except ValueError as error:
                raise ValueError(f"{path}:{number}: {error}") from error


def flush(directory, buffers):
    for index, data in buffers.items():
        with (directory / f"edges.{index}").open("ab") as out:
            out.write(data)
    buffers.clear()


def publish(directory, destination):
    # A concurrent invocation must never replace an existing directory, even
    # an empty one. Windows rename already has these semantics.
    if os.name == "nt":
        directory.rename(destination)
        return
    libc = ctypes.CDLL(None, use_errno=True)
    if not hasattr(libc, "renameat2"):
        raise OSError("atomic publication requires Linux renameat2 or Windows")
    at_fdcwd, rename_noreplace = -100, 1
    if libc.renameat2(at_fdcwd, os.fsencode(directory), at_fdcwd,
                      os.fsencode(destination), rename_noreplace) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), destination)


def prepare(source, destination, shards):
    if os.path.lexists(destination):
        raise FileExistsError(f"destination already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".prepare-graph-", dir=destination.parent) as tmp:
        directory = Path(tmp) / "dataset"
        directory.mkdir()
        # Buffer by shard for sequential appends without holding a file
        # descriptor per shard or opening a file for every edge.
        buffers = defaultdict(bytearray)
        count = buffered = 0
        for row in edges(source):
            buffers[count % shards].extend(row)
            count += 1
            buffered += len(row)
            if buffered >= 4 * 1024 * 1024:
                flush(directory, buffers)
                buffered = 0
        if not count:
            raise ValueError(f"{source}: empty edge list")
        flush(directory, buffers)
        for index in range(count, shards):
            (directory / f"edges.{index}").touch()
        (directory / "shard-count").write_bytes(f"{shards}\n".encode("ascii"))
        publish(directory, destination)
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="unweighted edge list (.txt or .gz)")
    parser.add_argument("output_dir", type=Path, help="new dataset directory")
    parser.add_argument("--shards", type=int, default=1, help="number of shards (default: 1)")
    args = parser.parse_args()
    if args.shards < 1:
        parser.error("--shards must be positive")
    try:
        count = prepare(args.input, args.output_dir, args.shards)
    except (gzip.BadGzipFile, EOFError, zlib.error) as error:
        parser.exit(1, f"{parser.prog}: {args.input}: {error}\n")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    print(f"{args.output_dir}: {count} edges, {args.shards} shards")


if __name__ == "__main__":
    main()
