#!/usr/bin/env python3
"""Focused end-to-end regression coverage for BFS graph input formats."""

import collections
import gzip
import pathlib
import shutil
import subprocess
import sys
import tempfile


EDGES = [(10, 20), (20, 900), (10, 20), (42, 42), (900, 10)]
EDGE_TEXT = "".join(f"{u} {v}\n" for u, v in EDGES)
PASS_MARKER = "GRAPH_INPUT_PASS"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(command, context):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    require(result.returncode == 0,
            f"{context} failed (exit {result.returncode})\n"
            f"command: {command!r}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def run_probe(probe, input_path, context):
    result = run([str(probe), str(input_path)], context)
    require(PASS_MARKER in result.stdout,
            f"{context}: loader probe omitted {PASS_MARKER!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")


def expect_failure(command, context):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    require(result.returncode != 0,
            f"{context} unexpectedly succeeded\nstdout:\n{result.stdout}")
    return result.stdout + result.stderr


def write_edges(path, edges=EDGES):
    path.write_text("".join(f"{u} {v}\n" for u, v in edges), encoding="utf-8")


def check_prepared(preparer, probe, source, output_dir, shards):
    run([sys.executable, str(preparer), str(source), str(output_dir),
         "--shards", str(shards)], f"prepare {source}")
    expected_names = {"shard-count"} | {f"edges.{i}" for i in range(shards)}
    actual_names = {entry.name for entry in output_dir.iterdir()}
    require(actual_names == expected_names,
            f"prepared shard names differ for {output_dir}: expected "
            f"{sorted(expected_names)}, got {sorted(actual_names)}")
    count = (output_dir / "shard-count").read_text(encoding="utf-8").strip()
    require(count == str(shards),
            f"{output_dir}/shard-count: expected {shards}, got {count!r}")

    combined = []
    for index in range(shards):
        shard = output_dir / f"edges.{index}"
        for number, line in enumerate(shard.read_text(encoding="utf-8").splitlines(), 1):
            fields = line.split()
            require(len(fields) == 2 and all(field.isdecimal() for field in fields),
                    f"invalid emitted edge at {shard}:{number}: {line!r}")
            combined.append(tuple(map(int, fields)))
    require(collections.Counter(combined) == collections.Counter(EDGES),
            f"prepared edge multiset differs for {output_dir}: {combined!r}")
    run_probe(probe, output_dir, f"load prepared directory {output_dir}")


def main():
    require(len(sys.argv) == 3,
            "usage: graph_input_regression.py PROBE PREPARE_GRAPH_PY")
    probe = pathlib.Path(sys.argv[1]).resolve()
    preparer = pathlib.Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="bfs-graph-input-") as temp:
        root = pathlib.Path(temp)

        # 1. A regular file always dispatches as a single edge list.
        plain_file = root / "plain.txt"
        plain_file.write_text("# header\n\n10 20#inline\n" + "".join(f"{u} {v}\n" for u, v in EDGES[1:]), encoding="utf-8")
        write_edges(pathlib.Path(f"{plain_file}.0"), [(10, 42)])
        write_edges(pathlib.Path(f"{plain_file}.1"), [])
        run_probe(probe, plain_file, "plain edge-list file")

        # 2. Legacy prefix requires exactly the numbered .0 through .31 set.
        legacy = root / "legacy"
        for index in range(32):
            suffix_edges = {
                1: [(10, 20), (20, 900)],
                9: [(10, 20), (42, 42)],
                31: [(900, 10)],
            }.get(index, [])
            write_edges(pathlib.Path(f"{legacy}.{index}"), suffix_edges)
        run_probe(probe, legacy, "legacy 32-shard prefix")

        # 3. Variable-count directory dispatch, including an empty shard.
        variable = root / "variable"
        variable.mkdir()
        (variable / "shard-count").write_text("3\n", encoding="utf-8")
        write_edges(variable / "edges.0", EDGES[:2])
        write_edges(variable / "edges.1", [])
        write_edges(variable / "edges.2", EDGES[2:])
        run_probe(probe, variable, "variable-shard directory")

        # 4. Plain and gzip source preparation preserve every input record.
        source = root / "source.txt"
        write_edges(source)
        compressed = root / "source.txt.gz"
        with gzip.open(compressed, "wt", encoding="utf-8") as output:
            output.write(EDGE_TEXT)
        prepared_plain = root / "prepared-plain"
        check_prepared(preparer, probe, source, prepared_plain, 3)
        copied_prepared = root / "prepared-plain-missing-last"
        shutil.copytree(prepared_plain, copied_prepared)
        prepared_missing = copied_prepared / "edges.2"
        prepared_missing.unlink()
        output = expect_failure([str(probe), str(copied_prepared)],
                                "prepared directory missing final shard")
        require(str(prepared_missing) in output,
                f"prepared-directory diagnostic omitted {prepared_missing!r}: {output}")
        check_prepared(preparer, probe, compressed, root / "prepared-gzip", 3)

        # 5. Missing middle and final shards identify the expected path.
        for missing_index, label in ((1, "middle"), (2, "last")):
            broken = root / f"missing-{label}"
            broken.mkdir()
            (broken / "shard-count").write_text("3\n", encoding="utf-8")
            for index in range(3):
                if index != missing_index:
                    write_edges(broken / f"edges.{index}", EDGES if index == 0 else [])
            output = expect_failure([str(probe), str(broken)],
                                    f"directory missing {label} shard")
            expected_missing = str(broken / f"edges.{missing_index}")
            require(expected_missing in output,
                    f"missing-shard diagnostic omitted {expected_missing!r}: {output}")

        # Loader and preparer must accept the same edge-list syntax.
        bad_input = root / "input-with-error.txt"
        for record in ("x 2", "-1 2", "1", "1 2 3", "1 18446744073709551616"):
            bad_input.write_text(f"# header\n10 20\n{record}\n", encoding="utf-8")
            for command in ([str(probe), str(bad_input)],
                            [sys.executable, str(preparer), str(bad_input),
                             str(root / "rejected")]):
                output = expect_failure(command, f"invalid record {record!r}")
                require(f"{bad_input}:3" in output, f"missing error location: {output}")
            require(not (root / "rejected").exists(), "partial dataset published")

        # 7. Parse failure leaves both absent and existing destinations intact.
        bad_source = root / "malformed-preparer.txt"
        bad_source.write_text("# header\n10 20\ninvalid\n", encoding="utf-8")
        absent_destination = root / "must-not-appear"
        output = expect_failure(
            [sys.executable, str(preparer), str(bad_source),
             str(absent_destination), "--shards", "3"],
            "preparer malformed input with absent destination")
        require(str(bad_source) in output and ":3" in output,
                f"preparer parse diagnostic lacks path and line 3: {output}")
        require(not absent_destination.exists(),
                f"preparer left partial destination {absent_destination}")

        existing_destination = root / "existing-destination"
        existing_destination.mkdir()
        sentinel = existing_destination / "sentinel.txt"
        sentinel.write_text("keep this content\n", encoding="utf-8")
        output = expect_failure(
            [sys.executable, str(preparer), str(source),
             str(existing_destination), "--shards", "3"],
            "preparer with existing destination")
        require(sentinel.read_text(encoding="utf-8") == "keep this content\n",
                "preparer changed existing destination sentinel content")
        require({entry.name for entry in existing_destination.iterdir()} ==
                {"sentinel.txt"},
                f"preparer added partial output in {existing_destination}")

    print("BFS_GRAPH_INPUT_REGRESSION_PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # Include context from assertions and subprocesses.
        print(f"BFS graph input regression failed: {error}", file=sys.stderr)
        sys.exit(1)
