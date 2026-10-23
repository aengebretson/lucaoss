"""Disposable native books; complete synchronous period P&L call measurements.
Usage: python3 -B tests/benchmarks/application_period_benchmark.py BUILD [20 100 250 450]
Setup is excluded. No product processing or financial calculations in Python.
"""
import datetime
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'application'))
from support import prepare, metadata, trade
build=Path(sys.argv[1]).resolve()
cli=build/'libs/application/luca';ingest=build/'libs/ingestion/luca-ingest'
for count in list(map(int,sys.argv[2:])) or [20,100,250,450]:
    assert 2<=count<=1000 and count%2==0
    with tempfile.TemporaryDirectory(prefix='luca-period-benchmark-') as temp:
        root=Path(temp);profile,binding,admin=prepare(root,ingest)
        rows=[]
        for i in range(count):
            row=trade('r'+str(i),'e'+str(i),'1' if i%2==0 else '-1','100' if i%2==0 else '110')
            row['event']['effective_at']=(datetime.datetime(2026,1,10)+datetime.timedelta(seconds=i)).isoformat()+'Z'
            row['atomic_group']='bounded-group';rows.append(row)
        payload=root/'input.jsonl';payload.write_text(''.join(json.dumps(r)+'\n' for r in rows))
        meta=root/'metadata.json';meta.write_text(json.dumps(metadata()))
        receipt=admin('receive','--key','benchmark','--metadata',str(meta),'--payload',str(payload))
        admin('worker','--mode','drain');assert admin('status',receipt['import_id'])['all_records_committed']
        request=root/'request.json';request.write_text(json.dumps({'book_id':'fixture-book'}))
        cap=json.loads(subprocess.check_output([str(cli),'--profile',str(profile),'capabilities','--request',str(request)]))['data']
        args=dict(book_id='fixture-book',accepted_through=str(count),policy_through='1',settlement_through='0',
            period_from='2026-01-02T00:00:00Z',period_until='2026-02-01T00:00:00Z',recorded_through='2200-01-01T00:00:00Z',
            mapping_digest=cap['mapping_digest'],reporting_timezone='UTC',correction_recognition='recorded_time',collection='rows',limit=100)
        request.write_text(json.dumps(args));durations=[];identity=None
        for _ in range(3):
            start=time.perf_counter_ns()
            run=subprocess.run([str(cli),'--profile',str(profile),'pnl','query','--request',str(request)],capture_output=True,text=True,timeout=60)
            durations.append((time.perf_counter_ns()-start)//1000000);doc=json.loads(run.stdout)
            if run.returncode:
                assert doc['error']['code']=='PERIOD_REPLAY_LIMIT_EXCEEDED',(run.stdout,run.stderr)
            else:
                assert doc['data']['manifest']['supported_complete']
                current=doc['data']['manifest']['report_id']
                assert identity is None or identity==current
                identity=current
        print(json.dumps(dict(records=count,settlements=0,distinct_trade_boundaries=count,collection='rows',
            outcome='available' if doc['ok'] else doc['error']['code'],cli_full_replay_ms=durations,median_ms=statistics.median(durations),
            response_bytes=len(run.stdout.encode()),stable_report_identity=identity,
            limits=dict(max_boundaries=512,max_replay_work=200000),measurement='process startup, immutable SQLite prefix reads, complete pure boundary replay and row selection, evidence/hash and first-page serialization; excludes ingestion setup')),flush=True)
