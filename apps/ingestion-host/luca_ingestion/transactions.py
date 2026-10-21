"""Strict JSONL shape/identity adapter. Exact values and lifecycle rules use C++."""
import hashlib
import json
import re
from datetime import datetime

from .store import IntakeError, _framed_hash, _json, _token


def issue(code, field="", message="Input does not satisfy the transaction schema.", **extra):
    return {"code":code,"field_path":field,"message":message,**extra}


def timestamp_shape(value):
    # Source evidence time is metadata, not a financial timestamp or host clock.
    # Match the same UTC/nanosecond wire grammar; calendar validity is explicit.
    if not isinstance(value,str) or not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]{1,9})?Z",value): return False
    try: datetime.strptime(value[:19],"%Y-%m-%dT%H:%M:%S")
    except ValueError: return False
    return True


def pointer(key): return key.replace("~","~0").replace("/","~1")


def parse_record(store, import_id, metadata, context, ordinal, raw, locator):
    evidence="src-"+_framed_hash([import_id,str(ordinal)])
    result={"evidence_id":evidence,"locator":locator,"raw":None,"packet":None,"issues":[],
            "canonical_id":None,"economic_id":None,"raw_id":None,"raw_causal":None,"atomic_group":None}
    def pairs(items):
        value={}
        for key,item in items:
            if key in value: raise ValueError("DUPLICATE_FIELD")
            value[key]=item
        return value
    def reject_constant(value): raise ValueError("PARSE_ERROR")
    try:
        text=raw.decode("utf-8")
        data=json.loads(text,object_pairs_hook=pairs,parse_constant=reject_constant)
        # Reject surrogate escapes before any metadata is serialized to UTF-8.
        _json(data).encode("utf-8")
    except UnicodeError:
        result["issues"]=[issue("INVALID_UTF8")]; return result
    except (ValueError,RecursionError) as error:
        result["issues"]=[issue("DUPLICATE_FIELD" if str(error)=="DUPLICATE_FIELD" else "PARSE_ERROR")]; return result
    if not isinstance(data,dict):
        result["issues"]=[issue("RECORD_TYPE")]; return result
    result["raw"]=data
    allowed={"record_id","economic_event_id","action","account_id","causal_record_id","external_record_id","source_event_at","atomic_group","event"}
    for key in sorted(set(data)-allowed):
        result["issues"].append(issue("CROSS_BOOK_DEPENDENCY" if key=="book_id" else "FIELD_UNKNOWN","/"+pointer(key)))
    def text_field(obj,key,path,required=True):
        if key not in obj and not required: return ""
        try: return _token(obj.get(key))
        except IntakeError:
            result["issues"].append(issue("FIELD_REQUIRED" if key not in obj else "FIELD_TYPE",path)); return ""
    record=text_field(data,"record_id","/record_id")
    economic=text_field(data,"economic_event_id","/economic_event_id")
    action=text_field(data,"action","/action")
    account=text_field(data,"account_id","/account_id")
    causal=text_field(data,"causal_record_id","/causal_record_id",action!="originate")
    group=text_field(data,"atomic_group","/atomic_group",False)
    for optional in ("external_record_id","source_event_at"):
        if optional in data: text_field(data,optional,"/"+optional)
    if "source_event_at" in data and not timestamp_shape(data["source_event_at"]):
        result["issues"].append(issue("INVALID_TIMESTAMP","/source_event_at"))
    result.update(raw_id=record or None,raw_causal=causal or None,atomic_group=group or None)
    if "atomic_group" in data and not group: result["membership_unknown"]=True
    def identity(kind,value):
        return kind+"-"+_framed_hash(["source-book-identity/1",store.tenant,store.book,metadata["source_id"],kind,value]) if value else ""
    result["canonical_id"]=identity("event",record) or None
    result["economic_id"]=identity("economic",economic) or None
    event=data.get("event",{})
    if action=="cancel":
        if "event" in data: result["issues"].append(issue("FIELD_UNKNOWN","/event"))
        event={}
    elif not isinstance(event,dict) or not event:
        result["issues"].append(issue("FIELD_REQUIRED","/event")); event={}
    event_type=event.get("type","")
    event_keys={"type","effective_at","currency","amount"} if event_type=="cash_movement" else {"type","effective_at","instrument_id","quantity","price","quote_currency","settlement_date"}
    for key in sorted(set(event)-event_keys): result["issues"].append(issue("FIELD_UNKNOWN","/event/"+pointer(key)))
    fields={key:text_field(event,key,"/event/"+pointer(key),action!="cancel") for key in event_keys}
    if action!="cancel" and event_type not in ("cash_movement","equity_trade"):
        result["issues"].append(issue("UNSUPPORTED_EVENT_TYPE","/event/type"))
    for key in ("amount","quantity","price"):
        if key in event and not isinstance(event[key],str):
            result["issues"]=[x for x in result["issues"] if x["field_path"]!="/event/"+pointer(key)]
            result["issues"].append(issue("INVALID_DECIMAL","/event/"+pointer(key)))
    if not result["issues"]:
        result["packet"]=[result["canonical_id"],result["economic_id"],action,account,identity("event",causal),
                          evidence,"0",event_type,fields.get("effective_at",""),fields.get("currency",fields.get("quote_currency","")),
                          fields.get("amount",""),fields.get("instrument_id",""),fields.get("quantity",""),fields.get("price",""),
                          fields.get("settlement_date",""),context["normalization"]]
    return result


def groups_for(records, whole_import):
    """Conservative dependency/declared-group closure with stable ordinal order."""
    parents=list(range(len(records)))
    def root(i):
        while parents[i]!=i:
            parents[i]=parents[parents[i]]; i=parents[i]
        return i
    def join(a,b): parents[root(a)]=root(b)
    ids={}; labels={}; origins={}
    for index,record in enumerate(records):
        for table,key in ((ids,record.get("raw_id")),(labels,record.get("atomic_group"))):
            if key:
                if key in table: join(index,table[key])
                else: table[key]=index
        data=record.get("raw") or {}
        if data.get("action") in ("originate","reverse") and record.get("economic_id"):
            key=record["economic_id"]
            if key in origins: join(index,origins[key])
            else: origins[key]=index
        if whole_import and index: join(index,0)
    for index,record in enumerate(records):
        if record.get("raw_causal") in ids: join(index,ids[record["raw_causal"]])
    result={}
    for index,record in enumerate(records): result.setdefault(root(index),[]).append(record)
    return list(result.values())


def causal_order(records):
    import heapq
    by_id={r.get("raw_id"):i for i,r in enumerate(records)}
    degrees=[0]*len(records); children={}
    for index,record in enumerate(records):
        parent=by_id.get(record.get("raw_causal")) if record.get("raw_causal") else None
        if parent is not None:
            degrees[index]+=1; children.setdefault(parent,[]).append(index)
    ready=[i for i,d in enumerate(degrees) if not d]; heapq.heapify(ready); result=[]
    while ready:
        index=heapq.heappop(ready); result.append(records[index])
        for child in children.get(index,[]):
            degrees[child]-=1
            if not degrees[child]: heapq.heappush(ready,child)
    # The core diagnoses cycles; ordering must not hide/drop their records.
    return result if len(result)==len(records) else records
