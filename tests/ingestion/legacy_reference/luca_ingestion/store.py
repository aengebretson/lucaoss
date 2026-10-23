"""Durable receipt, immutable evidence and resumable transfer for a single book.

This host performs storage work only. Queued parse work is retained for the next
slice; it does not call transaction parsers or claim ledger acceptance.
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import sys
import time
from typing import BinaryIO, Callable, Iterator, Mapping
from uuid import uuid4
from urllib.parse import quote


class IntakeError(Exception):
    def __init__(self, code: str, *, retryable: bool = False):
        super().__init__(code)
        self.code = code
        self.retryable = retryable


@dataclass(frozen=True)
class Limits:
    max_payload_bytes: int = 256 * 1024 * 1024
    max_retained_bytes: int = 4 * 1024 * 1024 * 1024
    max_pending_imports: int = 1024
    max_chunk_bytes: int = 4 * 1024 * 1024
    max_chunks: int = 8192
    max_page_size: int = 1000
    upload_ttl_seconds: int = 86400


def _json(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False)


def _token(value: object, maximum: int = 256) -> str:
    if not isinstance(value, str) or not value:
        raise IntakeError("INVALID_ENVELOPE")
    try:
        if len(value.encode("utf-8")) > maximum:
            raise IntakeError("INVALID_ENVELOPE")
    except UnicodeEncodeError as error:
        raise IntakeError("INVALID_ENVELOPE") from error
    return value


def _integer(value: object, minimum: int, maximum: int) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise IntakeError("LIMIT_EXCEEDED")
    return value


def _digest(value: object) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise IntakeError("INVALID_DIGEST")
    return value


def _framed_hash(fields: list[str | None]) -> str:
    digest = hashlib.sha256()
    for value in fields:
        if value is None:
            digest.update(b"\0")
        else:
            encoded = value.encode("utf-8")
            digest.update(b"\1" + len(encoded).to_bytes(8, "big") + encoded)
    return digest.hexdigest()


def _timestamp(ns: int) -> str:
    seconds, remainder = divmod(ns, 1_000_000_000)
    return datetime.fromtimestamp(seconds, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S") + f".{remainder:09d}Z"


def _sync_file(fd: int) -> None:
    os.fsync(fd)
    if sys.platform == "darwin":
        fcntl.fcntl(fd, fcntl.F_FULLFSYNC)


def _sync_dir(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _write_all(target: BinaryIO, data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = target.write(view)
        if type(written) is not int or not 0 < written <= len(view):
            raise IntakeError("OUTPUT_WRITE_FAILED")
        view = view[written:]


def _mkdir(path: Path) -> None:
    missing = []
    current = path
    while not current.exists():
        missing.append(current)
        current = current.parent
    for current in reversed(missing):
        current.mkdir(mode=0o700, exist_ok=True)
        _sync_dir(current.parent)
    _sync_dir(path)
    _sync_dir(path.parent)


def _patched_wal(version: tuple[int, ...]) -> bool:
    # SQLite's WAL-reset fix was also backported to these two release lines.
    return (version >= (3, 51, 3) or (3, 50, 7) <= version < (3, 51, 0)
            or (3, 44, 6) <= version < (3, 45, 0))


_SCHEMA = (
    "CREATE TABLE config (id INTEGER PRIMARY KEY CHECK(id=1), value TEXT NOT NULL)",
    "CREATE TABLE capacity (id INTEGER PRIMARY KEY CHECK(id=1), bytes INTEGER NOT NULL CHECK(bytes>=0), slots INTEGER NOT NULL CHECK(slots>=0))",
    "INSERT INTO capacity VALUES (1,0,0)",
    """CREATE TABLE transfers (
      id TEXT PRIMARY KEY, kind TEXT NOT NULL, state TEXT NOT NULL,
      size INTEGER NOT NULL, digest TEXT, chunk_size INTEGER,
      upload_key TEXT UNIQUE, request_hash TEXT, created_ns INTEGER NOT NULL,
      expires_ns INTEGER NOT NULL, reserved INTEGER NOT NULL CHECK(reserved IN (0,1)))""",
    "CREATE INDEX recovery_candidates ON transfers(id) WHERE state!='attached' AND reserved=1",
    "CREATE TABLE chunks (transfer_id TEXT NOT NULL REFERENCES transfers(id), number INTEGER NOT NULL, digest TEXT NOT NULL, PRIMARY KEY(transfer_id,number))",
    """CREATE TABLE imports (
      id TEXT PRIMARY KEY, source TEXT NOT NULL, request_key TEXT NOT NULL,
      fingerprint TEXT NOT NULL, metadata TEXT NOT NULL, context TEXT NOT NULL,
      receipt TEXT NOT NULL, transfer_id TEXT NOT NULL UNIQUE REFERENCES transfers(id),
      UNIQUE(source,request_key))""",
    """CREATE TABLE work (
      id TEXT PRIMARY KEY, kind TEXT NOT NULL, subject_id TEXT NOT NULL,
      state TEXT NOT NULL, UNIQUE(kind,subject_id))""",
    "CREATE INDEX ready_work ON work(state,id)",
    """CREATE TABLE events (
      import_id TEXT NOT NULL REFERENCES imports(id), revision INTEGER NOT NULL,
      body TEXT NOT NULL, PRIMARY KEY(import_id,revision))""",
)


class IntakeStore:
    """Trusted local API, scoped to one tenant/book. Each operation owns its DB connection.

    The configured directory is private, local storage, not an untrusted shared
    filesystem. Callers provide authorization before constructing this object.
    Limits and context definitions are pinned on first creation of a shard.
    """

    def __init__(self, root: str | Path, tenant_id: str, book_id: str, *,
                 source_ids: list[str], contexts: Mapping[str, Mapping[str, str]],
                 limits: Limits = Limits(), clock: Callable[[], int] = time.time_ns,
                 _fault_hook: Callable[[str], None] | None = None):
        if os.name != "posix":
            raise IntakeError("UNSUPPORTED_PLATFORM")
        self.tenant = _token(tenant_id)
        self.book = _token(book_id)
        self.sources = frozenset(_token(source) for source in source_ids)
        if not self.sources or not contexts:
            raise IntakeError("INVALID_CONFIGURATION")
        self.contexts = json.loads(_json(contexts))
        fields = {"adapter", "normalization", "rules", "engine", "reference_snapshot"}
        for context_id, context in self.contexts.items():
            _token(context_id)
            if set(context) != fields:
                raise IntakeError("INVALID_CONFIGURATION")
            for value in context.values():
                _token(value)
        for value in asdict(limits).values():
            _integer(value, 1, 2**53 - 1)
        self.limits = limits
        self.clock = clock
        self._fault = _fault_hook or (lambda phase: None)
        shard = _framed_hash(["shard/1", self.tenant, self.book])
        self.path = Path(root).absolute() / shard
        _mkdir(self.path)
        for directory in ("payloads", "transfers", "locks"):
            _mkdir(self.path / directory)
        self.database = self.path / "intake.sqlite3"
        config = _json({"tenant": self.tenant, "book": self.book,
                        "contexts": self.contexts, "limits": asdict(limits)})
        fd = os.open(self.path / "locks" / "initialization", os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            self.mode = self._storage_mode()
            with self._db(write=True, initialize=True) as db:
                version = db.execute("PRAGMA user_version").fetchone()[0]
                if version == 0:
                    if db.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchone():
                        raise IntakeError("UNRECOGNIZED_DATABASE")
                    for statement in _SCHEMA:
                        db.execute(statement)
                    db.execute("INSERT INTO config VALUES (1,?)", (config,))
                    db.execute("PRAGMA user_version=1")
                elif version != 1:
                    raise IntakeError("UNSUPPORTED_SCHEMA")
                if db.execute("SELECT value FROM config WHERE id=1").fetchone()[0] != config:
                    raise IntakeError("CONFIGURATION_CONFLICT")
        finally:
            os.close(fd)

    def _storage_mode(self) -> str:
        # Pin before opening SQLite: even opening/closing an old WAL connection
        # can checkpoint. Mixed runtimes must not silently convert a live store.
        profile = self.path / "storage-profile.json"
        if profile.exists():
            value = json.loads(profile.read_text())
            if value not in ({"schema": 1, "journal_mode": "wal"},
                             {"schema": 1, "journal_mode": "delete"}):
                raise IntakeError("UNSUPPORTED_STORAGE_PROFILE")
            mode = value["journal_mode"]
        else:
            if self.database.exists():
                raise IntakeError("UNRECOGNIZED_DATABASE")
            mode = "wal" if _patched_wal(sqlite3.sqlite_version_info) else "delete"
            temporary = self.path / "storage-profile.tmp"
            with temporary.open("wb") as target:
                target.write(_json({"schema": 1, "journal_mode": mode}).encode())
                target.flush()
                _sync_file(target.fileno())
            os.replace(temporary, profile)
            _sync_dir(self.path)
        if mode == "wal" and not _patched_wal(sqlite3.sqlite_version_info):
            raise IntakeError("UNPATCHED_WAL_RUNTIME")
        return mode

    @contextmanager
    def _db(self, *, write: bool = False, initialize: bool = False) -> Iterator[sqlite3.Connection]:
        db = None
        try:
            if self.mode == "wal" and not _patched_wal(sqlite3.sqlite_version_info):
                raise IntakeError("UNPATCHED_WAL_RUNTIME")
            db = sqlite3.connect(self.database, isolation_level=None, timeout=10)
            db.row_factory = sqlite3.Row
            if initialize:
                current = db.execute("PRAGMA journal_mode").fetchone()[0]
                if current == "wal" and not _patched_wal(sqlite3.sqlite_version_info):
                    raise IntakeError("UNPATCHED_WAL_RUNTIME")
                if current != self.mode and db.execute("PRAGMA user_version").fetchone()[0] != 0:
                    raise IntakeError("DURABILITY_UNAVAILABLE")
                actual = db.execute(f"PRAGMA journal_mode={self.mode}").fetchone()[0]
            else:
                actual = db.execute("PRAGMA journal_mode").fetchone()[0]
            if actual != self.mode:
                raise IntakeError("DURABILITY_UNAVAILABLE")
            db.execute("PRAGMA synchronous=" + ("FULL" if self.mode == "wal" else "EXTRA"))
            db.execute("PRAGMA foreign_keys=ON")
            if sys.platform == "darwin":
                db.execute("PRAGMA fullfsync=ON")
                db.execute("PRAGMA checkpoint_fullfsync=ON")
                if db.execute("PRAGMA fullfsync").fetchone()[0] != 1:
                    raise IntakeError("DURABILITY_UNAVAILABLE")
            if db.execute("PRAGMA synchronous").fetchone()[0] != (2 if self.mode == "wal" else 3):
                raise IntakeError("DURABILITY_UNAVAILABLE")
            db.execute("BEGIN IMMEDIATE" if write else "BEGIN")
            yield db
            db.commit()
            if write:
                _sync_dir(self.path)
        except sqlite3.Error as error:
            if db is not None:
                db.rollback()
            raise IntakeError("STORAGE_UNAVAILABLE", retryable=True) from error
        except BaseException:
            if db is not None:
                db.rollback()
            raise
        finally:
            if db is not None:
                db.close()

    @contextmanager
    def _guard(self, transfer_id: str, *, blocking: bool = True) -> Iterator[None]:
        if not re.fullmatch(r"[0-9a-f]{32}", transfer_id):
            raise IntakeError("NOT_FOUND")
        # A fixed namespace prevents failed/unknown transfers from accumulating
        # unlimited lock files. Collisions only serialize unrelated local work.
        fd = os.open(self.path / "locks" / transfer_id[:3], os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
            yield
        finally:
            os.close(fd)

    def _part(self, transfer_id: str) -> Path:
        return self.path / "transfers" / transfer_id

    def _payload(self, transfer_id: str) -> Path:
        return self.path / "payloads" / transfer_id

    def _metadata(self, metadata: Mapping[str, object]) -> dict:
        if not isinstance(metadata, Mapping):
            raise IntakeError("INVALID_ENVELOPE")
        result = dict(metadata)
        if set(result) - {"source_id", "format", "validation_context_id", "atomicity", "client_reference", "replaces"}:
            raise IntakeError("INVALID_ENVELOPE")
        source = _token(result.get("source_id"))
        if source not in self.sources:
            raise IntakeError("SOURCE_NOT_AUTHORIZED")
        context = self.contexts.get(_token(result.get("validation_context_id")))
        if context is None or result.get("format") != context["adapter"]:
            raise IntakeError("UNSUPPORTED_CONTEXT_OR_FORMAT")
        result.setdefault("atomicity", "independent_groups")
        if result["atomicity"] not in ("independent_groups", "whole_import"):
            raise IntakeError("INVALID_ENVELOPE")
        if "client_reference" in result:
            _token(result["client_reference"])
        if "replaces" in result:
            replacement = result["replaces"]
            if not isinstance(replacement, dict) or set(replacement) != {"import_id", "record_ids"}:
                raise IntakeError("INVALID_ENVELOPE")
            _token(replacement["import_id"])
            ids = replacement["record_ids"]
            if not isinstance(ids, list) or len(ids) > 1000:
                raise IntakeError("INVALID_ENVELOPE")
            result["replaces"] = {"import_id": replacement["import_id"],
                                  "record_ids": sorted(set(_token(item) for item in ids))}
        if len(_json(result).encode("utf-8")) > 16384:
            raise IntakeError("LIMIT_EXCEEDED")
        return result

    def _fingerprint(self, metadata: dict, digest: str, size: int) -> str:
        replacement = metadata.get("replaces")
        return _framed_hash(["submission/1", self.tenant, self.book, metadata["source_id"],
                             metadata["format"], metadata["validation_context_id"], metadata["atomicity"],
                             metadata.get("client_reference"), replacement["import_id"] if replacement else None,
                             _json(replacement["record_ids"]) if replacement else None, digest, str(size)])

    def _reserve(self, db: sqlite3.Connection, transfer_id: str, size: int, *, kind: str,
                 digest: str | None = None, chunk_size: int | None = None,
                 key: str | None = None, request_hash: str | None = None) -> None:
        used = db.execute("SELECT bytes,slots FROM capacity WHERE id=1").fetchone()
        if used[0] + size > self.limits.max_retained_bytes or used[1] >= self.limits.max_pending_imports:
            raise IntakeError("CAPACITY_UNAVAILABLE", retryable=True)
        now = self.clock()
        db.execute("INSERT INTO transfers VALUES (?,?,?,?,?,?,?,?,?,?,1)",
                   (transfer_id, kind, "receiving" if kind == "direct" else "uploading", size,
                    digest, chunk_size, key, request_hash, now, now + self.limits.upload_ttl_seconds * 10**9))
        db.execute("UPDATE capacity SET bytes=bytes+?,slots=slots+1 WHERE id=1", (size,))

    def _copy(self, stream: BinaryIO, size: int, target: BinaryIO | None = None) -> str:
        digest = hashlib.sha256()
        remaining = size
        while remaining:
            requested = min(1024 * 1024, remaining)
            block = stream.read(requested)
            if not isinstance(block, bytes) or not block:
                raise IntakeError("PAYLOAD_LENGTH_MISMATCH")
            if len(block) > requested:
                raise IntakeError("PAYLOAD_LENGTH_MISMATCH")
            if target is not None:
                _write_all(target, block)
            digest.update(block)
            remaining -= len(block)
        if stream.read(1) != b"":
            raise IntakeError("PAYLOAD_LENGTH_MISMATCH")
        return digest.hexdigest()

    def _install(self, transfer_id: str) -> None:
        part = self._part(transfer_id)
        if part.exists():
            os.chmod(part, 0o400)
            with part.open("rb") as stream:
                _sync_file(stream.fileno())
            # Unique transfer names; no payload is shared across receipts/sessions.
            os.replace(part, self._payload(transfer_id))
            _sync_dir(self.path / "payloads")
            _sync_dir(self.path / "transfers")
        elif not self._payload(transfer_id).is_file():
            raise IntakeError("PAYLOAD_UNAVAILABLE")

    def _existing(self, db: sqlite3.Connection, metadata: dict, key: str):
        return db.execute("SELECT * FROM imports WHERE source=? AND request_key=?",
                          (metadata["source_id"], key)).fetchone()

    def _attach(self, db: sqlite3.Connection, transfer_id: str, metadata: dict,
                key: str, digest: str, size: int) -> dict:
        fingerprint = self._fingerprint(metadata, digest, size)
        existing = self._existing(db, metadata, key)
        if existing:
            if existing["fingerprint"] != fingerprint:
                raise IntakeError("IDEMPOTENCY_CONFLICT")
            return json.loads(existing["receipt"])
        import_id = "imp-" + uuid4().hex
        now = _timestamp(self.clock())
        receipt = {"import_id": import_id, "receipt": "durable", "received_at": now,
                   "payload": {"algorithm": "sha256", "digest": digest, "bytes": size},
                   "durability_profile": "local-fsync-" + self.mode + "-v1",
                   "status_url": f"/v1/books/{quote(self.book, safe='')}/imports/{import_id}"}
        db.execute("INSERT INTO imports VALUES (?,?,?,?,?,?,?,?)",
                   (import_id, metadata["source_id"], key, fingerprint, _json(metadata),
                    _json(self.contexts[metadata["validation_context_id"]]), _json(receipt), transfer_id))
        db.execute("UPDATE transfers SET state='attached',digest=? WHERE id=?", (digest, transfer_id))
        db.execute("INSERT INTO work VALUES (?,?,?,'queued')", (uuid4().hex, "parse_import", import_id))
        event = {"event_id": uuid4().hex, "import_id": import_id, "revision": "1",
                 "type": "import.received", "occurred_at": now, "subject_ids": [import_id],
                 "links": {"status": receipt["status_url"]}}
        db.execute("INSERT INTO events VALUES (?,1,?)", (import_id, _json(event)))
        self._fault("before_receipt_commit")
        return receipt

    def receive(self, metadata: Mapping[str, object], key: str, stream: BinaryIO, size: int) -> dict:
        """Stream opaque bytes; acknowledge only after evidence, receipt and work commit."""
        metadata = self._metadata(metadata)
        key = _token(key, 128)
        size = _integer(size, 0, self.limits.max_payload_bytes)
        with self._db() as db:
            existing = self._existing(db, metadata, key)
        if existing:
            digest = self._copy(stream, size)
            if existing["fingerprint"] != self._fingerprint(metadata, digest, size):
                raise IntakeError("IDEMPOTENCY_CONFLICT")
            return json.loads(existing["receipt"])
        transfer_id = uuid4().hex
        with self._guard(transfer_id):
            try:
                with self._db(write=True) as db:
                    self._reserve(db, transfer_id, size, kind="direct")
                self._fault("after_reservation")
                with self._part(transfer_id).open("xb") as target:
                    digest = self._copy(stream, size, target)
                    target.flush()
                    _sync_file(target.fileno())
                self._fault("after_payload_flush")
                self._install(transfer_id)
                self._fault("after_payload_install")
                with self._db(write=True) as db:
                    receipt = self._attach(db, transfer_id, metadata, key, digest, size)
                self._fault("after_receipt_commit")
                return receipt
            finally:
                # Also releases losing concurrent same-key reservations. Attached
                # evidence is never deleted, even if returning the receipt failed.
                self._release_unattached(transfer_id, delete_row=True)

    def _release_unattached(self, transfer_id: str, *, delete_row: bool = False) -> bool:
        with self._db(write=True) as db:
            row = db.execute("SELECT * FROM transfers WHERE id=?", (transfer_id,)).fetchone()
            if row is None or row["state"] == "attached":
                return False
            for path in (self._part(transfer_id), self._payload(transfer_id)):
                path.unlink(missing_ok=True)
                _sync_dir(path.parent)
            if row["reserved"]:
                db.execute("UPDATE capacity SET bytes=bytes-?,slots=slots-1 WHERE id=1", (row["size"],))
            db.execute("UPDATE work SET state='complete' WHERE kind='verify_upload' AND subject_id=?", (transfer_id,))
            db.execute("UPDATE transfers SET reserved=0,state='expired' WHERE id=?", (transfer_id,))
            if delete_row:
                db.execute("DELETE FROM transfers WHERE id=?", (transfer_id,))
            return True

    def create_upload(self, key: str, *, expected_bytes: int, sha256: str, chunk_bytes: int) -> dict:
        key = _token(key, 128)
        size = _integer(expected_bytes, 0, self.limits.max_payload_bytes)
        digest = _digest(sha256)
        chunk_size = _integer(chunk_bytes, 1, self.limits.max_chunk_bytes)
        count = (size + chunk_size - 1) // chunk_size
        if count > self.limits.max_chunks:
            raise IntakeError("LIMIT_EXCEEDED")
        request_hash = _framed_hash(["upload/1", str(size), digest, str(chunk_size)])
        with self._db(write=True) as db:
            row = db.execute("SELECT * FROM transfers WHERE upload_key=?", (key,)).fetchone()
            if row:
                if row["request_hash"] != request_hash:
                    raise IntakeError("IDEMPOTENCY_CONFLICT")
                transfer_id = row["id"]
            else:
                transfer_id = uuid4().hex
                self._reserve(db, transfer_id, size, kind="upload", digest=digest,
                              chunk_size=chunk_size, key=key, request_hash=request_hash)
                if not count:
                    db.execute("UPDATE transfers SET state='verifying' WHERE id=?", (transfer_id,))
                    db.execute("INSERT INTO work VALUES (?,?,?,'queued')", (uuid4().hex, "verify_upload", transfer_id))
        return self.upload_status(transfer_id)

    def _upload(self, db: sqlite3.Connection, upload_id: str) -> sqlite3.Row:
        row = db.execute("SELECT * FROM transfers WHERE id=? AND kind='upload'", (upload_id,)).fetchone()
        if row is None:
            raise IntakeError("NOT_FOUND")
        return row

    def _live(self, row: sqlite3.Row) -> None:
        if row["state"] == "expired" or (row["state"] != "attached" and row["expires_ns"] <= self.clock()):
            raise IntakeError("UPLOAD_EXPIRED")

    def put_chunk(self, upload_id: str, number: int, data: bytes, sha256: str) -> None:
        digest = _digest(sha256)
        number = _integer(number, 0, self.limits.max_chunks - 1)
        if not isinstance(data, bytes) or len(data) > self.limits.max_chunk_bytes:
            raise IntakeError("LIMIT_EXCEEDED")
        if hashlib.sha256(data).hexdigest() != digest:
            raise IntakeError("CHECKSUM_MISMATCH")
        with self._guard(upload_id):
            with self._db() as db:
                row = self._upload(db, upload_id)
                self._live(row)
                count = (row["size"] + row["chunk_size"] - 1) // row["chunk_size"]
                if number >= count or len(data) != min(row["chunk_size"], row["size"] - number * row["chunk_size"]):
                    raise IntakeError("CHUNK_LENGTH_MISMATCH")
                previous = db.execute("SELECT digest FROM chunks WHERE transfer_id=? AND number=?", (upload_id, number)).fetchone()
                if previous and previous[0] != digest:
                    raise IntakeError("IDEMPOTENCY_CONFLICT")
                if previous:
                    return
                if row["state"] != "uploading":
                    raise IntakeError("UPLOAD_FROZEN")
            path = self._part(upload_id)
            with path.open("r+b" if path.exists() else "w+b") as target:
                target.seek(number * row["chunk_size"])
                target.write(data)
                target.flush()
                _sync_file(target.fileno())
            _sync_dir(path.parent)
            self._fault("after_chunk_flush")
            with self._db(write=True) as db:
                db.execute("INSERT INTO chunks VALUES (?,?,?)", (upload_id, number, digest))
                received = db.execute("SELECT count(*) FROM chunks WHERE transfer_id=?", (upload_id,)).fetchone()[0]
                if received == count:
                    db.execute("UPDATE transfers SET state='verifying' WHERE id=?", (upload_id,))
                    db.execute("INSERT INTO work VALUES (?,?,?,'queued')", (uuid4().hex, "verify_upload", upload_id))
            self._fault("after_chunk_commit")

    def verify_upload(self, upload_id: str) -> dict:
        """Run durable transfer verification work; never parses transaction bytes."""
        with self._guard(upload_id):
            with self._db() as db:
                row = self._upload(db, upload_id)
                self._live(row)
            if row["state"] in ("ready", "attached"):
                return self.upload_status(upload_id)
            if row["state"] != "verifying":
                raise IntakeError("UPLOAD_NOT_READY", retryable=row["state"] == "uploading")
            path = self._payload(upload_id) if self._payload(upload_id).exists() else self._part(upload_id)
            if not row["size"] and not path.exists():
                with path.open("xb") as target:
                    _sync_file(target.fileno())
            with path.open("rb") as source:
                digest = self._copy(source, row["size"])
            if digest != row["digest"]:
                with self._db(write=True) as db:
                    db.execute("UPDATE transfers SET state='failed' WHERE id=?", (upload_id,))
                    db.execute("UPDATE work SET state='complete' WHERE kind='verify_upload' AND subject_id=?", (upload_id,))
                raise IntakeError("CHECKSUM_MISMATCH")
            self._install(upload_id)
            self._fault("after_upload_install")
            with self._db(write=True) as db:
                db.execute("UPDATE transfers SET state='ready' WHERE id=?", (upload_id,))
                db.execute("UPDATE work SET state='complete' WHERE kind='verify_upload' AND subject_id=?", (upload_id,))
        return self.upload_status(upload_id)

    def receive_upload(self, metadata: Mapping[str, object], key: str, upload_id: str) -> dict:
        metadata = self._metadata(metadata)
        key = _token(key, 128)
        with self._guard(upload_id):
            with self._db(write=True) as db:
                row = self._upload(db, upload_id)
                existing = self._existing(db, metadata, key)
                if existing:
                    if existing["fingerprint"] != self._fingerprint(metadata, row["digest"], row["size"]):
                        raise IntakeError("IDEMPOTENCY_CONFLICT")
                    return json.loads(existing["receipt"])
                self._live(row)
                if row["state"] == "attached":
                    raise IntakeError("UPLOAD_FROZEN")
                if row["state"] != "ready":
                    raise IntakeError("UPLOAD_NOT_READY", retryable=row["state"] in ("uploading", "verifying"))
                receipt = self._attach(db, upload_id, metadata, key, row["digest"], row["size"])
            self._fault("after_receipt_commit")
            return receipt

    def upload_status(self, upload_id: str) -> dict:
        with self._db() as db:
            row = self._upload(db, upload_id)
            numbers = [item[0] for item in db.execute("SELECT number FROM chunks WHERE transfer_id=? ORDER BY number", (upload_id,))]
        ranges: list[list[int]] = []
        for number in numbers:
            if ranges and ranges[-1][1] + 1 == number:
                ranges[-1][1] = number
            else:
                ranges.append([number, number])
        state = row["state"]
        if state != "attached" and row["expires_ns"] <= self.clock():
            state = "expired"
        return {"upload_id": upload_id, "state": state, "expected_bytes": row["size"],
                "sha256": row["digest"], "chunk_bytes": row["chunk_size"],
                "received_ranges": ranges, "expires_at": _timestamp(row["expires_ns"]),
                "error": {"code": "CHECKSUM_MISMATCH", "retryable": False} if state == "failed" else None}

    def _import(self, db: sqlite3.Connection, import_id: str) -> sqlite3.Row:
        row = db.execute("SELECT * FROM imports WHERE id=?", (import_id,)).fetchone()
        if row is None:
            raise IntakeError("NOT_FOUND")
        return row

    def status(self, import_id: str) -> dict:
        with self._db() as db:
            row = self._import(db, import_id)
        return {"import_id": import_id, "revision": "1", "receipt": json.loads(row["receipt"]),
                "metadata": json.loads(row["metadata"]), "validation_context": json.loads(row["context"]),
                "enumeration": {"state": "pending", "records": None},
                "validation_counts": {"pending": 0, "running": 0, "valid": 0, "invalid": 0, "blocked": 0},
                "commitment_record_counts": {"not_committed": 0, "queued": 0, "committed": 0, "conflict": 0},
                "work_health": "queued", "all_records_committed": False, "event_cursor": "1"}

    def copy_payload(self, import_id: str, target: BinaryIO, *, offset: int = 0, length: int | None = None) -> None:
        with self._db() as db:
            row = self._import(db, import_id)
        size = json.loads(row["receipt"])["payload"]["bytes"]
        _integer(offset, 0, size)
        remaining = size - offset if length is None else _integer(length, 0, size - offset)
        with self._payload(row["transfer_id"]).open("rb") as source:
            source.seek(offset)
            while remaining:
                data = source.read(min(1024 * 1024, remaining))
                if not data:
                    raise IntakeError("PAYLOAD_UNAVAILABLE")
                _write_all(target, data)
                remaining -= len(data)

    def pending_work(self, *, after: str = "", limit: int = 100) -> list[dict]:
        _integer(limit, 1, self.limits.max_page_size)
        with self._db() as db:
            return [dict(row) for row in db.execute("SELECT id,kind,subject_id,state FROM work WHERE state='queued' AND id>? ORDER BY id LIMIT ?", (after, limit))]

    def events(self, import_id: str, *, after: str = "0", limit: int = 100) -> dict:
        _integer(limit, 1, self.limits.max_page_size)
        if not isinstance(after, str) or not re.fullmatch(r"0|[1-9][0-9]{0,17}", after):
            raise IntakeError("INVALID_CURSOR")
        with self._db() as db:
            self._import(db, import_id)
            rows = db.execute("SELECT revision,body FROM events WHERE import_id=? AND revision>? ORDER BY revision LIMIT ?", (import_id, int(after), limit)).fetchall()
        if int(after) > 1:
            raise IntakeError("INVALID_CURSOR")
        return {"items": [json.loads(row["body"]) for row in rows],
                "next_after": str(rows[-1]["revision"]) if rows else after, "high_watermark": "1"}

    def recover(self, *, after: str = "", limit: int = 100) -> dict:
        """Reclaim interrupted direct transfers and expired unattached uploads.

        Active writers hold an OS lock; process death releases it. Payload deletion
        precedes quota release. Attached evidence and its queue intent are retained.
        """
        _integer(limit, 1, self.limits.max_page_size)
        with self._db() as db:
            rows = db.execute("SELECT id,kind,expires_ns FROM transfers WHERE state!='attached' AND reserved=1 AND id>? ORDER BY id LIMIT ?", (after, limit)).fetchall()
        reclaimed = []
        for row in rows:
            if row["kind"] != "direct" and row["expires_ns"] > self.clock():
                continue
            try:
                with self._guard(row["id"], blocking=False):
                    if self._release_unattached(row["id"], delete_row=row["kind"] == "direct"):
                        reclaimed.append(row["id"])
            except BlockingIOError:
                pass
        return {"reclaimed": reclaimed, "next_after": rows[-1]["id"] if len(rows) == limit else None}

    def capabilities(self) -> dict:
        with self._db() as db:
            used = dict(db.execute("SELECT bytes,slots FROM capacity WHERE id=1").fetchone())
        return {"implementation": "durable-intake/1", "tenant_id": self.tenant, "book_id": self.book,
                "limits": asdict(self.limits), "contexts": json.loads(_json(self.contexts)),
                "durability_profile": "local-fsync-" + self.mode + "-v1", "sqlite_version": sqlite3.sqlite_version,
                "usage": used, "retention": "no automatic receipt/payload purge",
                "validation_available": False, "http_available": False}
