"""Synthetic durable intake only; no validation, ledger admission or report timing."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import sqlite3
import statistics
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "apps/ingestion-host"))
from luca_ingestion import IntakeStore

CONFIG = json.loads((ROOT / "apps/ingestion-host/example-config.json").read_text())
METADATA = {"source_id": "custodian-a", "format": "luca.transactions.jsonl/1",
            "validation_context_id": "cash-equity-v1-ref42"}


class ReadTracking:
    def __init__(self, source):
        self.source = source
        self.last_byte_at = None

    def read(self, size):
        data = self.source.read(size)
        if not data:
            self.last_byte_at = time.perf_counter()
        return data


class HashOutput:
    def __init__(self):
        self.digest = hashlib.sha256()

    def write(self, data):
        self.digest.update(data)
        return len(data)


def fixture(path, records):
    digest = hashlib.sha256()
    with path.open("wb") as target:
        for index in range(records):
            row = {"record_id": f"cash-{index}", "economic_event_id": f"deposit-{index}",
                   "action": "originate", "account_id": "portfolio-a",
                   "event": {"type": "cash_movement", "effective_at": "2026-10-20T12:00:00Z",
                             "currency": "USD", "amount": "1.000000"}}
            data = (json.dumps(row, separators=(",", ":")) + "\n").encode()
            digest.update(data)
            target.write(data)
        target.flush()
        os.fsync(target.fileno())
    return digest.hexdigest()


def measure(store, source, key, expected_digest):
    with source.open("rb") as stream:
        tracked = ReadTracking(stream)
        start = time.perf_counter()
        receipt = store.receive(METADATA, key, tracked, source.stat().st_size)
        end = time.perf_counter()
    if receipt["payload"]["digest"] != expected_digest:
        raise AssertionError("Receipt hash differs from generated evidence")
    output = HashOutput()
    store.copy_payload(receipt["import_id"], output)
    if output.digest.hexdigest() != expected_digest:
        raise AssertionError("Retained evidence differs from input")
    if store.status(receipt["import_id"])["enumeration"]["state"] != "pending":
        raise AssertionError("Benchmark unexpectedly included validation")
    return {"receipt_seconds": end-start, "post_last_byte_seconds": end-tracked.last_byte_at}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--records", type=int, nargs="+", default=[10000, 100000, 1000000])
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if any(not 0 < count <= 1000000 for count in args.records) or not 1 <= args.repetitions <= 10:
        parser.error("records must be 1..1000000; repetitions 1..10")
    result = {"schema": "luca.durable-intake-benchmark/1", "python": sys.version,
              "sqlite": sqlite3.sqlite_version, "platform": platform.platform(),
              "machine": platform.machine(), "logical_cpus": os.cpu_count(),
              "samples": [], "concurrency": [],
              "source_sha256": {"store.py": hashlib.sha256((ROOT / "apps/ingestion-host/luca_ingestion/store.py").read_bytes()).hexdigest(),
                                "benchmark.py": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()},
              "timing_scope": "Local cached source reads, streaming hash, durable payload, reservation/receipt/work/status writes; output hash verification excluded. No parsing, validation, ledger admission, network or reporting."}
    with tempfile.TemporaryDirectory(prefix="luca-intake-benchmark-") as directory:
        root = Path(directory)
        for records in args.records:
            source = root / f"input-{records}.jsonl"
            expected = fixture(source, records)
            store = IntakeStore(root / f"store-{records}", **CONFIG)
            samples = [measure(store, source, f"sample-{index}", expected) for index in range(args.repetitions)]
            duration = statistics.median(row["receipt_seconds"] for row in samples)
            result["samples"].append({"records": records, "bytes": source.stat().st_size,
                                      "durability_profile": store.capabilities()["durability_profile"],
                                      "runs": samples, "median_receipt_seconds": duration,
                                      "median_bytes_per_second": source.stat().st_size / duration})
            source.unlink()
        source = root / "concurrent.jsonl"
        expected = fixture(source, 10000)
        for partitioned in (False, True):
            for workers in (1, 2, 4):
                base = root / f"parallel-{partitioned}-{workers}"
                stores = [IntakeStore(base, **dict(CONFIG, book_id=f"book-{index}" if partitioned else "fund-a"))
                          for index in range(workers)]
                def ingest(index):
                    return measure(stores[index % workers], source, f"parallel-{index}", expected)
                start = time.perf_counter()
                with ThreadPoolExecutor(max_workers=workers) as pool:
                    runs = list(pool.map(ingest, range(8)))
                duration = time.perf_counter() - start
                result["concurrency"].append({"workers": workers, "separate_books": partitioned,
                                               "imports": 8, "bytes_per_import": source.stat().st_size,
                                               "wall_seconds_including_output_verification": duration,
                                               "runs": runs})
    result["max_rss_platform_units"] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"output": str(args.output), "sizes": [row["records"] for row in result["samples"]]}))


if __name__ == "__main__":
    main()
