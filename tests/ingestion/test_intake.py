"""Durability tests use separate processes and real files/SQLite, not mock storage."""

from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
import hashlib
import io
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

HOST = Path(__file__).resolve().parent / "legacy_reference"
sys.path.insert(0, str(HOST))
from luca_ingestion import IntakeError, IntakeStore, Limits
from luca_ingestion.store import _patched_wal

CONFIG = json.loads((HOST / "example-config.json").read_text())
METADATA = {"source_id": "custodian-a", "format": "luca.transactions.jsonl/1",
            "validation_context_id": "cash-equity-v1-ref42"}
OPAQUE = b'not json\x00\xff\n{"amount":"wrong"}\n'


def sha(data):
    return hashlib.sha256(data).hexdigest()


CHILD = r'''
import io,json,os,signal,sys
sys.path.insert(0,sys.argv[1])
from luca_ingestion import IntakeStore
config=json.loads(sys.argv[3]); metadata=json.loads(sys.argv[4])
def fault(phase):
    if phase == sys.argv[5]:
        print(phase,flush=True)
        os.kill(os.getpid(), signal.SIGKILL)
store=IntakeStore(sys.argv[2],**config,_fault_hook=fault)
if sys.argv[6] == 'receive':
    receipt=store.receive(metadata,'same-key',io.BytesIO(b'opaque-invalid'),14)
    print(json.dumps(receipt),flush=True)
elif sys.argv[6] == 'chunk':
    import hashlib
    store.put_chunk(sys.argv[7],0,b'abcd',hashlib.sha256(b'abcd').hexdigest())
elif sys.argv[6] == 'verify':
    store.verify_upload(sys.argv[7])
'''


class IntakeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.store = IntakeStore(self.root, **CONFIG)

    def tearDown(self):
        self.temporary.cleanup()

    def receive(self, payload=OPAQUE, key="key", metadata=None, store=None):
        return (store or self.store).receive(metadata or METADATA, key, io.BytesIO(payload), len(payload))

    def error(self, code, operation):
        with self.assertRaises(IntakeError) as caught:
            operation()
        self.assertEqual(caught.exception.code, code)

    def child(self, phase="", operation="receive", subject=""):
        return subprocess.run([sys.executable, "-B", "-c", CHILD, str(HOST), str(self.root),
                               json.dumps(CONFIG), json.dumps(METADATA), phase, operation, subject],
                              capture_output=True, text=True, timeout=20)

    def upload(self, payload=b"abcdefgh", key="upload", chunk=4):
        return self.store.create_upload(key, expected_bytes=len(payload), sha256=sha(payload), chunk_bytes=chunk)["upload_id"]

    def test_opaque_invalid_bytes_receipt_status_work_and_evidence_survive_reopen(self):
        receipt = self.receive()
        reopened = IntakeStore(self.root, **CONFIG)
        self.assertEqual(reopened.status(receipt["import_id"])["receipt"], receipt)
        self.assertEqual(reopened.status(receipt["import_id"])["enumeration"], {"state": "pending", "records": None})
        self.assertFalse(reopened.status(receipt["import_id"])["all_records_committed"])
        self.assertEqual(reopened.pending_work()[0]["kind"], "parse_import")
        target = io.BytesIO()
        reopened.copy_payload(receipt["import_id"], target)
        self.assertEqual(target.getvalue(), OPAQUE)
        self.assertEqual(receipt["payload"]["digest"], sha(OPAQUE))
        self.assertEqual(self.receive(), receipt)
        self.assertEqual(len(reopened.pending_work()), 1)

    def test_idempotency_conflicts_and_source_scoping(self):
        first = self.receive()
        self.error("IDEMPOTENCY_CONFLICT", lambda: self.receive(b"different"))
        changed = dict(METADATA, atomicity="whole_import")
        self.error("IDEMPOTENCY_CONFLICT", lambda: self.receive(metadata=changed))
        new = self.receive(key="another-key")
        self.assertNotEqual(first["import_id"], new["import_id"])
        self.assertEqual(self.store.capabilities()["usage"]["slots"], 2)

    def test_metadata_order_and_default_atomicity_do_not_change_identity(self):
        receipt = self.receive()
        reordered = dict(reversed(list(METADATA.items())))
        reordered["atomicity"] = "independent_groups"
        self.assertEqual(self.receive(metadata=reordered), receipt)

    def test_published_submission_fingerprint_vectors(self):
        fixture = json.loads((Path(__file__).parent / "fixtures/submission-fingerprints.json").read_text())
        for vector in fixture["vectors"]:
            payload = bytes.fromhex(vector["payload_hex"])
            metadata = self.store._metadata(vector["metadata"])
            self.assertEqual(self.store._fingerprint(metadata, sha(payload), len(payload)),
                             vector["expected_submission_sha256"])

    def test_same_key_is_independent_across_sources_and_tenants(self):
        receipt = self.receive()
        store = IntakeStore(self.root, **dict(CONFIG, source_ids=["custodian-b"]))
        other = self.receive(metadata=dict(METADATA, source_id="custodian-b"), store=store)
        self.assertNotEqual(receipt["import_id"], other["import_id"])
        tenant = IntakeStore(self.root, **dict(CONFIG, tenant_id="another-tenant"))
        self.error("NOT_FOUND", lambda: tenant.status(receipt["import_id"]))

    def test_scope_isolation_and_paths_are_not_source_identifiers(self):
        receipt = self.receive()
        other = IntakeStore(self.root, **dict(CONFIG, book_id="../other/book"))
        self.error("NOT_FOUND", lambda: other.status(receipt["import_id"]))
        self.error("NOT_FOUND", lambda: other.copy_payload(receipt["import_id"], io.BytesIO()))
        self.assertEqual(other.path.parent, self.root)
        second = self.receive(store=other)
        self.assertIn("..%2Fother%2Fbook", second["status_url"])

    def test_envelope_errors_never_create_receipts(self):
        for changes, code in [({"source_id": "other"}, "SOURCE_NOT_AUTHORIZED"),
                              ({"format": "unknown"}, "UNSUPPORTED_CONTEXT_OR_FORMAT"),
                              ({"validation_context_id": "latest"}, "UNSUPPORTED_CONTEXT_OR_FORMAT"),
                              ({"extra": 1}, "INVALID_ENVELOPE")]:
            self.error(code, lambda changes=changes: self.receive(metadata=dict(METADATA, **changes)))
        self.assertEqual(self.store.pending_work(), [])
        self.assertEqual(self.store.capabilities()["usage"], {"bytes": 0, "slots": 0})

    def test_context_definitions_and_limits_cannot_drift_on_reopen(self):
        contexts = json.loads(json.dumps(CONFIG["contexts"]))
        contexts["cash-equity-v1-ref42"]["rules"] = "changed"
        self.error("CONFIGURATION_CONFLICT", lambda: IntakeStore(self.root, **dict(CONFIG, contexts=contexts)))
        self.error("CONFIGURATION_CONFLICT", lambda: IntakeStore(self.root, **CONFIG, limits=replace(Limits(), max_chunk_bytes=12)))
        response = self.store.capabilities()
        response["contexts"].clear()
        self.receive()

    def test_payload_length_mismatch_releases_capacity_and_files(self):
        for size in (len(OPAQUE) - 1, len(OPAQUE) + 1):
            self.error("PAYLOAD_LENGTH_MISMATCH", lambda size=size: self.store.receive(METADATA, "bad", io.BytesIO(OPAQUE), size))
        self.assertEqual(self.store.capabilities()["usage"], {"bytes": 0, "slots": 0})
        self.assertEqual(list((self.store.path / "payloads").iterdir()), [])
        self.assertEqual(list((self.store.path / "transfers").iterdir()), [])

    def test_stream_reads_and_payload_range_are_bounded(self):
        data = b"x" * (2 * 1024 * 1024 + 19)
        class Stream(io.BytesIO):
            def read(self, n=-1):
                if not 0 < n <= 1024 * 1024:
                    raise AssertionError("unbounded read")
                return super().read(n)
        receipt = self.store.receive(METADATA, "large", Stream(data), len(data))
        target = io.BytesIO()
        self.store.copy_payload(receipt["import_id"], target, offset=len(data)-20, length=20)
        self.assertEqual(target.getvalue(), data[-20:])
        self.error("LIMIT_EXCEEDED", lambda: self.store.copy_payload(receipt["import_id"], target, offset=len(data)+1))

    def test_payload_output_handles_partial_writes(self):
        receipt = self.receive()
        class Partial(io.BytesIO):
            def write(self, data):
                return super().write(data[:3])
        target = Partial()
        self.store.copy_payload(receipt["import_id"], target)
        self.assertEqual(target.getvalue(), OPAQUE)

    def test_empty_payload_and_empty_upload(self):
        self.assertEqual(self.receive(b"")["payload"]["bytes"], 0)
        upload_id = self.upload(b"", "empty")
        self.assertEqual(self.store.verify_upload(upload_id)["state"], "ready")
        receipt = self.store.receive_upload(METADATA, "empty-upload", upload_id)
        self.assertEqual(receipt["payload"]["digest"], sha(b""))

    def test_quota_reservation_backpressure_and_retry_at_capacity(self):
        root = self.root / "limited"
        store = IntakeStore(root, **CONFIG, limits=replace(Limits(), max_retained_bytes=8, max_pending_imports=1))
        receipt = self.receive(b"12345678", store=store)
        self.assertEqual(self.receive(b"12345678", store=store), receipt)
        self.error("CAPACITY_UNAVAILABLE", lambda: self.receive(b"x", key="new", store=store))
        self.assertEqual(store.capabilities()["usage"], {"bytes": 8, "slots": 1})

    def test_concurrent_same_key_receives_create_one_receipt_and_work_item(self):
        with ThreadPoolExecutor(max_workers=8) as pool:
            receipts = list(pool.map(lambda _: self.receive(), range(16)))
        self.assertEqual(len({row["import_id"] for row in receipts}), 1)
        self.assertEqual(len(self.store.pending_work()), 1)
        self.assertEqual(self.store.capabilities()["usage"], {"bytes": len(OPAQUE), "slots": 1})
        self.assertEqual(len(list((self.store.path / "payloads").iterdir())), 1)

    def test_cross_process_same_key_receives(self):
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: self.child(), range(4)))
        for result in results:
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len({json.loads(result.stdout)["import_id"] for result in results}), 1)
        self.assertEqual(len(self.store.pending_work()), 1)

    def test_crash_boundaries_receipt_and_work_are_atomic(self):
        for phase in ("after_reservation", "after_payload_flush", "after_payload_install",
                      "before_receipt_commit", "after_receipt_commit"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as directory:
                old_root = self.root
                self.root = Path(directory)
                try:
                    result = self.child(phase)
                    self.assertEqual(result.returncode, -signal.SIGKILL, result.stderr)
                    self.assertIn(phase, result.stdout)
                    store = IntakeStore(self.root, **CONFIG)
                    tasks = store.pending_work()
                    self.assertEqual(len(tasks), int(phase == "after_receipt_commit"))
                    store.recover()
                    self.assertEqual(store.capabilities()["usage"]["slots"], len(tasks))
                    with store._db() as db:
                        for table in ("imports", "events"):
                            self.assertEqual(db.execute(f"SELECT count(*) FROM {table}").fetchone()[0], len(tasks))
                        self.assertEqual(db.execute("PRAGMA integrity_check").fetchone()[0], "ok")
                        self.assertEqual(db.execute("PRAGMA foreign_key_check").fetchall(), [])
                    if tasks:
                        target = io.BytesIO()
                        store.copy_payload(tasks[0]["subject_id"], target)
                        self.assertEqual(target.getvalue(), b"opaque-invalid")
                        receipt = store.receive(METADATA, "same-key", io.BytesIO(b"opaque-invalid"), 14)
                        self.assertEqual(receipt["import_id"], tasks[0]["subject_id"])
                    else:
                        self.assertEqual(list((store.path / "payloads").iterdir()), [])
                finally:
                    self.root = old_root

    def test_recovery_does_not_reclaim_an_active_writer(self):
        entered, proceed = threading.Event(), threading.Event()
        class SlowStream(io.BytesIO):
            def read(self, size=-1):
                entered.set()
                if not proceed.wait(10):
                    raise AssertionError("blocked")
                return super().read(size)
        with ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(self.store.receive, METADATA, "slow", SlowStream(OPAQUE), len(OPAQUE))
            try:
                self.assertTrue(entered.wait(10))
                self.assertEqual(self.store.recover()["reclaimed"], [])
            finally:
                proceed.set()
            self.assertEqual(pending.result()["receipt"], "durable")

    def test_upload_out_of_order_resume_and_attach_retry(self):
        upload_id = self.upload()
        self.store.put_chunk(upload_id, 1, b"efgh", sha(b"efgh"))
        self.store = IntakeStore(self.root, **CONFIG)
        self.assertEqual(self.store.upload_status(upload_id)["received_ranges"], [[1,1]])
        self.store.put_chunk(upload_id, 0, b"abcd", sha(b"abcd"))
        self.assertEqual(self.store.upload_status(upload_id)["state"], "verifying")
        self.assertEqual(self.store.pending_work()[0]["kind"], "verify_upload")
        self.store.verify_upload(upload_id)
        receipt = self.store.receive_upload(METADATA, "import", upload_id)
        self.assertEqual(self.store.receive_upload(METADATA, "import", upload_id), receipt)
        self.assertEqual(self.store.upload_status(upload_id)["state"], "attached")
        self.error("UPLOAD_FROZEN", lambda: self.store.receive_upload(METADATA, "different", upload_id))
        target = io.BytesIO()
        self.store.copy_payload(receipt["import_id"], target)
        self.assertEqual(target.getvalue(), b"abcdefgh")
        self.assertEqual(self.store.pending_work()[0]["kind"], "parse_import")

    def test_chunk_retry_conflict_size_and_checksum(self):
        upload_id = self.upload()
        self.error("CHECKSUM_MISMATCH", lambda: self.store.put_chunk(upload_id, 0, b"abcd", sha(b"nope")))
        self.error("CHUNK_LENGTH_MISMATCH", lambda: self.store.put_chunk(upload_id, 0, b"abc", sha(b"abc")))
        self.store.put_chunk(upload_id, 0, b"abcd", sha(b"abcd"))
        self.store.put_chunk(upload_id, 0, b"abcd", sha(b"abcd"))
        self.error("IDEMPOTENCY_CONFLICT", lambda: self.store.put_chunk(upload_id, 0, b"ABCD", sha(b"ABCD")))
        self.assertEqual(self.store.upload_status(upload_id)["received_ranges"], [[0,0]])

    def test_upload_bad_full_checksum_retains_failed_state_without_import(self):
        upload_id = self.upload(b"12345678")
        for number, data in enumerate((b"abcd", b"efgh")):
            self.store.put_chunk(upload_id, number, data, sha(data))
        self.error("CHECKSUM_MISMATCH", lambda: self.store.verify_upload(upload_id))
        self.assertEqual(self.store.upload_status(upload_id)["state"], "failed")
        self.assertEqual(self.store.upload_status(upload_id)["error"]["code"], "CHECKSUM_MISMATCH")
        self.error("UPLOAD_NOT_READY", lambda: self.store.receive_upload(METADATA, "bad", upload_id))
        self.assertEqual(self.store.pending_work(), [])

    def test_upload_creation_idempotency_and_cross_transport_receipt_identity(self):
        upload_id = self.upload()
        self.assertEqual(self.upload(), upload_id)
        self.error("IDEMPOTENCY_CONFLICT", lambda: self.upload(b"different", "upload"))
        for number, data in enumerate((b"abcd", b"efgh")):
            self.store.put_chunk(upload_id, number, data, sha(data))
        self.store.verify_upload(upload_id)
        receipt = self.store.receive_upload(METADATA, "import", upload_id)
        self.assertEqual(self.receive(b"abcdefgh", key="import"), receipt)

    def test_upload_expiry_releases_only_unattached_reservations(self):
        now = [1_000_000_000]
        store = IntakeStore(self.root / "expiry", **CONFIG, limits=replace(Limits(), upload_ttl_seconds=1), clock=lambda: now[0])
        upload = store.create_upload("u", expected_bytes=4, sha256=sha(b"abcd"), chunk_bytes=4)["upload_id"]
        store.put_chunk(upload, 0, b"abcd", sha(b"abcd"))
        store.verify_upload(upload)
        receipt = store.receive_upload(METADATA, "import", upload)
        other = store.create_upload("other", expected_bytes=4, sha256=sha(b"abcd"), chunk_bytes=4)["upload_id"]
        now[0] += 2_000_000_000
        self.error("UPLOAD_EXPIRED", lambda: store.put_chunk(other, 0, b"abcd", sha(b"abcd")))
        self.assertEqual(store.recover()["reclaimed"], [other])
        self.assertEqual(store.capabilities()["usage"], {"bytes": 4, "slots": 1})
        self.assertEqual(store.receive_upload(METADATA, "import", upload), receipt)
        self.assertEqual(store.upload_status(upload)["state"], "attached")

    def test_upload_chunk_and_install_crash_recovery(self):
        for phase, operation in (("after_chunk_flush", "chunk"), ("after_chunk_commit", "chunk"),
                                 ("after_upload_install", "verify")):
            with self.subTest(phase=phase):
                upload_id = self.upload(b"abcd", phase)
                if operation == "verify":
                    self.store.put_chunk(upload_id, 0, b"abcd", sha(b"abcd"))
                result = self.child(phase, operation, upload_id)
                self.assertEqual(result.returncode, -signal.SIGKILL, result.stderr)
                self.store.put_chunk(upload_id, 0, b"abcd", sha(b"abcd"))
                self.assertEqual(self.store.verify_upload(upload_id)["state"], "ready")
                self.store.receive_upload(METADATA, phase, upload_id)

    def test_flush_failure_never_returns_a_receipt(self):
        with patch("luca_ingestion.store._sync_file", side_effect=OSError("injected disk failure")):
            with self.assertRaises(OSError):
                self.receive()
        self.assertEqual(self.store.pending_work(), [])
        self.assertEqual(self.store.capabilities()["usage"], {"bytes": 0, "slots": 0})

    def test_status_event_storage_failure_rolls_back_receipt_and_work(self):
        with self.store._db(write=True) as db:
            db.execute("CREATE TRIGGER fail_event BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'injected failure'); END")
        self.error("STORAGE_UNAVAILABLE", lambda: self.receive())
        self.assertEqual(self.store.pending_work(), [])
        with self.store._db() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM imports").fetchone()[0], 0)
        self.assertEqual(self.store.capabilities()["usage"], {"bytes": 0, "slots": 0})

    def test_events_resume_without_duplication_and_bounded_limits(self):
        receipt = self.receive()
        page = self.store.events(receipt["import_id"], limit=1)
        self.assertEqual(len(page["items"]), 1)
        self.assertEqual(page["items"][0]["type"], "import.received")
        self.assertEqual(self.store.events(receipt["import_id"], after=page["next_after"])["items"], [])
        self.error("INVALID_CURSOR", lambda: self.store.events(receipt["import_id"], after="-1"))
        self.error("LIMIT_EXCEEDED", lambda: self.store.pending_work(limit=1001))

    def test_patched_wal_detection_and_actual_durability_settings(self):
        for version in ((3,51,3), (3,52,0), (3,50,7), (3,44,6)):
            self.assertTrue(_patched_wal(version))
        for version in ((3,51,2), (3,50,6), (3,45,3), (3,44,5)):
            self.assertFalse(_patched_wal(version))
        with self.store._db() as db:
            self.assertEqual(db.execute("PRAGMA journal_mode").fetchone()[0], self.store.mode)
            self.assertEqual(db.execute("PRAGMA synchronous").fetchone()[0], 2 if self.store.mode == "wal" else 3)
            self.assertEqual(db.execute("PRAGMA integrity_check").fetchone()[0], "ok")

    def test_storage_mode_is_pinned_across_runtime_changes(self):
        with patch("luca_ingestion.store.sqlite3.sqlite_version_info", (3,45,3)):
            older = IntakeStore(self.root / "pinned-delete", **CONFIG)
        reopened = IntakeStore(self.root / "pinned-delete", **CONFIG)
        self.assertEqual(older.mode, "delete")
        self.assertEqual(reopened.mode, "delete")
        if self.store.mode == "wal":
            with patch("luca_ingestion.store.sqlite3.sqlite_version_info", (3,45,3)), \
                 patch("luca_ingestion.store.sqlite3.connect") as connect:
                self.error("UNPATCHED_WAL_RUNTIME", lambda: IntakeStore(self.root, **CONFIG))
                connect.assert_not_called()

    def test_cli_receipt_status_and_conflict(self):
        config, metadata, payload = (self.root / name for name in ("config.json", "metadata.json", "data.bin"))
        config.write_text(json.dumps(CONFIG)); metadata.write_text(json.dumps(METADATA)); payload.write_bytes(OPAQUE)
        command = [sys.executable, "-B", "-m", "luca_ingestion", "--root", str(self.root), "--config", str(config)]
        env = dict(os.environ, PYTHONPATH=str(HOST), PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run(command + ["receive", "--metadata", str(metadata), "--payload", str(payload), "--key", "cli"], env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(result.stdout)
        result = subprocess.run(command + ["status", receipt["import_id"]], env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["receipt"], receipt)
        payload.write_bytes(b"changed")
        result = subprocess.run(command + ["receive", "--metadata", str(metadata), "--payload", str(payload), "--key", "cli"], env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stderr)["code"], "IDEMPOTENCY_CONFLICT")


if __name__ == "__main__":
    unittest.main()
