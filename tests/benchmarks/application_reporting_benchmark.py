"""Disposable native receipt/admission setup; measure complete C++ application report calls.
Usage: python3 -B tests/benchmarks/application_reporting_benchmark.py BUILD [100 1000 10000]
No production paths, financial floating point or product Python runtime.
"""
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
cli=build/'libs/application/luca'; ingest=build/'libs/ingestion/luca-ingest'
counts=list(map(int,sys.argv[2:])) or [100,1000,5000,10000]
for count in counts:
    assert 2<=count<=10000 and count%2==0
    with tempfile.TemporaryDirectory(prefix='luca-report-benchmark-') as temp:
        root=Path(temp)
        profile,binding,admin=prepare(root,ingest)
        payload=root/'input.jsonl'; meta=root/'metadata.json'
        rows=[]
        for i in range(count):
            tx=trade('r'+str(i),'e'+str(i),'1' if i%2==0 else '-1','100' if i%2==0 else '110')
            tx['atomic_group']='group-'+str(i//1000)
            rows.append(tx)
        payload.write_text(''.join(json.dumps(tx)+'\n' for tx in rows))
        meta.write_text(json.dumps(metadata()))
        receipt=admin('receive','--key','benchmark','--metadata',str(meta),'--payload',str(payload))
        # Native setup is excluded from reporting measurements.
        subprocess.run([str(ingest),'--root',binding['root'],'--config',str(root/'store-config.json'),'worker','--mode','drain'],check=True,capture_output=True,text=True,timeout=600)
        state=admin('status',receipt['import_id']);assert state['all_records_committed'],state
        request=root/'request.json';request.write_text(json.dumps({'book_id':'fixture-book'}))
        cap=json.loads(subprocess.check_output([str(cli),'--profile',str(profile),'capabilities','--request',str(request)]))['data']
        args=dict(book_id='fixture-book',accepted_through=str(count),policy_through='1',settlement_through='0',
            effective_at='2026-02-01T00:00:00Z',recorded_through='2200-01-01T00:00:00Z',mapping_digest=cap['mapping_digest'],collection='journals',limit=100)
        request.write_text(json.dumps(args))
        durations=[]
        for _ in range(3):
            start=time.perf_counter_ns()
            run=subprocess.run([str(cli),'--profile',str(profile),'report','--request',str(request)],capture_output=True,text=True,timeout=60)
            durations.append((time.perf_counter_ns()-start)//1000000)
            document=json.loads(run.stdout)
            if run.returncode:
                assert document['error']['code']=='PROJECTION_LIMIT_EXCEEDED',(run.stdout,run.stderr)
                report=None
            else:
                report=document['data']
                assert report['manifest']['charges_complete']
        print(json.dumps(dict(records=count,settlements=0,collection='journals',page_size=100,
            atomic_group_size=min(count,1000),outcome='available' if report else 'PROJECTION_LIMIT_EXCEEDED',total_rows=report['total_rows'] if report else None,cli_full_replay_ms=durations,median_ms=statistics.median(durations),
            response_bytes=len(run.stdout.encode()),measurement='process startup, SQLite immutable-prefix reads, pure projection, full row serialization/hash, first page; no ingestion setup')),flush=True)
