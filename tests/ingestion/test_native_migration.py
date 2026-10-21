"""Only test fixtures use Python; each operation under test invokes the C++ CLI."""
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

FIXTURE=Path(__file__).parent/'legacy_reference'
sys.path.insert(0,str(FIXTURE))
from luca_ingestion import IntakeStore
CLI=os.environ.get('LUCA_INGESTION_CLI','')
CONFIG=json.loads((FIXTURE/'example-config.json').read_text())
META={'source_id':'custodian-a','format':'luca.transactions.jsonl/1','validation_context_id':'cash-equity-v1-ref42'}
CASH={'record_id':'r','economic_event_id':'e','action':'originate','account_id':'portfolio-a',
      'event':{'type':'cash_movement','effective_at':'2026-10-20T00:00:00Z','currency':'USD','amount':'100'}}

@unittest.skipUnless(CLI,'Build native luca-ingest and set LUCA_INGESTION_CLI')
class NativeMigrationTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.root=Path(self.tmp.name)
        self.config=self.root/'config.json'; self.config.write_text(json.dumps(CONFIG))
        with patch('luca_ingestion.store._patched_wal',return_value=False):
            self.legacy=IntakeStore(self.root/'data',**CONFIG)
    def tearDown(self): self.tmp.cleanup()
    def command(self,*args,ok=True):
        result=subprocess.run([CLI,'--root',str(self.root/'data'),'--config',str(self.config),*map(str,args)],capture_output=True,text=True,timeout=30)
        if ok:
            self.assertEqual(result.returncode,0,result.stderr); return json.loads(result.stdout)
        self.assertNotEqual(result.returncode,0); return json.loads(result.stderr)
    def write(self,name,value):
        path=self.root/name; path.write_text(json.dumps(value)); return path
    def test_receipt_keys_bytes_and_pending_work_survive_native_migration(self):
        body=(json.dumps(CASH)+'\n').encode(); old=self.legacy.receive(META,'key',io.BytesIO(body),len(body))
        current=self.command('status',old['import_id']); self.assertEqual(current['receipt'],old)
        refs=self.write('ref.json',{'accounts':['portfolio-a'],'instruments':[]})
        self.command('register-reference','ref42','--definition',refs)
        self.command('worker','--mode','drain')
        self.assertTrue(self.command('status',old['import_id'])['all_records_committed'])
        payload=self.root/'payload.jsonl'; payload.write_bytes(body)
        metadata=self.write('meta.json',META)
        self.assertEqual(self.command('receive','--metadata',metadata,'--key','key','--payload',payload),old)
        retained=self.root/'retained'; self.command('payload',old['import_id'],'--output',retained)
        self.assertEqual(retained.read_bytes(),body)
        self.assertEqual(self.command('ledger-state')['last_sequence'],1)
        self.assertFalse(self.command('capabilities')['python_required'])
    def test_partial_legacy_upload_resumes_natively(self):
        import hashlib
        body=b'abcdefgh'; digest=hashlib.sha256(body).hexdigest()
        upload=self.legacy.create_upload('upload',expected_bytes=8,sha256=digest,chunk_bytes=4)['upload_id']
        self.legacy.put_chunk(upload,0,b'abcd',hashlib.sha256(b'abcd').hexdigest())
        chunk=self.root/'chunk'; chunk.write_bytes(b'efgh')
        self.command('upload-put',upload,'--index','1','--payload',chunk,'--sha256',hashlib.sha256(b'efgh').hexdigest())
        self.assertEqual(self.command('upload-verify',upload)['state'],'ready')
        receipt=self.command('receive-upload',upload,'--metadata',self.write('meta.json',META),'--key','attached')
        retained=self.root/'retained'; self.command('payload',receipt['import_id'],'--output',retained)
        self.assertEqual(retained.read_bytes(),body)
        self.command('worker','--mode','drain')
        self.assertEqual(self.command('status',receipt['import_id'])['validation_counts']['invalid'],1)
    def test_native_cli_rejects_unknown_scope_and_preserves_failed_receipt(self):
        metadata=self.write('bad.json',dict(META,source_id='forbidden'))
        payload=self.root/'bad'; payload.write_bytes(b'invalid')
        self.assertEqual(self.command('receive','--metadata',metadata,'--key','no','--payload',payload,ok=False)['code'],'SOURCE_NOT_AUTHORIZED')
        self.assertEqual(self.command('capabilities')['usage']['slots'],0)
