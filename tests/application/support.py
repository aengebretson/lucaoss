"""Disposable fixtures and a real stdio MCP peer; no network or production data."""
import json
import os
from pathlib import Path
import select
import subprocess


def config():
    return {"tenant_id": "fixture-tenant", "book_id": "fixture-book",
            "source_ids": ["fixture-source"], "contexts": {"v2": {
                "adapter": "luca.transactions.jsonl/1", "normalization": "source-book-identity/1",
                "rules": "cash-equity/2", "engine": "luca/0.1", "reference_snapshot": "fixture-ref"}}}


def metadata():
    return {"source_id": "fixture-source", "format": "luca.transactions.jsonl/1", "validation_context_id": "v2"}


def trade(record="buy", economic="buy-event", quantity="10", price="100"):
    return {"record_id": record, "economic_event_id": economic, "action": "originate",
            "account_id": "fixture-account", "event": {"type": "equity_trade",
            "effective_at": "2026-01-10T00:00:00Z", "settlement_date": "2026-01-12",
            "quote_currency": "USD", "instrument_id": "ABC", "quantity": quantity,
            "price": price, "charges": []}}


def payload():
    return "\n".join(json.dumps(r) for r in [trade(), trade("sell", "sell-event", "-3", "110"),
        trade("bad", "bad-event", "0.000000001")]) + "\nnot json\n"


def prepare(directory, ingest, page_size=None):
    directory = Path(directory)
    cfg = config()
    if page_size is not None:
        cfg["limits"] = {"max_page_size": page_size}
    cfg_file = directory / "store-config.json"
    cfg_file.write_text(json.dumps(cfg))
    root = directory / "store"
    def admin(command, *args, request=None):
        argv = [str(ingest), "--root", str(root), "--config", str(cfg_file), command, *args]
        if request is not None:
            p = directory / "admin-request.json"
            p.write_text(json.dumps(request))
            argv += ["--request", str(p)]
        run = subprocess.run(argv, text=True, capture_output=True, timeout=30)
        if run.returncode:
            raise AssertionError((argv, run.stdout, run.stderr))
        return json.loads(run.stdout)
    admin("register-reference", "fixture-ref", request={"accounts": ["fixture-account"], "instruments": ["ABC"]})
    revision = {"id": "fixture-policy", "scope": "legal_entity", "subject": "fixture-entity",
                "legal_entity": "fixture-entity", "effective_from": "2026-01-01T00:00:00Z",
                "lot_selection": "LIFO", "recognition": "accrual"}
    admin("policy-put", request={"revision": revision, "expected_sequence": 0, "actor": "fixture-admin"})
    target = {"legal_entity": "fixture-entity", "custodian_account": "fixture-custody", "account": "fixture-account"}
    binding = {"root": str(root), "config": cfg, "mapping_version": "fixture-mapping/1",
               "accounts": [target], "submit": True, "process": False, "submit_sources": ["fixture-source"]}
    profile = directory / "profile.json"
    profile.write_text(json.dumps({"books": [binding]}))
    return profile, binding, admin


class Peer:
    def __init__(self, binary, profile):
        self.proc = subprocess.Popen([str(binary), "--profile", str(profile)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.counter = 0
        self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                    "clientInfo": {"name": "luca-foundations-test", "version": "1"}})
        self.notify("notifications/initialized")
    def receive(self):
        ready, _, _ = select.select([self.proc.stdout], [], [], 30)
        if not ready:
            raise AssertionError("MCP response timeout")
        line = self.proc.stdout.readline()
        if not line:
            raise AssertionError("MCP exited: " + self.proc.stderr.read())
        return json.loads(line)
    def request(self, method, params=None):
        self.counter += 1
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.counter,
                              "method": method, "params": params or {}}) + "\n")
        self.proc.stdin.flush()
        response = self.receive()
        assert response["id"] == self.counter, response
        return response
    def notify(self, method):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()
    def call(self, name, arguments):
        response = self.request("tools/call", {"name": "luca_" + name, "arguments": arguments})
        result = response["result"]
        assert json.loads(result["content"][0]["text"]) == result["structuredContent"]
        assert result["isError"] == (not result["structuredContent"]["ok"])
        return result["structuredContent"]
    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            self.proc.wait(timeout=30)
        error = self.proc.stderr.read()
        self.proc.stdout.close()
        self.proc.stderr.close()
        assert self.proc.returncode == 0, error
    def kill(self):
        self.proc.kill()
        self.proc.communicate(timeout=10)
    def __enter__(self):
        return self
    def __exit__(self, *exc):
        self.close()
