from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
import hashlib
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT=Path(__file__).resolve().parents[2]
HOST=ROOT/"apps/ingestion-host"
sys.path.insert(0,str(HOST))
from luca_ingestion import IntakeError,IntakeStore,Limits
from luca_ingestion.worker import Worker,WorkerLimits

ENGINE=os.environ.get("LUCA_INGESTION_ENGINE",str(ROOT/"build/review-debug/tools/ingestion-engine/luca-ingestion-engine"))
CONFIG=json.loads((HOST/"example-config.json").read_text())
META={"source_id":"custodian-a","format":"luca.transactions.jsonl/1","validation_context_id":"cash-equity-v1-ref42"}


def cash(record="r1",economic="e1",amount="100.000000",**extra):
    row={"record_id":record,"economic_event_id":economic,"action":"originate","account_id":"portfolio-a",
         "event":{"type":"cash_movement","effective_at":"2026-10-20T00:00:00Z","currency":"USD","amount":amount}}
    row.update(extra); return row


CHILD=r'''
import json,os,signal,sys
sys.path.insert(0,sys.argv[1])
from luca_ingestion import IntakeStore
from luca_ingestion.worker import Worker,WorkerLimits
store=IntakeStore(sys.argv[2],**json.loads(sys.argv[3]))
def fault(phase):
    if phase==sys.argv[5]:
        print(phase,flush=True)
        os.kill(os.getpid(),signal.SIGKILL)
with Worker(store,sys.argv[4],limits=WorkerLimits(lease_seconds=1),_fault_hook=fault) as worker:
    worker.drain()
'''


@unittest.skipUnless(Path(ENGINE).exists(),"Build luca-ingestion-engine and set LUCA_INGESTION_ENGINE")
class ProcessingTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.root=Path(self.tmp.name)
        self.store=IntakeStore(self.root,**CONFIG)
        self.store.register_reference("ref42",accounts=["portfolio-a","other"],instruments=["ABC"])

    def tearDown(self): self.tmp.cleanup()

    def receive(self,rows,*,key=None,metadata=None,store=None):
        payload=rows if isinstance(rows,bytes) else ("\n".join(json.dumps(x) for x in rows)+"\n").encode()
        return (store or self.store).receive(metadata or META,key or hashlib.sha256(payload).hexdigest(),io.BytesIO(payload),len(payload))["import_id"]

    def drain(self,store=None,**kwargs):
        with Worker(store or self.store,ENGINE,**kwargs) as worker: return worker.drain()

    def error(self,code,operation):
        with self.assertRaises(IntakeError) as e: operation()
        self.assertEqual(e.exception.code,code)

    def test_portable_cash_lifecycle_fixture_and_canonical_replay(self):
        fixture=ROOT/"tests/conformance/staged-ingestion"
        import_id=self.receive((fixture/"cash-lifecycle.jsonl").read_bytes()); self.drain()
        records=self.store.records(import_id)["items"]
        sequences={row["record_id"]:str(row["sequence"]) for row in self.store.ledger_records()}
        expected=(fixture/"expected.tsv").read_text().splitlines()[1:]
        actual=[f"{r['ordinal']}\t{r['validation']['state']}\t{r['commitment']['state']}\t{sequences.get(r['canonical_record_id'],'-')}" for r in records]
        self.assertEqual(actual,expected)
        with Worker(self.store,ENGINE) as worker:
            with self.store._db() as db: worker._engine().sync(db)
            self.assertEqual(worker.engine.sequence,4)
        self.assertEqual(len(self.store.commits(import_id)["items"]),2)

    def test_revalidation_fences_an_inflight_prepared_admission(self):
        import_id=self.receive([cash()]); context=dict(CONFIG["contexts"][META["validation_context_id"]])
        self.store.register_context("same-semantics-new-id",context)
        once=[True]
        def fault(phase):
            if phase=="after_admission_prepare" and once[0]:
                once[0]=False
                record=self.store.records(import_id)["items"][0]; group=self.store.group(import_id,record["group_id"])
                self.store.revalidate(import_id,"context-change",validation_context_id="same-semantics-new-id",
                  groups=[{"group_id":group["id"],"expected_revision":str(group["revision"])}])
        self.drain(_fault_hook=fault)
        self.assertTrue(self.store.status(import_id)["all_records_committed"])
        self.assertEqual(self.store.book_state()["last_sequence"],1)
        record=self.store.records(import_id)["items"][0]
        self.assertEqual(record["validation"]["context_id"],"same-semantics-new-id")
        self.assertEqual(len(self.store.commits(import_id)["items"]),1)

    def test_cli_worker_and_retained_reads(self):
        import_id=self.receive([cash()]); config=self.root/"config.json"; config.write_text(json.dumps(CONFIG))
        def command(*args):
            env=dict(os.environ,PYTHONPATH=str(HOST))
            result=subprocess.run([sys.executable,"-B","-m","luca_ingestion","--root",str(self.root),"--config",str(config),*args],env=env,capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stderr); return json.loads(result.stdout)
        self.assertEqual(command("status",import_id)["enumeration"]["state"],"pending")
        self.assertEqual(command("worker","--engine",ENGINE,"--mode","drain")["jobs"],3)
        self.assertEqual(command("records",import_id)["items"][0]["commitment"]["state"],"committed")
        self.assertEqual(command("ledger-state")["last_sequence"],1)
        self.assertEqual(len(command("commits",import_id)["items"]),1)

    def test_background_validation_does_not_block_receipt(self):
        import_id=self.receive([cash()])
        self.assertEqual(self.store.status(import_id)["enumeration"]["state"],"pending")
        stop=threading.Event()
        with Worker(self.store,ENGINE) as worker:
            thread=threading.Thread(target=worker.run_forever,args=(stop,0.01)); thread.start()
            try:
                import time
                deadline=time.monotonic()+10
                while not self.store.status(import_id)["all_records_committed"]:
                    self.assertLess(time.monotonic(),deadline); stop.wait(0.01)
            finally: stop.set(); thread.join(10)
        self.assertEqual(self.store.book_state()["last_sequence"],1)
        self.assertEqual(self.store.capabilities()["usage"]["slots"],0)

    def test_mixed_records_retain_errors_and_commit_unrelated_valid_records(self):
        payload=(json.dumps(cash())+"\nnot json\n"+json.dumps(cash("r2","e2",amount=1.5))+"\n"+json.dumps(cash("r3","e3",amount="0.0000001"))+"\n").encode()
        import_id=self.receive(payload); self.drain()
        status=self.store.status(import_id)
        self.assertEqual(status["validation_counts"]["invalid"],3)
        self.assertEqual(status["commitment_record_counts"]["committed"],1)
        self.assertFalse(status["all_records_committed"])
        codes={x["code"] for x in self.store.diagnostics(import_id)["items"]}
        self.assertTrue({"PARSE_ERROR","INVALID_DECIMAL","EXCESS_PRECISION"}<=codes)
        reopened=IntakeStore(self.root,**CONFIG)
        self.assertEqual(reopened.status(import_id),status)
        output=io.BytesIO(); reopened.copy_payload(import_id,output); self.assertEqual(output.getvalue(),payload)
        self.assertTrue(reopened.validation_runs(import_id)["items"])

    def test_whole_import_and_declared_groups_prevent_partial_commit(self):
        for metadata,grouped in ((dict(META,atomicity="whole_import"),False),(META,True)):
            good,bad=cash("a"+str(grouped),"ea"+str(grouped)),cash("b"+str(grouped),"eb"+str(grouped),"invalid")
            if grouped: good["atomic_group"]=bad["atomic_group"]="batch"
            import_id=self.receive([good,bad],metadata=metadata); self.drain()
            records=self.store.records(import_id)["items"]
            self.assertEqual(len({r["group_id"] for r in records}),1)
            self.assertEqual(self.store.commits(import_id)["items"],[])
            self.assertEqual(records[0]["validation"]["state"],"valid")
            self.assertEqual(records[1]["validation"]["state"],"invalid")

    def test_parent_after_child_in_same_import_is_ordered_before_admission(self):
        parent=cash("parent","economics")
        correction=cash("correction","economics",amount="80",action="correct",causal_record_id="parent")
        import_id=self.receive([correction,parent]); self.drain()
        self.assertTrue(self.store.status(import_id)["all_records_committed"])
        records=self.store.records(import_id)["items"]
        log=self.store.ledger_records()
        self.assertEqual([r["record_id"] for r in log],[records[1]["canonical_record_id"],records[0]["canonical_record_id"]])
        self.assertEqual(len(self.store.commits(import_id)["items"]),1)

    def test_missing_predecessor_wakes_after_another_import_commits(self):
        child=self.receive([cash("correction","economics",amount="80",action="correct",causal_record_id="parent")])
        self.drain()
        self.assertEqual(self.store.status(child)["validation_counts"]["blocked"],1)
        self.assertEqual(self.store.capabilities()["usage"]["slots"],0)
        parent=self.receive([cash("parent","economics")]); self.drain()
        self.assertTrue(self.store.status(parent)["all_records_committed"])
        self.assertTrue(self.store.status(child)["all_records_committed"])
        self.assertGreaterEqual(len(self.store.validation_runs(child)["items"]),2)
        self.assertIn("MISSING_PREDECESSOR",{d["code"] for d in self.store.diagnostics(child)["items"]})

    def test_missing_snapshot_wakes_when_registered(self):
        config=json.loads(json.dumps(CONFIG)); config["book_id"]="missing-ref"
        store=IntakeStore(self.root,**config)
        import_id=self.receive([cash()],store=store); self.drain(store)
        self.assertEqual(store.status(import_id)["validation_counts"]["blocked"],1)
        store.register_reference("ref42",accounts=["portfolio-a"],instruments=[]); self.drain(store)
        self.assertTrue(store.status(import_id)["all_records_committed"])

    def test_reference_versions_revalidate_without_losing_earlier_outcomes(self):
        import_id=self.receive([cash(account_id="new-account")]); self.drain()
        record=self.store.records(import_id)["items"][0]; old_run=record["validation"]["run_id"]
        self.assertEqual(record["validation"]["state"],"blocked")
        context=dict(CONFIG["contexts"][META["validation_context_id"]],reference_snapshot="ref43")
        self.store.register_context("context43",context)
        self.store.register_reference("ref43",accounts=["new-account"],instruments=[])
        group=self.store.group(import_id,record["group_id"])
        request={"validation_context_id":"context43","groups":[{"group_id":group["id"],"expected_revision":str(group["revision"])}]}
        result=self.store.revalidate(import_id,"retry",**request)
        self.assertEqual(self.store.revalidate(import_id,"retry",**request),result)
        self.drain()
        self.assertTrue(self.store.status(import_id)["all_records_committed"])
        self.assertEqual(self.store.record(import_id,record["record_id"],run_id=old_run)["historical_validation"]["state"],"blocked")
        self.assertIn("UNKNOWN_ACCOUNT",{d["code"] for d in self.store.diagnostics(import_id)["items"]})
        self.error("STALE_SELECTION",lambda:self.store.revalidate(import_id,"new-request",**request))
        reopened=IntakeStore(self.root,**CONFIG)
        self.assertIn("context43",reopened.contexts)
        self.error("REFERENCE_CONFLICT",lambda:self.store.register_reference("ref43",accounts=["different"],instruments=[]))

    def test_causal_cycles_duplicates_and_inexact_reversals_are_not_accepted(self):
        a=cash("a","a-econ",action="reverse",causal_record_id="b")
        b=cash("b","b-econ",action="reverse",causal_record_id="a")
        cyc=self.receive([a,b]); self.drain()
        self.assertIn("CAUSAL_CYCLE",{d["code"] for d in self.store.diagnostics(cyc)["items"]})
        dup=self.receive([cash("d1","d-econ"),cash("d2","d-econ")]); self.drain()
        self.assertEqual(self.store.commits(dup)["items"],[])
        parent=self.receive([cash("parent","parent-econ")]); self.drain()
        rev=self.receive([cash("rev","rev-econ","-99",action="reverse",causal_record_id="parent")]); self.drain()
        self.assertEqual(self.store.commits(rev)["items"],[])
        self.assertTrue(self.store.status(parent)["all_records_committed"])
        self.assertEqual(self.store.book_state()["last_sequence"],1)

    def test_competing_corrections_commit_once_and_retain_conflict(self):
        self.receive([cash("parent","econ")]); self.drain()
        first=self.receive([cash("c1","econ","90",action="correct",causal_record_id="parent")])
        second=self.receive([cash("c2","econ","80",action="correct",causal_record_id="parent")])
        # Validate both against the same prefix before permitting admission.
        with Worker(self.store,ENGINE) as worker:
            for _ in range(2):
                job=worker._claim(); self.assertEqual(job["kind"],"parse_import"); worker._parse(job)
                with self.store._db(write=True) as db:
                    db.execute("UPDATE work SET ready_ns=? WHERE kind='validate_group'",(2**62,))
            with self.store._db(write=True) as db:
                db.execute("UPDATE work SET ready_ns=0 WHERE kind='validate_group'")
            # Scheduling may select an admission after the first validation; force
            # only validation here, then let ordinary workers race for admission.
            with self.store._db(write=True) as db:
                db.execute("UPDATE work SET ready_ns=? WHERE kind='admit_group'",(2**62,))
            while True:
                job=worker._claim()
                if not job: break
                if job["kind"]=="validate_group": worker._validate(job)
                with self.store._db(write=True) as db: db.execute("UPDATE work SET ready_ns=? WHERE kind='admit_group'",(2**62,))
            with self.store._db(write=True) as db: db.execute("UPDATE work SET ready_ns=0 WHERE kind='admit_group'")
        def run(_): return self.drain()
        with ThreadPoolExecutor(max_workers=2) as pool: list(pool.map(run,range(2)))
        self.assertEqual(self.store.book_state()["last_sequence"],2)
        self.assertEqual(len(self.store.commits(first)["items"])+len(self.store.commits(second)["items"]),1)
        self.assertIn("CONFLICTING_SUCCESSOR",{d["code"] for i in (first,second) for d in self.store.diagnostics(i)["items"]})

    def test_admission_conflict_does_not_change_prior_validation_run(self):
        self.receive([cash("parent","econ")]); self.drain()
        import_id=self.receive([cash("child","econ","90",action="correct",causal_record_id="parent")])
        with Worker(self.store,ENGINE) as worker:
            worker.run_once(); worker.run_once()
            record=self.store.records(import_id)["items"][0]; run_id=record["validation"]["run_id"]
            before=self.store.diagnostics(import_id,run_id=run_id)["items"]
            # Let a different import consume the sole lifecycle successor first.
            with self.store._db(write=True) as db:
                db.execute("UPDATE work SET ready_ns=? WHERE kind='admit_group' AND import_id=?",(2**62,import_id))
            rival=self.receive([cash("rival","econ","80",action="correct",causal_record_id="parent")])
            worker.drain(); self.assertTrue(self.store.status(rival)["all_records_committed"])
            with self.store._db(write=True) as db: db.execute("UPDATE work SET ready_ns=0 WHERE import_id=?",(import_id,))
            worker.drain()
        self.assertEqual(self.store.diagnostics(import_id,run_id=run_id)["items"],before)
        errors=self.store.diagnostics(import_id)["items"]
        self.assertNotEqual(errors[-1]["run_id"],run_id)
        self.assertEqual(self.store.validation_runs(import_id)["items"][-1]["kind"],"admission")
        self.assertEqual(self.store.records(import_id)["items"][0]["commitment"]["state"],"conflict")

    def test_v1_receipts_migrate_without_changing_identity_or_evidence(self):
        import sqlite3
        from luca_ingestion.store import _SCHEMA
        import_id=self.receive([cash()])
        before=self.store.status(import_id)["receipt"]
        # Rebuild precisely the previous release's schema with a receipted import
        # and unique parse intent. No worker has touched the opaque payload yet.
        old_columns={"config":"id,value","capacity":"id,bytes,slots",
          "transfers":"id,kind,state,size,digest,chunk_size,upload_key,request_hash,created_ns,expires_ns,reserved",
          "chunks":"transfer_id,number,digest","imports":"id,source,request_key,fingerprint,metadata,context,receipt,transfer_id",
          "work":"id,kind,subject_id,state","events":"import_id,revision,body"}
        with self.store._db() as db:
            saved={name:[tuple(row) for row in db.execute(f"SELECT {columns} FROM {name}")] for name,columns in old_columns.items()}
        db=sqlite3.connect(self.store.database)
        try:
            tables=[row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")]
            for table in tables: db.execute(f'DROP TABLE "{table}"')
            for statement in _SCHEMA: db.execute(statement)
            db.execute("DELETE FROM capacity")
            for name,rows in saved.items():
                for row in rows: db.execute(f"INSERT INTO {name} VALUES ({','.join('?' for _ in row)})",row)
            db.execute("PRAGMA user_version=1"); db.commit()
        finally: db.close()
        migrated=IntakeStore(self.root,**CONFIG)
        self.assertEqual(migrated.status(import_id)["receipt"],before)
        migrated.register_reference("ref42",accounts=["portfolio-a"],instruments=[])
        self.drain(migrated)
        self.assertTrue(migrated.status(import_id)["all_records_committed"])
        self.assertEqual(migrated.capabilities()["usage"]["slots"],0)

    def test_metadata_calendar_nfc_and_cursor_scope_errors_are_retained(self):
        import_id=self.receive([cash("bad-date","ed",source_event_at="2026-02-30T00:00:00Z"),
                                cash("bad-nfc","en",account_id="e\u0301"),cash("good","eg")])
        self.drain()
        self.assertEqual(self.store.status(import_id)["validation_counts"]["invalid"],2)
        self.assertEqual(self.store.book_state()["last_sequence"],1)
        codes={x["code"] for x in self.store.diagnostics(import_id)["items"]}
        self.assertTrue({"INVALID_TIMESTAMP","INVALID_IDENTIFIER"}<=codes)
        page=self.store.records(import_id,limit=1)
        self.error("INVALID_CURSOR",lambda:self.store.records(import_id,cursor=page["next_cursor"],validation_state="valid"))
        self.error("INVALID_CURSOR",lambda:self.store.records(import_id,cursor="!not-base64!"))

    def test_stale_worker_cannot_publish_after_lease_expiry(self):
        import_id=self.receive([cash()]); now=[self.store.clock()]; self.store.clock=lambda:now[0]
        with Worker(self.store,ENGINE,limits=WorkerLimits(lease_seconds=1)) as old,Worker(self.store,ENGINE) as new:
            job=old._claim(); now[0]+=2*10**9
            replacement=new._claim(); self.assertGreater(replacement["generation"],job["generation"])
            self.error("LEASE_LOST",lambda:old._parse(job))
            new._parse(replacement); new.drain()
        self.assertTrue(self.store.status(import_id)["all_records_committed"])

    def test_worker_failure_is_operational_not_financial_invalidity(self):
        import_id=self.receive([cash()]); now=[self.store.clock()]; self.store.clock=lambda:now[0]
        with Worker(self.store,"/does/not/exist",limits=WorkerLimits(max_attempts=2)) as worker:
            self.assertEqual(worker.run_once()["outcome"],"deferred")
            self.assertEqual(self.store.status(import_id)["validation_counts"]["invalid"],0)
            now[0]+=10*10**9; worker.run_once()
        self.assertEqual(self.store.status(import_id)["work_health"],"attention_required")
        self.assertEqual(self.store.diagnostics(import_id)["items"],[])
        self.assertEqual(len(self.store.work_attempts(import_id)["items"]),2)

    def test_crash_recovery_before_and_after_validation_and_admission(self):
        import time
        phases=("after_parse_batch","before_enumeration_commit","before_validation_publish",
                "before_validation_commit","after_validation_commit","after_admission_prepare",
                "before_admission_commit","after_admission_commit","after_admission_publish")
        for phase in phases:
            with self.subTest(phase=phase),tempfile.TemporaryDirectory() as root:
                store=IntakeStore(root,**CONFIG); store.register_reference("ref42",accounts=["portfolio-a"],instruments=[])
                import_id=self.receive([cash()],store=store)
                process=subprocess.run([sys.executable,"-B","-c",CHILD,str(HOST),root,json.dumps(CONFIG),ENGINE,phase],capture_output=True,text=True,timeout=20)
                self.assertEqual(process.returncode,-signal.SIGKILL,process.stderr)
                self.assertIn(phase,process.stdout)
                # Simulate elapsed lease time without a blocking sleep.
                actual=store.clock; store.clock=lambda:actual()+3*10**9
                self.drain(store)
                self.assertTrue(store.status(import_id)["all_records_committed"])
                self.assertEqual(store.book_state()["last_sequence"],1)
                self.assertEqual(len(store.commits(import_id)["items"]),1)
                with Worker(store,ENGINE) as restored:
                    with store._db() as db: restored._engine().sync(db)
                    self.assertEqual(restored.engine.sequence,1)

    def test_snapshot_pages_remain_stable_while_workers_advance(self):
        import_id=self.receive([cash("a","ea"),cash("b","eb"),cash("c","ec")])
        with Worker(self.store,ENGINE) as worker:
            self.assertEqual(worker.run_once()["kind"],"parse_import")
            page=self.store.records(import_id,limit=1)
            self.assertEqual(page["items"][0]["validation"]["state"],"pending")
            worker.drain()
        old=self.store.records(import_id,limit=1,cursor=page["next_cursor"])
        self.assertEqual(old["items"][0]["validation"]["state"],"pending")
        self.assertEqual(old["items"][0]["commitment"]["state"],"not_committed")
        self.assertEqual(self.store.records(import_id)["items"][1]["commitment"]["state"],"committed")

    def test_import_and_group_limits_leave_retained_diagnostics(self):
        import_id=self.receive([cash("a","ea"),cash("b","eb")])
        self.drain(limits=WorkerLimits(max_records=1))
        self.assertEqual(self.store.status(import_id)["enumeration"]["state"],"failed")
        self.assertIn("IMPORT_LIMIT_EXCEEDED",{d["code"] for d in self.store.diagnostics(import_id)["items"]})
        config=dict(CONFIG,book_id="group-limit")
        limited=IntakeStore(self.root,**config); limited.register_reference("ref42",accounts=["portfolio-a"],instruments=[])
        other=self.receive([cash("c","ec"),cash("d","ed")],store=limited,metadata=dict(META,atomicity="whole_import"))
        self.drain(limited,limits=WorkerLimits(max_group_records=1))
        self.assertEqual(limited.commits(other)["items"],[])
        self.assertIn("GROUP_LIMIT_EXCEEDED",{d["code"] for d in limited.diagnostics(other)["items"]})
        self.error("INCOMPATIBLE_WORKER_LIMITS",lambda:Worker(limited,ENGINE))

    def test_empty_import_and_monotone_recorded_time(self):
        empty=self.receive(b""); self.drain(); self.assertTrue(self.store.status(empty)["all_records_committed"])
        first=self.receive([cash()]); self.drain()
        previous=self.store.book_state()["recorded_ns"]
        self.store.clock=lambda:previous-10**9
        second=self.receive([cash("r2","e2")]); self.drain()
        self.assertTrue(self.store.status(second)["all_records_committed"])
        self.assertEqual(self.store.book_state()["recorded_ns"],previous)

    def test_corrupt_canonical_log_prevents_further_admission(self):
        first=self.receive([cash()]); self.drain()
        with self.store._db(write=True) as db: db.execute("UPDATE ledger_records SET canonical=? WHERE sequence=1",(b"corrupt",))
        second=self.receive([cash("r2","e2")]); self.drain()
        self.assertEqual(self.store.status(second)["work_health"],"attention_required")
        self.assertEqual(self.store.book_state()["last_sequence"],1)
        self.assertTrue(self.store.status(first)["all_records_committed"])


if __name__=="__main__": unittest.main()
