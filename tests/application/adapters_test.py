import concurrent.futures
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from support import Peer, config, metadata, payload, prepare, trade

CLI, MCP, INGEST, CATALOG = map(Path, sys.argv[1:5])
del sys.argv[1:5]

# The catalog's output schemas use this small, closed JSON Schema vocabulary.
# This independent checker makes schema drift fail without adding a runtime dep.
def validate(value, schema):
    if "anyOf" in schema:
        errors = []
        for choice in schema["anyOf"]:
            try:
                validate(value, choice)
                return
            except AssertionError as e:
                errors.append(str(e))
        raise AssertionError(errors)
    if "const" in schema:
        assert value == schema["const"], (value, schema)
    if "enum" in schema:
        assert value in schema["enum"], (value, schema)
    if "type" in schema:
        kinds = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        checks = {"object": isinstance(value, dict), "array": isinstance(value, list),
                  "string": isinstance(value, str), "null": value is None,
                  "integer": type(value) is int, "boolean": type(value) is bool}
        assert any(checks[k] for k in kinds), (value, schema)
    if isinstance(value, dict):
        assert set(schema.get("required", [])) <= set(value), (value, schema)
        for k, v in value.items():
            if k in schema.get("properties", {}):
                validate(v, schema["properties"][k])
            else:
                additional = schema.get("additionalProperties", True)
                assert additional, (k, schema)
                if isinstance(additional, dict):
                    validate(v, additional)
    if isinstance(value, list) and "items" in schema:
        for item in value:
            validate(item, schema["items"])
    if isinstance(value, str):
        import re
        assert len(value) >= schema.get("minLength", 0)
        assert len(value) <= schema.get("maxLength", 2**32)
        if "pattern" in schema:
            assert re.fullmatch(schema["pattern"], value), value
    if type(value) is int:
        assert value >= schema.get("minimum", -2**64)
        assert value <= schema.get("maximum", 2**64)


class Adapters(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="luca-adapters-")
        self.directory = Path(self.temp.name)
        self.profile, self.binding, self.admin = prepare(self.directory, INGEST)
        self.catalog = {t["name"]: t for t in json.loads(CATALOG.read_text())["tools"]}
    def tearDown(self):
        self.temp.cleanup()
    def cli(self, command, args, expected=0, profile=None):
        # Unique files allow parallel CLI processes in concurrency tests.
        import uuid
        path = self.directory / ("request-" + uuid.uuid4().hex + ".json")
        path.write_text(json.dumps(args))
        run = subprocess.run([str(CLI), "--profile", str(profile or self.profile), *command.split(),
                              "--request", str(path)], text=True, capture_output=True, timeout=30)
        self.assertEqual(run.returncode, expected, (run.stdout, run.stderr))
        self.assertEqual(run.stderr, "")
        return json.loads(run.stdout)
    def both(self, peer, name, command, args, expected=0):
        cli = self.cli(command, args, expected)
        mcp = peer.call(name, args)
        # Cursor expiry is stamped at each call. Identity/filter/snapshot/offset
        # must match; a cursor from either adapter is usable by the other.
        left, right = copy.deepcopy(cli), copy.deepcopy(mcp)
        if cli.get("data") and cli["data"].get("next_cursor"):
            lc, rc = cli["data"]["next_cursor"], mcp["data"]["next_cursor"]
            self.assertEqual(json.loads(bytes.fromhex(lc))[:-1], json.loads(bytes.fromhex(rc))[:-1])
            left["data"]["next_cursor"] = right["data"]["next_cursor"] = "clock-dependent-expiry"
        self.assertEqual(left, right)
        validate(mcp, self.catalog["luca_" + name]["outputSchema"])
        return cli
    def submit(self, key="fixture-key", body=None):
        return {"book_id": "fixture-book", "idempotency_key": key,
                "metadata": metadata(), "payload_utf8": payload() if body is None else body}
    def test_receipts_recovery_retained_errors_and_equivalence(self):
        request = self.submit()
        with Peer(MCP, self.profile) as p:
            catalog = p.request("tools/list")["result"]["tools"]
            self.assertEqual(catalog, list(self.catalog.values()))
            receipt = self.both(p, "submit_import", "import submit", request)["data"]
            self.assertTrue(receipt["durable_acceptance"])
            scope = {"book_id": "fixture-book", "import_id": receipt["import_id"]}
            pending = self.both(p, "import_status", "import status", scope)["data"]
            self.assertEqual(pending["enumeration"]["state"], "pending")
            self.assertFalse(pending["all_records_committed"])
            self.assertEqual(self.admin("ledger-state")["last_sequence"], 0)
            # Explicit native worker in a new process recovers pending durable intent.
        self.admin("worker", "--mode", "drain")
        with Peer(MCP, self.profile) as p:
            self.assertEqual(self.both(p, "submit_import", "import submit", request)["data"], receipt)
            status = self.both(p, "import_status", "import status", scope)["data"]
            self.assertEqual(status["validation_counts"]["invalid"], 2)
            self.assertEqual(status["commitment_record_counts"]["committed"], 2)
            for name, command in [("read_records", "import records"), ("read_diagnostics", "import errors")]:
                args = {**scope, "limit": 1}
                first = self.both(p, name, command, args)["data"]
                self.assertEqual(len(first["items"]), 1)
                self.assertIsNotNone(first["next_cursor"])
                second = self.both(p, name, command, {**args, "cursor": first["next_cursor"]})["data"]
                self.assertEqual(first["snapshot_revision"], second["snapshot_revision"])
                self.assertNotEqual(first["items"], second["items"])
            conflict = self.both(p, "submit_import", "import submit", self.submit(body="different"), 4)
            self.assertEqual(conflict["error"]["code"], "IDEMPOTENCY_CONFLICT")
            self.both(p, "accounts", "accounts", {"book_id": "fixture-book"})
            self.both(p, "capabilities", "capabilities", {"book_id": "fixture-book"})
            self.both(p, "books", "books", {})
            self.both(p, "policy_history", "policy history", {"book_id": "fixture-book"})
            policy_args = {"book_id": "fixture-book", "target": self.binding["accounts"][0],
                           "effective_at": "2026-02-01T00:00:00Z", "through_sequence": "1"}
            self.both(p, "resolve_policy", "policy resolve", policy_args)
            self.both(p, "policy_result", "policy result", {"book_id": "fixture-book", "evidence_id": "missing"}, 4)
            records = p.call("read_records", scope)["data"]["items"]
            self.both(p, "read_record", "import record", {**scope, "record_id": records[0]["record_id"]})
            # Existing canonical transaction pages and exact economic lot projections.
            args = {"book_id": "fixture-book", "accepted_through": "2", "limit": 1}
            t = self.both(p, "transactions", "transactions", args)["data"]
            self.assertTrue(t["has_more"])
            self.assertEqual(t["next_after"], "1")
            self.both(p, "transactions", "transactions", {**args, "after": "1"})
            lots = {"book_id": "fixture-book", "accepted_through": "2", "policy_through": "1",
                    "effective_at": "2026-02-01T00:00:00Z", "recorded_through": "2200-01-01T00:00:00Z",
                    "collection": "open_lots", "mapping_digest": p.call("capabilities", {"book_id": "fixture-book"})["data"]["mapping_digest"]}
            r = self.both(p, "lots", "lots", lots)["data"]
            self.assertEqual(r["items"][0]["remaining_quantity"], "7.00000000")
            self.assertFalse(r["recognition_applied"])
            self.assertEqual(r["resolved_policies"][0]["recognition"], "accrual")
            relief = self.both(p, "lots", "lots", {**lots, "collection": "reliefs"})["data"]
            self.assertEqual(relief["items"][0]["net_pnl"]["amount"], "30.000000")
            # Appending cannot change a pinned prefix, including paginated lot results.
            extra = self.submit("later", json.dumps(trade("later", "later-event", "2")) + "\n")
            p.call("submit_import", extra)
            self.admin("worker", "--mode", "drain")
            self.assertEqual(p.call("lots", lots)["data"], r)
    def test_scope_read_only_and_closed_schema(self):
        readonly = copy.deepcopy(self.binding)
        readonly.pop("submit")
        readonly.pop("process")
        readonly.pop("submit_sources")
        profile = self.directory / "readonly.json"
        profile.write_text(json.dumps({"books": [readonly]}))
        with Peer(MCP, profile) as p:
            names = [t["name"] for t in p.request("tools/list")["result"]["tools"]]
            self.assertNotIn("luca_submit_import", names)
            self.assertIn("error", p.request("tools/call", {"name": "luca_submit_import", "arguments": self.submit()}))
            self.assertEqual(self.cli("import submit", self.submit(), 3, profile)["error"]["code"], "PERMISSION_DENIED")
            self.both(p, "import_status", "import status", {"book_id": "other-book", "import_id": "i"}, 3)
            for args in [{"book_id": "fixture-book", "admin": True}, {"book_id": 3}]:
                self.both(p, "accounts", "accounts", args, 2)
            self.assertEqual(self.cli("worker run", {}, 2)["error"]["code"], "INVALID_ARGUMENTS")
        with Peer(MCP, self.profile) as p:
            bad = self.submit();bad["metadata"]["source_id"] = "other-source"
            self.both(p, "submit_import", "import submit", bad, 3)
            for args in [{"book_id": "fixture-book", "accepted_through": -1},
                         {"book_id": "fixture-book", "accepted_through": "18446744073709551615"},
                         {"book_id": "fixture-book", "accepted_through": "00"}]:
                self.both(p, "transactions", "transactions", args, 2)
    def test_process_restart_and_concurrent_access(self):
        running = copy.deepcopy(self.binding);running["process"] = True
        profile = self.directory / "worker.json"
        profile.write_text(json.dumps({"books": [running]}))
        peer = Peer(MCP, self.profile)
        receipt = peer.call("submit_import", self.submit())["data"]
        peer.kill() # acceptance is durable even if the process loses its workers
        scope = {"book_id": "fixture-book", "import_id": receipt["import_id"]}
        with Peer(MCP, profile) as p:
            deadline = time.monotonic() + 15
            while True:
                status = p.call("import_status", scope)["data"]
                if status["enumeration"]["state"] == "complete" and status["validation_counts"]["invalid"] == 2 and status["commitment_record_counts"]["committed"] == 2:
                    break
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.03)
            # One native worker owner and concurrent process receipts/readers.
            def call(i):
                req = self.submit("concurrent", json.dumps(trade("concurrent", "concurrent-event", "1")) + "\n")
                return self.cli("import submit", req)["data"]["import_id"]
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                identities = list(pool.map(call, range(8)))
            self.assertEqual(len(set(identities)), 1)
            for _ in range(3):
                self.both(p, "read_diagnostics", "import errors", scope)
    def test_native_charge_and_diagnostic_schema_bounds(self):
        charged = trade("large-charge", "large-charge-event")
        charged["event"]["charges"] = [{"id": "i"*300, "category": "commission", "payee": "p"*300,
                                        "schedule_version": "v"*300, "amount": "1", "currency": "USD", "state": "reported"}]
        unknown = trade("unknown-field", "unknown-event")
        unknown["x"*5000] = "invalid"
        with Peer(MCP, self.profile) as p:
            receipt = self.both(p, "submit_import", "import submit", self.submit(body=json.dumps(charged)+"\n"+json.dumps(unknown)+"\n"))["data"]
            self.admin("worker", "--mode", "drain")
            scope = {"book_id": "fixture-book", "import_id": receipt["import_id"]}
            errors = self.both(p, "read_diagnostics", "import errors", scope)["data"]["items"]
            self.assertTrue(any(len(e["field_path"]) > 4096 for e in errors))
            tx = self.both(p, "transactions", "transactions", {"book_id": "fixture-book", "accepted_through": "1"})
            self.assertEqual(len(tx["data"]["items"][0]["transaction"]["event"]["charges"][0]["id"]), 300)
            lots = {"book_id": "fixture-book", "accepted_through": "1", "policy_through": "1",
                    "effective_at": "2026-02-01T00:00:00Z", "recorded_through": "2200-01-01T00:00:00Z",
                    "collection": "trades", "mapping_digest": p.call("capabilities", {"book_id": "fixture-book"})["data"]["mapping_digest"]}
            for collection in ["trades", "open_lots", "reliefs"]:
                self.both(p, "lots", "lots", {**lots, "collection": collection})

    def test_configured_page_limits_and_error_categories(self):
        small = self.directory / "small"
        small.mkdir()
        profile, binding, admin = prepare(small, INGEST, page_size=1)
        with Peer(MCP, profile) as p:
            request = self.submit()
            receipt = p.call("submit_import", request)["data"]
            admin("worker", "--mode", "drain")
            scope = {"book_id": "fixture-book", "import_id": receipt["import_id"]}
            for name in ["read_records", "read_diagnostics"]:
                self.assertEqual(len(p.call(name, scope)["data"]["items"]), 1)
            caps = p.call("capabilities", {"book_id": "fixture-book"})["data"]
            self.assertEqual(caps["max_adapter_page_size"], 1)
            lots = {"book_id": "fixture-book", "accepted_through": "2", "policy_through": "1",
                    "effective_at": "2026-02-01T00:00:00Z", "recorded_through": "2200-01-01T00:00:00Z",
                    "collection": "trades", "mapping_digest": caps["mapping_digest"], "limit": 1}
            first = p.call("lots", lots)
            self.assertTrue(first["ok"], first)
            self.assertEqual(first["data"]["next_after"], "1")
            self.assertTrue(p.call("lots", {**lots, "after": "1"})["ok"])
            self.assertEqual(p.call("lots", {**lots, "mapping_digest": "0"*64})["error"]["exit_code"], 4)
            self.assertEqual(p.call("transactions", {"book_id": "fixture-book", "accepted_through": "2", "limit": 2})["error"]["exit_code"], 2)
        with Peer(MCP, self.profile) as p:
            policy = {"book_id": "fixture-book", "target": self.binding["accounts"][0],
                      "effective_at": "2026-02-30T00:00:00Z", "through_sequence": "1"}
            self.both(p, "resolve_policy", "policy resolve", policy, 2)
            self.both(p, "resolve_policy", "policy resolve", {**policy, "effective_at": "2026-02-01T00:00:00Z", "through_sequence": "2"}, 4)
            self.both(p, "submit_import", "import submit", {**self.submit(), "idempotency_key": "x"*129}, 2)

    def test_input_schema_vocabulary(self):
        allowed = {"type", "properties", "required", "additionalProperties", "enum", "minimum", "maximum", "minLength", "maxLength", "pattern", "description"}
        def walk(schema):
            self.assertTrue(set(schema) <= allowed, set(schema) - allowed)
            self.assertIn(schema.get("type"), ["object", "string", "integer", "boolean"])
            if schema.get("type") == "object":
                self.assertIs(schema.get("additionalProperties"), False)
                for prop in schema.get("properties", {}).values():
                    walk(prop)
        for tool in self.catalog.values():
            walk(tool["inputSchema"])

    def test_protocol_and_help_without_storage(self):
        for binary in [CLI, MCP]:
            for flag in ["--help", "--version"]:
                r = subprocess.run([str(binary), flag], text=True, capture_output=True, timeout=10)
                self.assertEqual(r.returncode, 0)
        with Peer(MCP, self.profile) as p:
            self.assertIn("result", p.request("ping"))
            self.assertTrue(p.request("tools/call", {"name": "luca_books"})["result"]["structuredContent"]["ok"])
            self.assertEqual(p.request("no-such-method")["error"]["code"], -32601)
            p.proc.stdin.write("not json\n");p.proc.stdin.flush()
            self.assertEqual(p.receive()["error"]["code"], -32700)
            p.proc.stdin.write("[]\n");p.proc.stdin.flush()
            self.assertEqual(p.receive()["error"]["code"], -32600)
            self.assertIn("result", p.request("ping"))

if __name__ == "__main__":
    unittest.main()
