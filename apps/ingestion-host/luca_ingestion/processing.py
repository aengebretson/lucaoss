"""Transactional processing metadata and read APIs; financial rules stay in C++."""
import base64
from collections import Counter
import hashlib
import json
from uuid import uuid4
from urllib.parse import quote

from .store import IntakeError, _integer, _json, _token, _timestamp


def migrate(db):
    if db.execute("PRAGMA user_version").fetchone()[0] == 2:
        return
    statements = [
        "ALTER TABLE transfers ADD COLUMN queue_reserved INTEGER NOT NULL DEFAULT 1",
        "ALTER TABLE work ADD COLUMN import_id TEXT REFERENCES imports(id)",
        "ALTER TABLE work ADD COLUMN owner TEXT",
        "ALTER TABLE work ADD COLUMN generation INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE work ADD COLUMN lease_until INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE work ADD COLUMN attempts INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE work ADD COLUMN ready_ns INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE work ADD COLUMN last_error TEXT",
        "UPDATE work SET import_id=subject_id WHERE kind='parse_import'",
        "CREATE INDEX processing_work ON work(kind,state,ready_ns,id)",
        "CREATE INDEX import_work ON work(import_id,state)",
        """CREATE TABLE progress(import_id TEXT PRIMARY KEY REFERENCES imports(id),
          revision INTEGER NOT NULL DEFAULT 1, enumeration TEXT NOT NULL DEFAULT 'pending',
          total INTEGER, vc TEXT NOT NULL DEFAULT '{}', cc TEXT NOT NULL DEFAULT '{}',
          errors INTEGER NOT NULL DEFAULT 0)""",
        "INSERT INTO progress(import_id) SELECT id FROM imports",
        "CREATE TRIGGER import_progress AFTER INSERT ON imports BEGIN INSERT INTO progress(import_id) VALUES (new.id); END",
        "CREATE TABLE reference_snapshots(id TEXT PRIMARY KEY, digest TEXT NOT NULL, body TEXT NOT NULL)",
        "CREATE TABLE context_history(id TEXT PRIMARY KEY, body TEXT NOT NULL, created_ns INTEGER NOT NULL)",
        "CREATE TABLE revalidation_requests(id TEXT PRIMARY KEY, body TEXT NOT NULL, created_ns INTEGER NOT NULL)",
        """CREATE TABLE staged_records(id TEXT PRIMARY KEY, import_id TEXT NOT NULL REFERENCES imports(id),
          ordinal INTEGER NOT NULL, body TEXT NOT NULL, group_id TEXT, validation TEXT NOT NULL,
          commitment TEXT NOT NULL DEFAULT 'not_committed', run_id TEXT, commit_id TEXT,
          UNIQUE(import_id,ordinal))""",
        "CREATE INDEX group_records ON staged_records(group_id,ordinal)",
        """CREATE TABLE record_history(record_id TEXT NOT NULL REFERENCES staged_records(id),
          revision INTEGER NOT NULL, body TEXT NOT NULL, PRIMARY KEY(record_id,revision))""",
        """CREATE TABLE admission_groups(id TEXT PRIMARY KEY, import_id TEXT NOT NULL REFERENCES imports(id),
          context_id TEXT NOT NULL, revision INTEGER NOT NULL DEFAULT 1, validation TEXT NOT NULL DEFAULT 'pending',
          commitment TEXT NOT NULL DEFAULT 'not_committed', run_id TEXT, member_hash TEXT NOT NULL,
          member_count INTEGER NOT NULL)""",
        "CREATE INDEX import_groups ON admission_groups(import_id,id)",
        """CREATE TABLE validation_runs(seq INTEGER PRIMARY KEY, id TEXT UNIQUE NOT NULL,
          import_id TEXT NOT NULL REFERENCES imports(id), group_id TEXT NOT NULL REFERENCES admission_groups(id),
          body TEXT NOT NULL)""",
        "CREATE TABLE outcomes(run_id TEXT NOT NULL, record_id TEXT NOT NULL, body TEXT NOT NULL, PRIMARY KEY(run_id,record_id))",
        """CREATE TABLE diagnostics(seq INTEGER PRIMARY KEY, id TEXT UNIQUE NOT NULL,
          import_id TEXT NOT NULL REFERENCES imports(id), group_id TEXT, record_id TEXT,
          run_id TEXT, code TEXT NOT NULL, body TEXT NOT NULL)""",
        "CREATE INDEX import_diagnostics ON diagnostics(import_id,seq)",
        "CREATE INDEX record_diagnostics ON diagnostics(record_id,seq)",
        "CREATE TABLE dependencies(group_id TEXT NOT NULL REFERENCES admission_groups(id), kind TEXT NOT NULL, key TEXT NOT NULL, PRIMARY KEY(group_id,kind,key))",
        "CREATE INDEX dependency_key ON dependencies(kind,key,group_id)",
        """CREATE TABLE ledger_state(id INTEGER PRIMARY KEY CHECK(id=1), revision INTEGER NOT NULL,
          last_sequence INTEGER NOT NULL, recorded_ns INTEGER NOT NULL)""",
        "INSERT INTO ledger_state VALUES (1,0,0,0)",
        "CREATE TABLE commits(seq INTEGER PRIMARY KEY, id TEXT UNIQUE NOT NULL, import_id TEXT NOT NULL REFERENCES imports(id), group_id TEXT NOT NULL UNIQUE REFERENCES admission_groups(id), body TEXT NOT NULL)",
        """CREATE TABLE ledger_records(sequence INTEGER PRIMARY KEY, record_id TEXT UNIQUE NOT NULL,
          economic_id TEXT NOT NULL, commit_id TEXT NOT NULL REFERENCES commits(id),
          digest TEXT NOT NULL, canonical BLOB NOT NULL)""",
        "CREATE TABLE attempts(seq INTEGER PRIMARY KEY, work_id TEXT NOT NULL REFERENCES work(id), body TEXT NOT NULL)",
        "CREATE TABLE processing_config(id INTEGER PRIMARY KEY CHECK(id=1),body TEXT NOT NULL)",
        "PRAGMA user_version=2",
    ]
    for statement in statements:
        db.execute(statement)


class ProcessingStore:
    def register_context(self, context_id, definition):
        _token(context_id)
        if not isinstance(definition, dict) or set(definition) != {"adapter", "normalization", "rules", "engine", "reference_snapshot"}:
            raise IntakeError("INVALID_CONFIGURATION")
        for value in definition.values():
            _token(value)
        with self._db(write=True) as db:
            config = json.loads(db.execute("SELECT value FROM config WHERE id=1").fetchone()[0])
            old = config["contexts"].get(context_id)
            if old is not None and old != definition:
                raise IntakeError("CONTEXT_CONFLICT")
            config["contexts"][context_id] = dict(definition)
            db.execute("UPDATE config SET value=? WHERE id=1", (_json(config),))
            db.execute("INSERT OR IGNORE INTO context_history VALUES (?,?,?)", (context_id, _json(definition), self.clock()))
        self.contexts = config["contexts"]

    def register_reference(self, snapshot_id, *, accounts, instruments):
        _token(snapshot_id)
        if not isinstance(accounts, list) or not isinstance(instruments, list) or len(accounts)+len(instruments)>200000:
            raise IntakeError("LIMIT_EXCEEDED")
        body = _json({"accounts": sorted(set(_token(x) for x in accounts)),
                      "instruments": sorted(set(_token(x) for x in instruments))})
        digest = hashlib.sha256(body.encode()).hexdigest()
        with self._db(write=True) as db:
            old = db.execute("SELECT digest FROM reference_snapshots WHERE id=?", (snapshot_id,)).fetchone()
            if old and old[0] != digest:
                raise IntakeError("REFERENCE_CONFLICT")
            db.execute("INSERT OR IGNORE INTO reference_snapshots VALUES (?,?,?)", (snapshot_id, digest, body))
        self.wake_dependencies()
        return {"snapshot_id": snapshot_id, "digest": digest}

    def _context(self, db, context_id):
        contexts = json.loads(db.execute("SELECT value FROM config WHERE id=1").fetchone()[0])["contexts"]
        if context_id not in contexts:
            raise IntakeError("UNSUPPORTED_CONTEXT_OR_FORMAT")
        return contexts[context_id]

    def _event(self, db, import_id, event_type, subjects=()):
        db.execute("UPDATE progress SET revision=revision+1 WHERE import_id=?", (import_id,))
        revision = db.execute("SELECT revision FROM progress WHERE import_id=?", (import_id,)).fetchone()[0]
        event = {"event_id": uuid4().hex, "import_id": import_id, "revision": str(revision),
                 "type": event_type, "occurred_at": _timestamp(self.clock()), "subject_ids": list(subjects)[:32], "links": {}}
        db.execute("INSERT INTO events VALUES (?,?,?)", (import_id, revision, _json(event)))
        return revision

    def _history(self, db, record_ids, revision):
        for record_id in record_ids:
            row = db.execute("SELECT * FROM staged_records WHERE id=?", (record_id,)).fetchone()
            data = {"validation": row["validation"], "commitment": row["commitment"],
                    "group_id": row["group_id"], "run_id": row["run_id"], "commit_id": row["commit_id"]}
            run=db.execute("SELECT body FROM validation_runs WHERE id=?",(row["run_id"],)).fetchone() if row["run_id"] else None
            if run:
                context=json.loads(run[0]); data["context_id"]=context["context_id"]
                data["ledger_revision_examined"]=context.get("ledger_revision_examined")
            else:
                group=db.execute("SELECT context_id FROM admission_groups WHERE id=?",(row["group_id"],)).fetchone() if row["group_id"] else None
                metadata=json.loads(db.execute("SELECT metadata FROM imports WHERE id=?",(row["import_id"],)).fetchone()[0])
                data["context_id"]=group[0] if group else metadata["validation_context_id"]
                data["ledger_revision_examined"]=None
            db.execute("INSERT INTO record_history VALUES (?,?,?)", (record_id, revision, _json(data)))

    def _counts(self, db, import_id, validation_delta=None, commitment_delta=None):
        row = db.execute("SELECT vc,cc FROM progress WHERE import_id=?", (import_id,)).fetchone()
        vc, cc = Counter(json.loads(row[0])), Counter(json.loads(row[1]))
        vc.update(validation_delta or {})
        cc.update(commitment_delta or {})
        if any(value<0 for value in (*vc.values(),*cc.values())): raise IntakeError("STATE_CORRUPT")
        db.execute("UPDATE progress SET vc=?,cc=? WHERE import_id=?", (_json(dict(vc)), _json(dict(cc)), import_id))

    def _record_state(self, db, record, *, validation=None, commitment=None, run_id=None, commit_id=None):
        vd, cd = Counter(), Counter()
        if validation is not None:
            vd[record["validation"]]-=1; vd[validation]+=1
        if commitment is not None:
            cd[record["commitment"]]-=1; cd[commitment]+=1
        db.execute("UPDATE staged_records SET validation=?,commitment=?,run_id=?,commit_id=? WHERE id=?",
                   (validation or record["validation"], commitment or record["commitment"],
                    run_id or record["run_id"], commit_id or record["commit_id"], record["id"]))
        return vd, cd

    def _reserve_pipeline(self, db, import_id):
        transfer = db.execute("SELECT t.id,t.queue_reserved FROM transfers t JOIN imports i ON i.transfer_id=t.id WHERE i.id=?", (import_id,)).fetchone()
        if transfer["queue_reserved"]:
            return True
        if db.execute("SELECT slots FROM capacity WHERE id=1").fetchone()[0] >= self.limits.max_pending_imports:
            return False
        db.execute("UPDATE transfers SET queue_reserved=1 WHERE id=?", (transfer["id"],))
        db.execute("UPDATE capacity SET slots=slots+1 WHERE id=1")
        return True

    def _idle_release(self, db, import_id):
        if db.execute("SELECT 1 FROM work WHERE import_id=? AND state IN ('queued','running','retry_wait') LIMIT 1", (import_id,)).fetchone():
            return
        transfer = db.execute("SELECT transfer_id FROM imports WHERE id=?", (import_id,)).fetchone()[0]
        changed = db.execute("UPDATE transfers SET queue_reserved=0 WHERE id=? AND queue_reserved=1", (transfer,)).rowcount
        if changed:
            db.execute("UPDATE capacity SET slots=slots-1 WHERE id=1")

    def _enqueue(self, db, kind, subject, import_id):
        db.execute("""INSERT INTO work(id,kind,subject_id,state,import_id,ready_ns) VALUES (?,?,?,'queued',?,?)
          ON CONFLICT(kind,subject_id) DO UPDATE SET state='queued',owner=NULL,
          generation=work.generation+1,lease_until=0,ready_ns=excluded.ready_ns,attempts=0,last_error=NULL""",
                   (uuid4().hex, kind, subject, import_id, self.clock()))

    def wake_dependencies(self, limit=100):
        _integer(limit, 1, self.limits.max_page_size)
        with self._db(write=True) as db:
            rows = db.execute("""SELECT DISTINCT g.* FROM admission_groups g JOIN dependencies d ON d.group_id=g.id
              WHERE g.validation='blocked' AND g.commitment='not_committed' AND
              ((d.kind='record' AND EXISTS(SELECT 1 FROM ledger_records l WHERE l.record_id=d.key)) OR
               (d.kind='snapshot' AND EXISTS(SELECT 1 FROM reference_snapshots r WHERE r.id=d.key))) LIMIT ?""", (limit,)).fetchall()
            for row in rows:
                if self._reserve_pipeline(db, row["import_id"]):
                    db.execute("DELETE FROM dependencies WHERE group_id=?", (row["id"],))
                    self._enqueue(db, "validate_group", row["id"], row["import_id"])
                    self._event(db, row["import_id"], "validation.queued", [row["id"]])
        return len(rows)

    def revalidate(self, import_id, key, *, validation_context_id, groups):
        _token(key,128)
        if not isinstance(groups,list) or not 0<len(groups)<=1000:
            raise IntakeError("LIMIT_EXCEEDED")
        if any(not isinstance(item,dict) or set(item)!={"group_id","expected_revision"} for item in groups):
            raise IntakeError("INVALID_SELECTION")
        for item in groups: _token(item["group_id"])
        selection = sorted((item["group_id"],str(item["expected_revision"])) for item in groups)
        if len({item[0] for item in selection})!=len(selection): raise IntakeError("INVALID_SELECTION")
        request = _json([validation_context_id,selection])
        identity = hashlib.sha256((import_id+"\0"+key).encode()).hexdigest()
        with self._db(write=True) as db:
            self._import(db,import_id)
            old=db.execute("SELECT body FROM revalidation_requests WHERE id=?",("revalidate-"+identity,)).fetchone()
            if old:
                result=json.loads(old[0])
                if result["request"]!=request: raise IntakeError("IDEMPOTENCY_CONFLICT")
                return result["receipt"]
            context=self._context(db,validation_context_id)
            selected=[]
            for group_id,revision in selection:
                row=db.execute("SELECT * FROM admission_groups WHERE id=? AND import_id=?",(group_id,import_id)).fetchone()
                if not row or str(row["revision"])!=revision or row["commitment"]=="committed":
                    raise IntakeError("STALE_SELECTION")
                prior=self._context(db,row["context_id"])
                if any(context[k]!=prior[k] for k in ("adapter","normalization")):
                    raise IntakeError("INCOMPATIBLE_CONTEXT")
                selected.append(row)
            if not self._reserve_pipeline(db,import_id): raise IntakeError("CAPACITY_UNAVAILABLE",retryable=True)
            for row in selected:
                db.execute("UPDATE admission_groups SET context_id=?,revision=revision+1,validation='pending',commitment='not_committed',run_id=NULL WHERE id=?",(validation_context_id,row["id"]))
                vd,cd=Counter(),Counter()
                records=db.execute("SELECT * FROM staged_records WHERE group_id=?",(row["id"],)).fetchall()
                for record in records:
                    a,b=self._record_state(db,record,validation="pending",commitment="not_committed")
                    vd.update(a); cd.update(b)
                db.execute("UPDATE staged_records SET run_id=NULL WHERE group_id=?",(row["id"],))
                self._counts(db,import_id,vd,cd)
                db.execute("DELETE FROM dependencies WHERE group_id=?",(row["id"],))
                db.execute("UPDATE work SET state='superseded',generation=generation+1,owner=NULL WHERE subject_id=? AND kind IN ('validate_group','admit_group')",(row["id"],))
                self._enqueue(db,"validate_group",row["id"],import_id)
            revision=self._event(db,import_id,"validation.queued",[x[0] for x in selection])
            for row in selected:
                ids=[x[0] for x in db.execute("SELECT id FROM staged_records WHERE group_id=?",(row["id"],))]
                self._history(db,ids,revision)
            receipt={"revalidation_id":identity,"import_id":import_id,"status_url":f"/v1/books/{quote(self.book,safe='')}/imports/{import_id}"}
            db.execute("INSERT INTO revalidation_requests VALUES (?,?,?)",("revalidate-"+identity,_json({"request":request,"receipt":receipt}),self.clock()))
            return receipt

    def _status(self, db, import_id):
        row=self._import(db,import_id)
        progress=db.execute("SELECT * FROM progress WHERE import_id=?",(import_id,)).fetchone()
        vc={key:json.loads(progress["vc"]).get(key,0) for key in ("pending","running","valid","invalid","blocked")}
        cc={key:json.loads(progress["cc"]).get(key,0) for key in ("not_committed","queued","committed","conflict")}
        states={x[0] for x in db.execute("SELECT DISTINCT state FROM work WHERE import_id=?",(import_id,))}
        health=next((x for x in ("attention_required","running","queued","retry_wait") if x in states),"idle")
        return {"import_id":import_id,"revision":str(progress["revision"]),"receipt":json.loads(row["receipt"]),
                "metadata":json.loads(row["metadata"]),"validation_context":json.loads(row["context"]),
                "enumeration":{"state":progress["enumeration"],"records":progress["total"]},
                "validation_counts":vc,"commitment_record_counts":cc,"work_health":health,
                "import_error_count":progress["errors"],
                "all_records_committed":progress["enumeration"]=="complete" and not progress["errors"] and cc["committed"]==progress["total"],
                "event_cursor":str(progress["revision"])}

    def _record_public(self, db, row, state=None):
        data=json.loads(row["body"])
        state=state or dict(row)
        run=db.execute("SELECT body FROM validation_runs WHERE id=?",(state["run_id"],)).fetchone() if state["run_id"] else None
        context=json.loads(run[0]) if run else {}
        if not run:
            group=db.execute("SELECT context_id FROM admission_groups WHERE id=?",(state["group_id"],)).fetchone() if state["group_id"] else None
            context["context_id"]=group[0] if group else json.loads(db.execute("SELECT metadata FROM imports WHERE id=?",(row["import_id"],)).fetchone()[0])["validation_context_id"]
        return {"record_id":row["id"],"ordinal":row["ordinal"],"source_record_id":data["evidence_id"],
                "locator":data["locator"],"canonical_record_id":data.get("canonical_id"),
                "economic_event_id":data.get("economic_id"),"group_id":state["group_id"],
                "validation":{"state":state["validation"],"run_id":state["run_id"],
                  "context_id":state.get("context_id",context.get("context_id")),"ledger_revision_examined":state.get("ledger_revision_examined",context.get("ledger_revision_examined"))},
                "commitment":{"state":state["commitment"],"commit_id":state["commit_id"]}}

    def record(self, import_id, record_id, *, run_id=None):
        with self._db() as db:
            self._import(db,import_id)
            row=db.execute("SELECT * FROM staged_records WHERE import_id=? AND id=?",(import_id,record_id)).fetchone()
            if not row: raise IntakeError("NOT_FOUND")
            result=self._record_public(db,row)
            if run_id:
                outcome=db.execute("SELECT body FROM outcomes WHERE run_id=? AND record_id=?",(run_id,record_id)).fetchone()
                if not outcome: raise IntakeError("NOT_FOUND")
                result["historical_validation"]=json.loads(outcome[0])
            return result

    def group(self, import_id, group_id):
        with self._db() as db:
            row=db.execute("SELECT * FROM admission_groups WHERE id=? AND import_id=?",(group_id,import_id)).fetchone()
            if not row: raise IntakeError("NOT_FOUND")
            return dict(row)

    def records(self, import_id, *, cursor=None, limit=100, **filters):
        _integer(limit,1,self.limits.max_page_size)
        if set(filters)-{"validation_state","commitment_state","group_id","commit_id"}: raise IntakeError("INVALID_FILTER")
        with self._db() as db:
            self._import(db,import_id)
            snapshot=db.execute("SELECT revision FROM progress WHERE import_id=?",(import_id,)).fetchone()[0]
            current=snapshot
            after=0
            if cursor:
                snapshot,after=self._uncursor(cursor,"records",import_id,filters)
                if snapshot>current: raise IntakeError("INVALID_CURSOR")
            rows=db.execute("""SELECT r.*,h.body AS history FROM staged_records r JOIN record_history h ON h.record_id=r.id
              WHERE r.import_id=? AND r.ordinal>? AND h.revision=(SELECT max(x.revision) FROM record_history x WHERE x.record_id=r.id AND x.revision<=?)
              ORDER BY r.ordinal""",(import_id,after,snapshot))
            items=[]; next_cursor=None
            for row in rows:
                state=json.loads(row["history"])
                if any(state[{"validation_state":"validation","commitment_state":"commitment"}.get(k,k)]!=v for k,v in filters.items()): continue
                if len(items)==limit:
                    next_cursor=self._cursor("records",import_id,filters,snapshot,items[-1]["ordinal"]); break
                items.append(self._record_public(db,row,state))
            return {"items":items,"snapshot_revision":str(snapshot),"next_cursor":next_cursor}

    def _cursor(self, kind, import_id, filters, snapshot, after):
        data=_json([kind,import_id,filters,snapshot,after,self.clock()+900*10**9])
        return base64.urlsafe_b64encode(data.encode()).decode()

    def _uncursor(self, cursor, kind, import_id, filters):
        try:
            value=json.loads(base64.urlsafe_b64decode(cursor))
            if not isinstance(value,list) or len(value)!=6 or value[:3]!=[kind,import_id,filters]: raise ValueError()
            if any(type(v) is not int or not 0<=v<=2**63-1 for v in value[3:]): raise ValueError()
            if value[5]<=self.clock(): raise IntakeError("CURSOR_EXPIRED")
            return value[3],value[4]
        except (ValueError,TypeError,IndexError): raise IntakeError("INVALID_CURSOR") from None

    def _immutable_page(self, table, import_id, cursor=None, limit=100, **filters):
        _integer(limit,1,self.limits.max_page_size)
        permitted={"diagnostics":{"record_id","group_id","run_id","code"},"validation_runs":{"group_id"},"commits":{"group_id"}}[table]
        if set(filters)-permitted: raise IntakeError("INVALID_FILTER")
        with self._db() as db:
            self._import(db,import_id)
            maximum=db.execute(f"SELECT coalesce(max(seq),0) FROM {table} WHERE import_id=?",(import_id,)).fetchone()[0]
            after=0
            if cursor:
                captured,after=self._uncursor(cursor,table,import_id,filters)
                if captured>maximum: raise IntakeError("INVALID_CURSOR")
                maximum=captured
            clauses="".join(f" AND {name}=?" for name in filters)
            rows=db.execute(f"SELECT seq,body FROM {table} WHERE import_id=? AND seq>? AND seq<=?{clauses} ORDER BY seq LIMIT ?",
                            (import_id,after,maximum,*filters.values(),limit+1)).fetchall()
            chosen=rows[:limit]
            return {"items":[json.loads(x["body"]) for x in chosen],"snapshot_revision":str(maximum),
                    "next_cursor":self._cursor(table,import_id,filters,maximum,chosen[-1]["seq"]) if len(rows)>limit else None}

    def diagnostics(self, import_id, **kwargs): return self._immutable_page("diagnostics",import_id,**kwargs)
    def validation_runs(self, import_id, **kwargs): return self._immutable_page("validation_runs",import_id,**kwargs)
    def commits(self, import_id, **kwargs): return self._immutable_page("commits",import_id,**kwargs)

    def book_state(self):
        with self._db() as db:
            return dict(db.execute("SELECT revision,last_sequence,recorded_ns FROM ledger_state WHERE id=1").fetchone())

    def ledger_records(self, *, after_sequence=0, limit=100):
        _integer(limit,1,self.limits.max_page_size)
        _integer(after_sequence,0,2**63-1)
        with self._db() as db:
            return [dict(x) for x in db.execute("SELECT * FROM ledger_records WHERE sequence>? ORDER BY sequence LIMIT ?",(after_sequence,limit))]

    def work_attempts(self, import_id, *, after=0, limit=100):
        _integer(after,0,2**63-1); _integer(limit,1,self.limits.max_page_size)
        with self._db() as db:
            self._import(db,import_id)
            rows=db.execute("SELECT a.seq,a.body FROM attempts a JOIN work w ON w.id=a.work_id WHERE w.import_id=? AND a.seq>? ORDER BY a.seq LIMIT ?",(import_id,after,limit)).fetchall()
            return {"items":[json.loads(x["body"]) for x in rows],"next_after":rows[-1]["seq"] if rows else after}
