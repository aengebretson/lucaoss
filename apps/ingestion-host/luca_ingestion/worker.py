"""Lease-fenced background validation and atomic durable ledger admission."""
from collections import Counter
from contextlib import contextmanager
from dataclasses import dataclass
import fcntl
import hashlib
import json
import os
import threading
from uuid import uuid4
from urllib.parse import quote

from .engine import Engine
from .store import IntakeError, _framed_hash, _json, _timestamp
from .transactions import causal_order, groups_for, issue, parse_record


@dataclass(frozen=True)
class WorkerLimits:
    batch_records: int = 256
    max_records: int = 100000
    max_record_bytes: int = 65536
    max_group_records: int = 1000
    max_ledger_records: int = 1000000
    lease_seconds: int = 60
    max_attempts: int = 3


class Worker:
    def __init__(self, store, executable, *, limits=WorkerLimits(), _fault_hook=None):
        if not 1<=limits.batch_records<=1000 or not 1<=limits.max_group_records<=1000 or any(
            type(value) is not int or value<1 for value in vars(limits).values()):
            raise IntakeError("INVALID_WORKER_LIMITS")
        self.store=store; self.executable=executable; self.limits=limits
        resource_limits={key:getattr(limits,key) for key in ("max_records","max_record_bytes","max_group_records","max_ledger_records")}
        with store._db(write=True) as db:
            prior=db.execute("SELECT body FROM processing_config WHERE id=1").fetchone()
            if prior and json.loads(prior[0])!=resource_limits: raise IntakeError("INCOMPATIBLE_WORKER_LIMITS")
            db.execute("INSERT OR IGNORE INTO processing_config VALUES (1,?)",(_json(resource_limits),))
        self.owner=uuid4().hex; self.engine=None; self.lock=threading.Lock()
        self.fault=_fault_hook or (lambda phase: None)

    def __enter__(self): return self
    def __exit__(self,*args): self.close()
    def close(self):
        if self.engine: self.engine.close(); self.engine=None
    def _engine(self):
        if self.engine is None: self.engine=Engine(self.executable)
        return self.engine

    def _claim(self):
        now=self.store.clock()
        with self.store._db(write=True) as db:
            while True:
                row=db.execute("""SELECT * FROM work WHERE kind IN ('parse_import','validate_group','admit_group')
                  AND ((state IN ('queued','retry_wait') AND ready_ns<=?) OR (state='running' AND lease_until<=?))
                  ORDER BY ready_ns,id LIMIT 1""",(now,now)).fetchone()
                if row is None: return None
                if row["attempts"]>=self.limits.max_attempts:
                    db.execute("UPDATE work SET state='attention_required',owner=NULL WHERE id=?",(row["id"],))
                    self.store._event(db,row["import_id"],"work.attention_required",[row["id"]])
                    self.store._idle_release(db,row["import_id"]); continue
                if not self.store._reserve_pipeline(db,row["import_id"]): return None
                db.execute("UPDATE work SET state='running',owner=?,generation=generation+1,attempts=attempts+1,lease_until=? WHERE id=?",
                           (self.owner,now+self.limits.lease_seconds*10**9,row["id"]))
                job=dict(db.execute("SELECT * FROM work WHERE id=?",(row["id"],)).fetchone())
                self.store._event(db,job["import_id"],"work.started",[job["id"]])
                return job

    def _lease(self, db, job):
        row=db.execute("SELECT * FROM work WHERE id=?",(job["id"],)).fetchone()
        if row["state"]!="running" or row["owner"]!=self.owner or row["generation"]!=job["generation"] or row["lease_until"]<=self.store.clock():
            raise IntakeError("LEASE_LOST",retryable=True)

    def _renew(self, job):
        with self.store._db(write=True) as db:
            self._lease(db,job)
            db.execute("UPDATE work SET lease_until=? WHERE id=?",(self.store.clock()+self.limits.lease_seconds*10**9,job["id"]))

    def _complete(self, db, job):
        self._lease(db,job)
        db.execute("UPDATE work SET state='complete',owner=NULL,lease_until=0 WHERE id=?",(job["id"],))
        self.store._idle_release(db,job["import_id"])

    def _failure(self, job, error):
        with self.store._db(write=True) as db:
            try: self._lease(db,job)
            except IntakeError: return
            code=error.code if isinstance(error,IntakeError) else type(error).__name__
            state="attention_required" if job["attempts"]>=self.limits.max_attempts or code in ("LEDGER_CORRUPT","PAYLOAD_CORRUPT") else "retry_wait"
            body={"work_id":job["id"],"generation":job["generation"],"attempt":job["attempts"],"code":code,"at":_timestamp(self.store.clock())}
            db.execute("INSERT INTO attempts(work_id,body) VALUES (?,?)",(job["id"],_json(body)))
            db.execute("UPDATE work SET state=?,owner=NULL,lease_until=0,ready_ns=?,last_error=? WHERE id=?",
                       (state,self.store.clock()+min(60,2**job["attempts"])*10**9+uuid4().int%10**9,_json(body),job["id"]))
            self.store._event(db,job["import_id"],"work."+state,[job["id"]])
            self.store._idle_release(db,job["import_id"])

    def run_once(self):
        with self.lock:
            self.store.wake_dependencies()
            job=self._claim()
            if not job: return None
            try:
                self.fault("after_claim")
                {"parse_import":self._parse,"validate_group":self._validate,"admit_group":self._admit}[job["kind"]](job)
                return {"work_id":job["id"],"kind":job["kind"],"outcome":"processed"}
            except Exception as error:
                self.close()
                if not isinstance(error,IntakeError) or error.code!="LEASE_LOST": self._failure(job,error)
                return {"work_id":job["id"],"kind":job["kind"],"outcome":"deferred",
                        "code":error.code if isinstance(error,IntakeError) else type(error).__name__}

    def drain(self, max_jobs=100000):
        count=0
        while count<max_jobs and self.run_once() is not None: count+=1
        return count

    def run_forever(self, stop, idle_seconds=0.25):
        while not stop.is_set():
            if self.run_once() is None: stop.wait(idle_seconds)

    @staticmethod
    def _supported(context):
        return context["adapter"]=="luca.transactions.jsonl/1" and context["normalization"]=="source-book-identity/1" and context["rules"]=="cash-equity/1" and context["engine"]=="luca/0.1"

    def _diagnostic(self, db, import_id, diagnostic, *, record=None, group_id=None, run_id=None, classification="invalid"):
        identity=uuid4().hex if run_id else hashlib.sha256(_json([import_id,record["id"] if record else None,diagnostic["code"],diagnostic.get("field_path","")]).encode()).hexdigest()
        data={"diagnostic_id":identity,"scope":"record" if record else "group" if group_id else "import",
              "import_id":import_id,"record_id":record["id"] if record else None,"group_id":group_id,
              "run_id":run_id,"code":diagnostic["code"],"severity":"error",
              "message":diagnostic.get("message","Validation failed."),"field_path":diagnostic.get("field_path",""),
              "locator":json.loads(record["body"])["locator"] if record else None,
              "related_ids":diagnostic.get("related_ids",[]),"classification":classification,
              "retryable":classification=="dependency","details":diagnostic.get("details",{})}
        db.execute("INSERT OR IGNORE INTO diagnostics(id,import_id,group_id,record_id,run_id,code,body) VALUES (?,?,?,?,?,?,?)",
                   (identity,import_id,group_id,data["record_id"],run_id,data["code"],_json(data)))

    def _publish_records(self, job, batch):
        context=batch[0]["context"]
        packets=[data["packet"] for data in batch if data["packet"]]
        if packets and self._supported(context):
            results=iter(self._engine().validate(packets))
            for data in batch:
                if data["packet"]:
                    error=next(results)
                    if error: data["issues"].append(error)
        with self.store._db(write=True) as db:
            self._lease(db,job)
            revision=self.store._event(db,job["import_id"],"import.enumerating")
            new=[]; vc=Counter(); cc=Counter()
            for data in batch:
                data["issues"].sort(key=lambda x:(x.get("field_path",""),x["code"]))
                record_id=job["import_id"]+":"+str(data["ordinal"])
                encoded=_json(data)
                old=db.execute("SELECT body FROM staged_records WHERE id=?",(record_id,)).fetchone()
                if old:
                    if old[0]!=encoded: raise IntakeError("PAYLOAD_CORRUPT")
                    continue
                state="invalid" if data["issues"] else "pending"
                db.execute("INSERT INTO staged_records(id,import_id,ordinal,body,validation) VALUES (?,?,?,?,?)",
                           (record_id,job["import_id"],data["ordinal"],encoded,state))
                row=db.execute("SELECT * FROM staged_records WHERE id=?",(record_id,)).fetchone()
                for diagnostic in data["issues"]: self._diagnostic(db,job["import_id"],diagnostic,record=row)
                new.append(record_id); vc[state]+=1; cc["not_committed"]+=1
            self.store._counts(db,job["import_id"],vc,cc)
            self.store._history(db,new,revision)
        self.fault("after_parse_batch")
        self._renew(job)

    def _parse(self, job):
        with self.store._db() as db:
            receipt=self.store._import(db,job["import_id"])
            metadata=json.loads(receipt["metadata"]); context=json.loads(receipt["context"])
            payload=self.store._payload(receipt["transfer_id"])
        batch=[]; ordinal=0; physical_line=0; digest=hashlib.sha256(); failure=None
        with payload.open("rb") as source:
            while raw:=source.readline(self.limits.max_record_bytes+1):
                offset=source.tell()-len(raw); physical_line+=1; raw_hash=hashlib.sha256(raw); digest.update(raw); length=len(raw)
                too_long=len(raw)>self.limits.max_record_bytes
                if too_long and not raw.endswith(b"\n"):
                    while rest:=source.readline(self.limits.max_record_bytes+1):
                        raw_hash.update(rest); digest.update(rest); length+=len(rest)
                        if rest.endswith(b"\n"): break
                if not raw.strip() and not too_long: continue
                ordinal+=1
                if ordinal>self.limits.max_records:
                    failure=issue("IMPORT_LIMIT_EXCEEDED"); break
                locator={"byte_offset":offset,"byte_length":length,"line":physical_line,"sha256":raw_hash.hexdigest()}
                data=parse_record(self.store,job["import_id"],metadata,context,ordinal,raw if not too_long else b"",locator)
                if too_long: data["issues"]=[issue("RECORD_LIMIT_EXCEEDED")]
                data.update(ordinal=ordinal,context=context)
                batch.append(data)
                if len(batch)==self.limits.batch_records: self._publish_records(job,batch); batch=[]
        if batch: self._publish_records(job,batch)
        if not failure and digest.hexdigest()!=json.loads(receipt["receipt"])["payload"]["digest"]:
            raise IntakeError("PAYLOAD_CORRUPT")
        with self.store._db(write=True) as db:
            self._lease(db,job)
            records=[dict(row) for row in db.execute("SELECT * FROM staged_records WHERE import_id=? ORDER BY ordinal",(job["import_id"],))]
            data=[dict(json.loads(row["body"]),id=row["id"]) for row in records]
            if any(r.get("membership_unknown") for r in data) or (any(r.get("atomic_group") for r in data) and any((r.get("raw") is None) for r in data)):
                failure=issue("GROUP_MEMBERSHIP_UNKNOWN")
            revision=self.store._event(db,job["import_id"],"import.enumerated")
            if failure:
                self._diagnostic(db,job["import_id"],failure)
                db.execute("UPDATE progress SET enumeration='failed',errors=1 WHERE import_id=?",(job["import_id"],))
            else:
                for members in groups_for(data,metadata["atomicity"]=="whole_import"):
                    member_hash=hashlib.sha256(_json([x["id"] for x in members]).encode()).hexdigest()
                    group_id="group-"+member_hash
                    db.execute("INSERT INTO admission_groups(id,import_id,context_id,member_hash,member_count) VALUES (?,?,?,?,?)",
                               (group_id,job["import_id"],metadata["validation_context_id"],member_hash,len(members)))
                    db.executemany("UPDATE staged_records SET group_id=? WHERE id=?",[(group_id,x["id"]) for x in members])
                    self.store._enqueue(db,"validate_group",group_id,job["import_id"])
                db.execute("UPDATE progress SET enumeration='complete',total=? WHERE import_id=?",(len(records),job["import_id"]))
            self.store._history(db,[row["id"] for row in records],revision)
            self._complete(db,job)
            self.fault("before_enumeration_commit")

    def _group_input(self, db, job):
        group=db.execute("SELECT * FROM admission_groups WHERE id=?",(job["subject_id"],)).fetchone()
        if not group: raise IntakeError("STATE_CORRUPT")
        records=[dict(x) for x in db.execute("SELECT * FROM staged_records WHERE group_id=? ORDER BY ordinal",(group["id"],))]
        return dict(group),records,self.store._context(db,group["context_id"])

    def _check_group(self, db, job, group):
        self._lease(db,job)
        current=db.execute("SELECT * FROM admission_groups WHERE id=?",(group["id"],)).fetchone()
        if current["revision"]!=group["revision"] or current["context_id"]!=group["context_id"]:
            raise IntakeError("LEASE_LOST")
        return current

    def _validate(self, job):
        with self.store._db() as db:
            group,records,context=self._group_input(db,job)
            ref=db.execute("SELECT * FROM reference_snapshots WHERE id=?",(context["reference_snapshot"],)).fetchone()
        accounts=set(json.loads(ref["body"])["accounts"]) if ref else set()
        instruments=set(json.loads(ref["body"])["instruments"]) if ref else set()
        errors={row["id"]:list(json.loads(row["body"])["issues"]) for row in records}
        states={row["id"]:"invalid" if errors[row["id"]] else "valid" for row in records}
        group_errors=[]; dependencies=[]
        if not self._supported(context):
            group_errors.append((issue("UNSUPPORTED_CONTEXT"),"dependency"))
        elif not ref:
            group_errors.append((issue("REFERENCE_SNAPSHOT_UNAVAILABLE"),"dependency"))
            dependencies.append(("snapshot",context["reference_snapshot"]))
        else:
            for row in records:
                data=json.loads(row["body"]); raw=data.get("raw") or {}
                if not data.get("packet") or states[row["id"]]=="invalid": continue
                if raw["account_id"] not in accounts:
                    errors[row["id"]].append(issue("UNKNOWN_ACCOUNT","/account_id",classification="dependency")); states[row["id"]]="blocked"
                event=raw.get("event",{})
                if event.get("type")=="equity_trade" and event["instrument_id"] not in instruments:
                    errors[row["id"]].append(issue("UNKNOWN_INSTRUMENT","/event/instrument_id",classification="dependency")); states[row["id"]]="blocked"
        if group["member_count"]>self.limits.max_group_records:
            group_errors.append((issue("GROUP_LIMIT_EXCEEDED"),"invalid"))
        examined=None
        if not group_errors and all(state=="valid" for state in states.values()):
            engine=self._engine()
            with self.store._db() as db: state=engine.sync(db)
            examined=state["revision"]
            ordered=causal_order([dict(json.loads(row["body"]),id=row["id"]) for row in records])
            now=max(self.store.clock(),state["recorded_ns"])
            if not 0<=now<=2**63-1: raise IntakeError("CLOCK_RANGE")
            accepted,error=engine.prepare([r["packet"] for r in ordered],now)
            if error:
                code,classification=self._core_error(error)
                target=next((r for r in records if json.loads(r["body"])["canonical_id"]==error["canonical_id"]),records[0])
                errors[target["id"]].append(issue(code,message=error["message"],classification=classification,related_ids=[error["related_id"]] if error["related_id"] else []))
                states[target["id"]]="blocked" if classification=="dependency" else "invalid"
                if classification=="dependency": dependencies.append(("record",error["related_id"]))
            else: engine.discard()
        outcome="invalid" if "invalid" in states.values() or any(c=="invalid" for _,c in group_errors) else "blocked" if "blocked" in states.values() or group_errors else "valid"
        if group_errors and outcome=="blocked":
            states={k:"blocked" if v=="valid" else v for k,v in states.items()}
        self.fault("before_validation_publish")
        with self.store._db(write=True) as db:
            self._check_group(db,job,group)
            run_id="run-"+uuid4().hex
            run={"run_id":run_id,"kind":"validation","group_id":group["id"],"context_id":group["context_id"],
                 "context":context,"reference_digest":ref["digest"] if ref else None,
                 "engine_sha256":self._engine().digest,"ledger_revision_examined":examined,
                 "outcome":outcome,"finished_at":_timestamp(self.store.clock())}
            db.execute("INSERT INTO validation_runs(id,import_id,group_id,body) VALUES (?,?,?,?)",(run_id,job["import_id"],group["id"],_json(run)))
            vd,cd=Counter(),Counter()
            for row in records:
                a,b=self.store._record_state(db,row,validation=states[row["id"]],commitment="queued" if outcome=="valid" else "not_committed",run_id=run_id)
                vd.update(a); cd.update(b)
                body={"state":states[row["id"]],"context_id":group["context_id"],"run_id":run_id,"diagnostics":errors[row["id"]]}
                db.execute("INSERT INTO outcomes VALUES (?,?,?)",(run_id,row["id"],_json(body)))
                for error in errors[row["id"]]:
                    self._diagnostic(db,job["import_id"],error,record=row,group_id=group["id"],run_id=run_id,classification=error.get("classification","invalid"))
            for error,classification in group_errors:
                self._diagnostic(db,job["import_id"],error,group_id=group["id"],run_id=run_id,classification=classification)
            db.execute("UPDATE admission_groups SET validation=?,commitment=?,run_id=?,revision=revision+1 WHERE id=?",
                       (outcome,"queued" if outcome=="valid" else "not_committed",run_id,group["id"]))
            db.execute("DELETE FROM dependencies WHERE group_id=?",(group["id"],))
            db.executemany("INSERT OR IGNORE INTO dependencies VALUES (?,?,?)",[(group["id"],kind,key) for kind,key in dependencies])
            self.store._counts(db,job["import_id"],vd,cd)
            revision=self.store._event(db,job["import_id"],"validation."+outcome,[group["id"]])
            self.store._history(db,[row["id"] for row in records],revision)
            if outcome=="valid": self.store._enqueue(db,"admit_group",group["id"],job["import_id"])
            self._complete(db,job)
            self.fault("before_validation_commit")
        self.fault("after_validation_commit")

    @staticmethod
    def _core_error(error):
        codes={"causal_reference_missing":("MISSING_PREDECESSOR","dependency"),
               "causal_cycle":("CAUSAL_CYCLE","invalid"),"causal_self_reference":("CAUSAL_CYCLE","invalid"),
               "duplicate_identity":("DUPLICATE_IDENTITY","conflict"),
               "conflicting_lifecycle_successor":("CONFLICTING_SUCCESSOR","conflict"),
               "incompatible_account":("INCOMPATIBLE_ACCOUNT","invalid"),
               "incompatible_event_relationship":("INVALID_LIFECYCLE_RELATIONSHIP","invalid")}
        return codes.get(error["category"],("ADMISSION_CONFLICT","conflict"))

    @contextmanager
    def _admission_lock(self):
        fd=os.open(self.store.path/"locks"/"admission",os.O_CREAT|os.O_RDWR,0o600)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX); yield
        finally: os.close(fd)

    def _admit(self, job):
        with self._admission_lock():
            with self.store._db() as db:
                group,records,context=self._group_input(db,job)
                self._lease(db,job)
                existing=db.execute("SELECT id FROM commits WHERE group_id=?",(group["id"],)).fetchone()
                if not existing:
                    if group["validation"]!="valid" or group["commitment"]!="queued": raise IntakeError("LEASE_LOST")
                    engine=self._engine(); state=engine.sync(db)
            if existing:
                with self.store._db(write=True) as db: self._complete(db,job)
                return
            ordered=causal_order([dict(json.loads(row["body"]),id=row["id"]) for row in records])
            if state["last_sequence"]+len(ordered)>self.limits.max_ledger_records: raise IntakeError("LEDGER_LIMIT_EXCEEDED")
            now=max(self.store.clock(),state["recorded_ns"])
            if not 0<=now<=2**63-1: raise IntakeError("CLOCK_RANGE")
            accepted,error=engine.prepare([r["packet"] for r in ordered],now)
            self.fault("after_admission_prepare")
            with self.store._db(write=True) as db:
                self._check_group(db,job,group)
                if db.execute("SELECT revision FROM ledger_state WHERE id=1").fetchone()[0]!=state["revision"]:
                    raise IntakeError("LEDGER_REVISION_CHANGED",retryable=True)
                admission_run_id="run-"+uuid4().hex
                admission_run={"run_id":admission_run_id,"kind":"admission","group_id":group["id"],
                               "context_id":group["context_id"],"context":context,
                               "validation_run_id":group["run_id"],"engine_sha256":engine.digest,
                               "ledger_revision_examined":state["revision"],
                               "outcome":"rejected" if error else "committed", "finished_at":_timestamp(self.store.clock())}
                db.execute("INSERT INTO validation_runs(id,import_id,group_id,body) VALUES (?,?,?,?)",
                           (admission_run_id,job["import_id"],group["id"],_json(admission_run)))
                if error:
                    code,classification=self._core_error(error)
                    self._diagnostic(db,job["import_id"],issue(code,message=error["message"],related_ids=[error["related_id"]] if error["related_id"] else []),
                                     group_id=group["id"],run_id=admission_run_id,classification=classification)
                    target="blocked" if classification=="dependency" else "conflict"
                    vd,cd=Counter(),Counter()
                    for row in records:
                        a,b=self.store._record_state(db,row,commitment="not_committed" if target=="blocked" else "conflict")
                        vd.update(a); cd.update(b)
                    db.execute("UPDATE admission_groups SET validation=?,commitment=?,revision=revision+1 WHERE id=?",
                               ("blocked" if target=="blocked" else "valid","not_committed" if target=="blocked" else "conflict",group["id"]))
                    if target=="blocked": db.execute("INSERT OR IGNORE INTO dependencies VALUES (?,'record',?)",(group["id"],error["related_id"]))
                    self.store._counts(db,job["import_id"],vd,cd)
                    revision=self.store._event(db,job["import_id"],"group."+target,[group["id"]])
                    self.store._history(db,[r["id"] for r in records],revision)
                    self._complete(db,job); return
                commit_id="commit-"+uuid4().hex
                receipt={"commit_id":commit_id,"import_id":job["import_id"],"group_id":group["id"],
                         "member_hash":group["member_hash"],"validation_run_id":group["run_id"],"admission_run_id":admission_run_id,"context":context,
                         "engine_sha256":engine.digest,"book_id":self.store.book,"prior_revision":str(state["revision"]),
                         "ledger_revision":str(state["revision"]+1),"record_count":len(accepted),
                         "first_sequence":str(accepted[0][0]),"last_sequence":str(accepted[-1][0]),
                         "recorded_ns":str(now),"records_url":f"/v1/books/{quote(self.store.book,safe='')}/imports/{job['import_id']}/records?commit_id={commit_id}"}
                db.execute("INSERT INTO commits(id,import_id,group_id,body) VALUES (?,?,?,?)",(commit_id,job["import_id"],group["id"],_json(receipt)))
                for record,(sequence,canonical) in zip(ordered,accepted):
                    db.execute("INSERT INTO ledger_records VALUES (?,?,?,?,?,?)",(sequence,record["canonical_id"],record["economic_id"],commit_id,hashlib.sha256(canonical).hexdigest(),canonical))
                db.execute("UPDATE ledger_state SET revision=revision+1,last_sequence=?,recorded_ns=? WHERE id=1",(accepted[-1][0],now))
                vd,cd=Counter(),Counter()
                for row in records:
                    a,b=self.store._record_state(db,row,commitment="committed",commit_id=commit_id); vd.update(a); cd.update(b)
                db.execute("UPDATE admission_groups SET commitment='committed',revision=revision+1 WHERE id=?",(group["id"],))
                self.store._counts(db,job["import_id"],vd,cd)
                revision=self.store._event(db,job["import_id"],"group.committed",[group["id"]])
                self.store._history(db,[r["id"] for r in records],revision)
                self._complete(db,job)
                self.fault("before_admission_commit")
            self.fault("after_admission_commit")
            engine.publish(len(accepted)); engine.recorded_ns=now
            self.fault("after_admission_publish")
