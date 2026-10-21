"""Local ingestion and background-processing CLI. No network listener."""

import argparse
import json
from pathlib import Path
import sys

from .store import IntakeError, IntakeStore, Limits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("capabilities")
    work = commands.add_parser("work")
    work.add_argument("--after", default="")
    work.add_argument("--limit", type=int, default=100)
    recover = commands.add_parser("recover")
    recover.add_argument("--after", default="")
    recover.add_argument("--limit", type=int, default=100)
    receive = commands.add_parser("receive")
    receive.add_argument("--metadata", required=True, type=Path)
    receive.add_argument("--key", required=True)
    receive.add_argument("--payload", required=True, type=Path)
    for name in ("status", "events", "payload"):
        command = commands.add_parser(name)
        command.add_argument("import_id")
        if name == "events":
            command.add_argument("--after", default="0")
            command.add_argument("--limit", type=int, default=100)
        if name == "payload":
            command.add_argument("--output", required=True, type=Path)
            command.add_argument("--offset", type=int, default=0)
            command.add_argument("--length", type=int)
    create = commands.add_parser("upload-create")
    create.add_argument("--key", required=True)
    create.add_argument("--bytes", required=True, type=int)
    create.add_argument("--sha256", required=True)
    create.add_argument("--chunk-bytes", required=True, type=int)
    for name in ("upload-status", "upload-verify", "upload-put", "receive-upload"):
        command = commands.add_parser(name)
        command.add_argument("upload_id")
        if name == "upload-put":
            command.add_argument("--index", required=True, type=int)
            command.add_argument("--payload", required=True, type=Path)
            command.add_argument("--sha256", required=True)
        if name == "receive-upload":
            command.add_argument("--metadata", required=True, type=Path)
            command.add_argument("--key", required=True)
    worker = commands.add_parser("worker")
    worker.add_argument("--engine", required=True, type=Path)
    worker.add_argument("--mode", choices=("once", "drain", "watch"), default="once")
    commands.add_parser("ledger-state")
    for name in ("records", "diagnostics", "validation-runs", "commits", "work-attempts"):
        command = commands.add_parser(name)
        command.add_argument("import_id")
        command.add_argument("--limit", type=int, default=100)
        if name == "work-attempts": command.add_argument("--after", type=int, default=0)
        else: command.add_argument("--cursor")
    command = commands.add_parser("record")
    command.add_argument("import_id"); command.add_argument("record_id"); command.add_argument("--run-id")
    command = commands.add_parser("group")
    command.add_argument("import_id"); command.add_argument("group_id")
    for name in ("register-reference", "register-context"):
        command = commands.add_parser(name)
        command.add_argument("id"); command.add_argument("--definition", required=True, type=Path)
    command = commands.add_parser("revalidate")
    command.add_argument("import_id"); command.add_argument("--key", required=True)
    command.add_argument("--request", required=True, type=Path)
    args = parser.parse_args()
    try:
        config = json.loads(args.config.read_text())
        if "limits" in config:
            config["limits"] = Limits(**config["limits"])
        store = IntakeStore(args.root, **config)
        if args.command == "worker":
            from .worker import Worker
            with Worker(store,args.engine) as runner:
                if args.mode == "watch":
                    import signal
                    import threading
                    stop=threading.Event()
                    signal.signal(signal.SIGINT,lambda *_: stop.set())
                    signal.signal(signal.SIGTERM,lambda *_: stop.set())
                    runner.run_forever(stop)
                    result={"state":"stopped"}
                elif args.mode == "drain": result={"jobs":runner.drain()}
                else: result=runner.run_once()
        elif args.command == "ledger-state": result=store.book_state()
        elif args.command in ("records", "diagnostics", "validation-runs", "commits"):
            result=getattr(store,args.command.replace("-","_"))(args.import_id,cursor=args.cursor,limit=args.limit)
        elif args.command == "work-attempts": result=store.work_attempts(args.import_id,after=args.after,limit=args.limit)
        elif args.command == "record": result=store.record(args.import_id,args.record_id,run_id=args.run_id)
        elif args.command == "group": result=store.group(args.import_id,args.group_id)
        elif args.command == "register-reference":
            result=store.register_reference(args.id,**json.loads(args.definition.read_text()))
        elif args.command == "register-context":
            store.register_context(args.id,json.loads(args.definition.read_text())); result={"context_id":args.id}
        elif args.command == "revalidate":
            result=store.revalidate(args.import_id,args.key,**json.loads(args.request.read_text()))
        elif args.command == "receive":
            with args.payload.open("rb") as source:
                result = store.receive(json.loads(args.metadata.read_text()), args.key,
                                       source, args.payload.stat().st_size)
        elif args.command == "receive-upload":
            result = store.receive_upload(json.loads(args.metadata.read_text()), args.key, args.upload_id)
        elif args.command == "status":
            result = store.status(args.import_id)
        elif args.command == "payload":
            with args.output.open("xb") as target:
                store.copy_payload(args.import_id, target, offset=args.offset, length=args.length)
            result = {"output": str(args.output)}
        elif args.command == "events":
            result = store.events(args.import_id, after=args.after, limit=args.limit)
        elif args.command == "upload-create":
            result = store.create_upload(args.key, expected_bytes=args.bytes, sha256=args.sha256,
                                         chunk_bytes=args.chunk_bytes)
        elif args.command == "upload-status":
            result = store.upload_status(args.upload_id)
        elif args.command == "upload-verify":
            result = store.verify_upload(args.upload_id)
        elif args.command == "upload-put":
            with args.payload.open("rb") as source:
                data = source.read(store.limits.max_chunk_bytes + 1)
            store.put_chunk(args.upload_id, args.index, data, args.sha256)
            result = store.upload_status(args.upload_id)
        elif args.command == "recover":
            result = store.recover(after=args.after, limit=args.limit)
        elif args.command == "work":
            result = store.pending_work(after=args.after, limit=args.limit)
        else:
            result = store.capabilities()
        print(json.dumps(result, sort_keys=True))
        return 0
    except IntakeError as error:
        print(json.dumps({"code": error.code, "retryable": error.retryable}), file=sys.stderr)
        return 1
    except (OSError, ValueError, TypeError) as error:
        print(json.dumps({"code": "LOCAL_REQUEST_FAILED", "error_type": type(error).__name__}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
