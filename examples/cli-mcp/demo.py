#!/usr/bin/env python3
"""Run a bounded end-to-end demonstration against a disposable store only."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests/application"))
from support import Peer, metadata, payload, prepare

parser = argparse.ArgumentParser()
parser.add_argument("--build", required=True, type=Path)
args = parser.parse_args()
build = args.build.resolve()
cli, mcp, ingest = [build / path for path in
                   ["libs/application/luca", "libs/application/luca-mcp", "libs/ingestion/luca-ingest"]]
with tempfile.TemporaryDirectory(prefix="luca-foundations-demo-") as temporary:
    profile, binding, admin = prepare(temporary, ingest)
    def command(words, request):
        file = Path(temporary) / "request.json"
        file.write_text(json.dumps(request))
        run = subprocess.run([str(cli), "--profile", str(profile), *words.split(), "--request", str(file)],
                             text=True, capture_output=True, check=True, timeout=30)
        return json.loads(run.stdout)
    request = {"book_id": "fixture-book", "idempotency_key": "disposable-demo-001",
               "metadata": metadata(), "payload_utf8": payload()}
    receipt = command("import submit", request)["data"]
    scope = {"book_id": "fixture-book", "import_id": receipt["import_id"]}
    pending = command("import status", scope)["data"]
    assert pending["enumeration"]["state"] == "pending"
    admin("worker", "--mode", "drain")
    status = command("import status", scope)
    with Peer(mcp, profile) as peer:
        assert peer.call("import_status", scope) == status
        assert peer.call("submit_import", request)["data"] == receipt
        errors = peer.call("read_diagnostics", scope)
        assert errors == command("import errors", scope)
        caps = peer.call("capabilities", {"book_id": "fixture-book"})["data"]
        lots_request = {"book_id": "fixture-book", "accepted_through": "2", "policy_through": "1",
                        "effective_at": "2026-02-01T00:00:00Z", "recorded_through": "2200-01-01T00:00:00Z",
                        "collection": "open_lots", "mapping_digest": caps["mapping_digest"]}
        lots = peer.call("lots", lots_request)
        assert lots == command("lots", lots_request)
        print(json.dumps({"demo": "disposable-only", "receipt": receipt,
                          "before_worker": pending["enumeration"],
                          "after_restart": status["data"],
                          "retained_errors": errors["data"]["items"],
                          "economic_open_lots": lots["data"],
                          "cli_mcp_equivalent": True, "identical_retry_recovered_receipt": True,
                          "temporary_store_removed_on_exit": True}, indent=2))
