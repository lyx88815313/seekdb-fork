#!/usr/bin/env python3
"""MiniVectorDB local benchmark.

Reproduces the shape of the performance benchmark described in the problem
statement: a bulk write phase (10,000 records, committed in batches of 100)
followed by a query phase (1,000 TopK queries with K = 10, half of them with an
equality filter on an ordinary column).

The script drives the container over the JSON Lines protocol and reports

  * the wall clock time of the write phase (from the first request until the
    last response, the clean shutdown that persists the data is reported
    separately),
  * the wall clock time of the query phase (the container start up, which
    rebuilds the vector index, is not part of the query time),
  * the recall of the approximate mode against the exact mode, separated into
    the filtered and the unfiltered group,
  * the peak memory and the disk I/O of the container, read from the cgroup
    files of the container, the same way the platform measures them.

The numbers are produced on the machine the script runs on and are *not* the
official platform measurements.

Usage:
  python benchmark.py --image minivectordb:local
"""
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import time
import uuid


def build_requests(args, seed):
    rng = random.Random(seed)
    records = []
    for i in range(args.rows):
        category = "cat%02d" % (i % args.categories)
        vector = [rng.uniform(-1.0, 1.0) for _ in range(args.dim)]
        records.append((i + 1, category, "payload-%06d" % i, vector))

    writes = [
        {
            "id": 1,
            "op": "execute",
            "sql": (
                "CREATE TABLE bench (id BIGINT PRIMARY KEY, category VARCHAR(16), "
                "payload VARCHAR(32), embedding VECTOR(%d));" % args.dim
            ),
            "search_mode": "exact",
        }
    ]
    request_id = 2
    for start in range(0, len(records), args.batch):
        chunk = records[start:start + args.batch]
        values = ",".join(
            "(%d, '%s', '%s', '[%s]')"
            % (rid, category, payload, ",".join("%.6f" % v for v in vector))
            for rid, category, payload, vector in chunk
        )
        writes.append(
            {
                "id": request_id,
                "op": "execute",
                "sql": "INSERT INTO bench VALUES %s;" % values,
                "search_mode": "exact",
            }
        )
        request_id += 1

    queries = []
    for index in range(args.queries):
        vector = [rng.uniform(-1.0, 1.0) for _ in range(args.dim)]
        literal = "[%s]" % ",".join("%.6f" % v for v in vector)
        filtered = index % 2 == 1
        sql = (
            "SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM bench" % literal
        )
        if filtered:
            sql += " WHERE category = 'cat%02d'" % (index % args.categories)
        sql += " ORDER BY distance LIMIT %d;" % args.k
        queries.append(
            {"id": request_id, "op": "execute", "sql": sql, "search_mode": args.mode}
        )
        request_id += 1
    return writes, queries


class Container:
    """A container run of the database against a named volume."""

    def __init__(self, image, volume, name):
        self.image = image
        self.volume = volume
        self.name = name
        self.process = None
        self.sequence = 0

    def start(self):
        argv = [
            "docker", "run", "--name", self.name, "--interactive",
            "--network=none", "--cpus=2", "--memory=2g", "--memory-swap=2g",
            "--pids-limit=128", "--cap-drop=ALL", "--security-opt=no-new-privileges",
            "--read-only", "--mount", "type=volume,src=%s,dst=/data" % self.volume,
            self.image,
        ]
        started = time.monotonic()
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL)
        ready = self.process.stdout.readline().decode("utf-8", "replace").strip()
        self.startup_seconds = time.monotonic() - started
        if ready != '{"type":"ready"}':
            raise SystemExit("missing ready handshake: %r" % (ready,))

    def send(self, request):
        self.process.stdin.write((json.dumps(request, ensure_ascii=False) + "\n").encode("utf-8"))
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise SystemExit("the database exited before the response")
        return json.loads(line.decode("utf-8"))

    def run(self, requests):
        """Sends the requests and returns the elapsed time and the answers."""
        started = time.monotonic()
        answers = []
        for request in requests:
            answers.append(self.send(request))
        return time.monotonic() - started, answers

    def cgroup(self, path):
        try:
            output = subprocess.run(
                ["docker", "exec", self.name, "cat", path],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=30)
            return output.stdout.decode("utf-8", "replace").strip()
        except (subprocess.SubprocessError, OSError):
            return ""

    def resources(self):
        peak = self.cgroup("/sys/fs/cgroup/memory.peak")
        io_stat = self.cgroup("/sys/fs/cgroup/io.stat")
        read_bytes = write_bytes = 0
        for line in io_stat.splitlines():
            for field in line.split()[1:]:
                if field.startswith("rbytes="):
                    read_bytes += int(field.split("=", 1)[1])
                elif field.startswith("wbytes="):
                    write_bytes += int(field.split("=", 1)[1])
        return peak, read_bytes, write_bytes

    def shutdown(self):
        try:
            self.send({"id": 10 ** 9, "op": "shutdown"})
        except (SystemExit, OSError, ValueError):
            pass
        try:
            self.process.stdin.close()
            self.process.wait(timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            pass
        subprocess.run(["docker", "rm", "--force", self.name],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--batch", type=int, default=100)
    parser.add_argument("--categories", type=int, default=16)
    parser.add_argument("--seed", type=int, default=20261113)
    parser.add_argument("--mode", default="ann", choices=("ann", "exact"))
    args = parser.parse_args()

    writes, queries = build_requests(args, args.seed)
    volume = "mvdb-bench-" + uuid.uuid4().hex[:12]
    subprocess.run(["docker", "volume", "create", volume],
                   stdout=subprocess.DEVNULL, check=True)

    try:
        # ------------------------------------------------------------------
        print("write phase: %d records in batches of %d" % (args.rows, args.batch))
        container = Container(args.image, volume, "mvdb-bench-write")
        container.start()
        write_seconds, answers = container.run(writes)
        failures = [a for a in answers if a.get("ok") is not True]
        if failures:
            raise SystemExit("write phase failed: %r" % (failures[:3],))
        write_peak, write_read, write_write = container.resources()
        shutdown_started = time.monotonic()
        container.shutdown()
        shutdown_seconds = time.monotonic() - shutdown_started
        print("  write: %.3f s, shutdown: %.3f s" % (write_seconds, shutdown_seconds))

        # ------------------------------------------------------------------
        print("query phase: %d TopK queries, K = %d, mode = %s"
              % (args.queries, args.k, args.mode))
        container = Container(args.image, volume, "mvdb-bench-query")
        container.start()
        print("  start up (index rebuild): %.3f s" % container.startup_seconds)
        query_seconds, answers = container.run(queries)
        failures = [a for a in answers if a.get("ok") is not True]
        if failures:
            raise SystemExit("query phase failed: %r" % (failures[:3],))
        found = [[row[0] for row in a["rows"]] for a in answers]
        for ids in found:
            if len(ids) != len(set(ids)):
                raise SystemExit("a query returned duplicate identifiers")
        query_peak, query_read, query_write = container.resources()
        container.shutdown()

        # ------------------------------------------------------------------
        # The exact mode provides the ground truth for the recall measurement.
        exact_container = Container(args.image, volume, "mvdb-bench-exact")
        exact_container.start()
        exact_requests = []
        for request in queries:
            copy = dict(request)
            copy["search_mode"] = "exact"
            exact_requests.append(copy)
        exact_seconds, exact_answers = exact_container.run(exact_requests)
        exact_container.shutdown()
        truth = [[row[0] for row in a["rows"]] for a in exact_answers]

        recalls = []
        for expected, actual in zip(truth, found):
            denominator = min(args.k, len(expected))
            if denominator == 0:
                continue
            recalls.append(len(set(expected) & set(actual)) / denominator)
        filtered = [r for i, r in enumerate(recalls) if i % 2 == 1]
        unfiltered = [r for i, r in enumerate(recalls) if i % 2 == 0]

        def average(values):
            return round(sum(values) / len(values), 4) if values else None

        report = {
            "rows": args.rows,
            "dim": args.dim,
            "queries": args.queries,
            "k": args.k,
            "mode": args.mode,
            "write_seconds": round(write_seconds, 3),
            "write_shutdown_seconds": round(shutdown_seconds, 3),
            "query_seconds": round(query_seconds, 3),
            "query_seconds_exact_reference": round(exact_seconds, 3),
            "startup_seconds": round(container.startup_seconds, 3),
            "recall_avg": average(recalls),
            "recall_filtered": average(filtered),
            "recall_unfiltered": average(unfiltered),
            "write_peak_memory": write_peak,
            "query_peak_memory": query_peak,
            "write_io_read_bytes": write_read,
            "write_io_write_bytes": write_write,
            "query_io_read_bytes": query_read,
            "query_io_write_bytes": query_write,
            "note": ("local measurement on the host running this script; "
                     "not an official platform result"),
        }
        print(json.dumps(report, indent=2))
    finally:
        subprocess.run(["docker", "volume", "rm", "-f", volume],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
