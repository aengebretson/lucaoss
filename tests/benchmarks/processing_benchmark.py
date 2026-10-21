"""Correctness-checked local receipt/validation/admission baseline; no network/report timing."""
import argparse
from collections import Counter,defaultdict
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
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

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"apps/ingestion-host"))
from luca_ingestion import IntakeStore
from luca_ingestion.worker import Worker
from luca_ingestion.transactions import parse_record
CONFIG=json.loads((ROOT/"apps/ingestion-host/example-config.json").read_text())
META={"source_id":"custodian-a","format":"luca.transactions.jsonl/1","validation_context_id":"cash-equity-v1-ref42"}


def payload(count,prefix,group_size,invalid_every=0):
    rows=[]
    for index in range(count):
        row={"record_id":f"{prefix}-r{index}","economic_event_id":f"{prefix}-e{index}","action":"originate","account_id":"portfolio-a",
             "event":{"type":"cash_movement","effective_at":"2026-10-20T00:00:00Z","currency":"USD",
                      "amount":"invalid" if invalid_every and index%invalid_every==0 else "1.000000"}}
        if group_size>1: row["atomic_group"]=f"g{index//group_size}"
        rows.append(json.dumps(row,separators=(",",":")))
    return ("\n".join(rows)+"\n").encode()


def store_at(root,book):
    store=IntakeStore(root,**dict(CONFIG,book_id=book))
    store.register_reference("ref42",accounts=["portfolio-a"],instruments=[])
    return store


def receive(store,data,key):
    start=time.perf_counter(); receipt=store.receive(META,key,io.BytesIO(data),len(data))
    elapsed=time.perf_counter()-start
    assert receipt["payload"]["digest"]==hashlib.sha256(data).hexdigest()
    assert store.status(receipt["import_id"])["enumeration"]["state"]=="pending"
    return receipt["import_id"],elapsed


def drain(worker):
    durations=defaultdict(float); counts=Counter()
    while True:
        start=time.perf_counter(); result=worker.run_once(); duration=time.perf_counter()-start
        if result is None: break
        if result["outcome"]!="processed": raise AssertionError(result)
        durations[result["kind"]]+=duration; counts[result["kind"]]+=1
    return {"job_seconds":dict(durations),"jobs":dict(counts)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine",type=Path,required=True)
    parser.add_argument("--records",type=int,nargs="+",default=[1000,10000,100000])
    parser.add_argument("--group-size",type=int,default=256)
    parser.add_argument("--repetitions",type=int,default=3)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    if any(not 1<=n<=100000 for n in args.records) or not 1<=args.group_size<=1000 or not 1<=args.repetitions<=10: parser.error("bounded inputs required")
    result={"schema":"luca.processing-benchmark/1","python":sys.version,"sqlite":sqlite3.sqlite_version,
            "platform":platform.platform(),"logical_cpus":os.cpu_count(),"engine_sha256":hashlib.sha256(args.engine.read_bytes()).hexdigest(),
            "scope":"Local durable receipt then single-worker parsing/native exact validation/atomic ledger admission; cached input, fresh stores. Declared atomic groups are workload semantics. No HTTP, report, GPU, production SLO or Geneva comparison.",
            "samples":[],"concurrency":[]}
    with tempfile.TemporaryDirectory(prefix="luca-processing-bench-") as temp:
        root=Path(temp)
        for count in args.records:
            for repetition in range(args.repetitions):
                store=store_at(root,f"book-{count}-{repetition}"); data=payload(count,"initial",args.group_size)
                import_id,receipt_seconds=receive(store,data,"initial")
                start=time.perf_counter()
                with Worker(store,args.engine) as worker:
                    stages=drain(worker); processing_seconds=time.perf_counter()-start
                    assert store.status(import_id)["all_records_committed"] and store.book_state()["last_sequence"]==count
                    # Warm prepared suffix: unchanged growing prefix, explicit discard,
                    # one new candidate; transport/canonicalization included, persistence excluded.
                    candidate=json.loads(payload(1,"suffix",1)); context=CONFIG["contexts"][META["validation_context_id"]]
                    packet=parse_record(store,"probe",META,context,1,json.dumps(candidate).encode(),{})["packet"]
                    with store._db() as db: state=worker._engine().sync(db)
                    probes=[]
                    for _ in range(50):
                        at=time.perf_counter(); accepted,error=worker.engine.prepare([packet],state["recorded_ns"]); worker.engine.discard()
                        assert error is None and accepted[0][0]==count+1; probes.append(time.perf_counter()-at)
                    # Measure an actual incremental append with the same persistent worker.
                    extra=payload(256,"incremental",256); next_id,incremental_receipt=receive(store,extra,"incremental")
                    at=time.perf_counter(); incremental=drain(worker); incremental_seconds=time.perf_counter()-at
                    assert store.status(next_id)["all_records_committed"] and store.book_state()["last_sequence"]==count+256
                result["samples"].append({"records":count,"group_size":args.group_size,"repetition":repetition,"bytes":len(data),
                  "profile":store.capabilities()["durability_profile"],"receipt_seconds":receipt_seconds,"processing_seconds":processing_seconds,
                  "records_per_processing_second":count/processing_seconds,**stages,
                  "warm_prepare_suffix_median_seconds":statistics.median(probes),"incremental_records":256,
                  "incremental_receipt_seconds":incremental_receipt,"incremental_processing_seconds":incremental_seconds, "incremental":incremental})
        # Same-book serialization versus separate-book worker partitions. Invalid
        # singleton rows remain inspectable; unrelated valid rows still commit.
        for separate in (False,True):
            for workers in (1,2,4):
                stores=[store_at(root,f"parallel-{separate}-{workers}-{i if separate else 0}") for i in range(workers)]
                imports=[]
                for i in range(4):
                    store=stores[i%workers]; import_id,_=receive(store,payload(100,f"i{i}",1,20),f"i{i}"); imports.append((store,import_id))
                def run(index):
                    with Worker(stores[index],args.engine) as worker: return drain(worker)
                at=time.perf_counter()
                with ThreadPoolExecutor(max_workers=workers) as pool: runs=list(pool.map(run,range(workers)))
                elapsed=time.perf_counter()-at
                for store,import_id in imports:
                    status=store.status(import_id)
                    assert status["validation_counts"]["invalid"]==5 and status["commitment_record_counts"]["committed"]==95
                result["concurrency"].append({"workers":workers,"separate_books":separate,"records":400,"invalid_records":20,"wall_seconds":elapsed,"runs":runs})
    result["peak_python_rss_platform_units"]=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    result["peak_child_rss_platform_units"]=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    result["source_sha256"]={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
        [Path(__file__),*sorted((ROOT/"apps/ingestion-host/luca_ingestion").glob("*.py"))]}
    args.output.parent.mkdir(parents=True,exist_ok=True); args.output.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({"output":str(args.output),"samples":len(result["samples"])}))

if __name__=="__main__": main()
