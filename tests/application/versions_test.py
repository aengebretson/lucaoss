"""Reviewed immutable-version rules through direct C++, CLI and real stdio MCP."""
import concurrent.futures
import copy
import datetime
import hashlib
import json
from pathlib import Path
import sys
import unittest
import reporting_test as reports
from support import Peer, trade
MCP=reports.MCP

def before(stamp):
    head,ns=stamp[:-1].split('.')
    if int(ns):return head+'.'+str(int(ns)-1).zfill(9)+'Z'
    dt=datetime.datetime.fromisoformat(head)-datetime.timedelta(seconds=1)
    return dt.isoformat()+'.999999999Z'

class Versions(unittest.TestCase):
    cli=reports.Reporting.cli
    both=reports.Reporting.both
    submit=reports.Reporting.submit
    prepare_book=reports.Reporting.prepare_book
    inputs=reports.Reporting.inputs
    report=reports.Reporting.report
    tearDown=reports.Reporting.tearDown
    def setUp(self):
        reports.Reporting.setUp(self)
        self.admin('register-context','versions',request={'adapter':'luca.transactions.envelope.jsonl/1','normalization':'source-book-identity/1','rules':'cash-equity/2','engine':'luca/0.1','reference_snapshot':'fixture-ref'})
    def edit(self,key='edit',original=None,event=None,void_at='2026-01-20T00:00:00Z'):
        return dict(book_id='fixture-book',idempotency_key=key,original_record_id=original or self.buy,
            validation_context_id='versions',void_at=void_at,reason='Correct synthetic quantity',
            replacement_event_utf8=json.dumps(trade(quantity='11')['event']) if event is None else event)
    def query(self,p,through='4',effective='2026-01-22T00:00:00Z',**extra):
        args=self.inputs(p);args.pop('policy_through');args.pop('settlement_through')
        args.update(accepted_through=through,effective_at=effective,**extra)
        return self.both(p,'query_transactions','transactions query',args)['data']
    def test_atomic_edit_receipt_restart_history_and_historical_reports(self):
        p=Peer(MCP,self.profile)
        self.prepare_book(p)
        request=self.edit()
        receipt=self.both(p,'edit_transaction','transactions edit',request)['data']
        status=p.call('import_status',dict(book_id='fixture-book',import_id=receipt['import_id']))['data']
        self.assertEqual(status['enumeration']['state'],'pending')
        p.kill()
        self.admin('worker','--mode','drain')
        with Peer(MCP,self.profile) as p:
            self.assertEqual(self.both(p,'edit_transaction','transactions edit',request)['data'],receipt)
            status=self.both(p,'import_status','import status',dict(book_id='fixture-book',import_id=receipt['import_id']))['data']
            self.assertTrue(status['all_records_committed']);self.assertEqual(status['commitment_record_counts']['committed'],2)
            rows=self.query(p)['items']
            original=next(v for v in rows if v['record_id']==self.buy)['transaction']
            replacement=next(v for v in rows if v['transaction']['activation_effective_at'] is not None)
            self.assertEqual(original['status'],'voided')
            self.assertEqual(original['replacement_version_id'],replacement['record_id'])
            self.assertEqual(replacement['transaction']['activation_effective_at'],'2026-01-20T00:00:00.000000000Z')
            self.assertIsNone(replacement['transaction']['void_effective_at'])
            pending=self.query(p,effective='2026-01-19T23:59:59.999999999Z',filters={'status':'pending_activation'})['items']
            self.assertEqual([v['record_id'] for v in pending],[replacement['record_id']])
            self.assertEqual(len(self.query(p,effective='2026-01-19T23:59:59.999999999Z',filters={'status':'active'})['items']),2)
            known=self.query(p,recorded_through=before(replacement['transaction']['recorded_at']))['items']
            self.assertEqual(len(known),2);self.assertTrue(all(v['transaction']['status']=='active' for v in known))
            args=self.inputs(p);args['accepted_through']='4'
            prior=self.report(p,{**args,'effective_at':'2026-01-19T23:59:59.999999999Z'},'obligations')
            after=self.report(p,{**args,'effective_at':'2026-01-20T00:00:00Z'},'obligations')
            self.assertEqual(next(v for v in prior['items'] if v['record_id']==self.buy)['contractual_cash']['amount'],'-1000.000000')
            self.assertEqual(next(v for v in after['items'] if v['record_id']==replacement['record_id'])['contractual_cash']['amount'],'-1100.000000')
            self.assertEqual(after['manifest']['input']['policy_selection'],'explicit_sequence')
            self.assertEqual(len(after['manifest']['input']['policy_knowledge']),1)
            self.both(p,'accounting_report','report',{**args,'accepted_through':'3','collection':'journals'},2)
            evidence=self.both(p,'transaction_evidence','transactions evidence',dict(book_id='fixture-book',record_id=replacement['record_id']))['data']
            source=json.loads(evidence['payload_utf8'])['source']
            self.assertEqual(source['actor'],'local-profile');self.assertEqual(source['reason'],request['reason'])
            self.assertEqual(source['operation'],'edit_transaction');self.assertEqual(source['original_record_id'],self.buy)
            self.assertEqual(hashlib.sha256(evidence['payload_utf8'].encode()).hexdigest(),evidence['locator']['sha256'])
            self.assertEqual(len(evidence['payload_utf8'].encode()),int(evidence['locator']['byte_length']))
            late=dict(book_id='fixture-book',idempotency_key='late-before-gate',request=dict(account='fixture-account',trade_record=self.buy,cash='-400',currency='USD',effective_at='2026-01-15T00:00:00Z',source=dict(source_id='fixture-source',reference='Late synthetic bank evidence')))
            self.both(p,'submit_settlement','settlement submit',late);self.admin('worker','--mode','drain')
            late_status=self.both(p,'settlement_status','settlement status',dict(book_id='fixture-book',idempotency_key='late-before-gate'))['data']
            self.assertEqual(late_status['state'],'committed',late_status)
            before_cash=self.report(p,{**args,'settlement_through':'1','effective_at':'2026-01-19T23:59:59Z'},'obligations')
            after_cash=self.report(p,{**args,'settlement_through':'1','effective_at':'2026-01-20T00:00:00Z'},'obligations')
            self.assertEqual(next(v for v in before_cash['items'] if v['record_id']==self.buy)['outstanding_cash']['amount'],'-600.000000')
            self.assertEqual(next(v for v in after_cash['items'] if v['record_id']==replacement['record_id'])['outstanding_cash']['amount'],'-700.000000')
        with Peer(MCP,self.profile) as p:self.assertEqual(self.report(p,{**args,'effective_at':'2026-01-20T00:00:00Z'},'obligations'),after)
    def test_invalid_replacement_and_stale_edit_leave_original_history(self):
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            for i,event in enumerate(['not JSON',json.dumps({'type':'equity_trade','quantity':'NaN'}),json.dumps({**trade()['event'],'instrument_id':'MISSING'}),json.dumps(trade()['event'])[:-1]+',"quantity":"12"}']):
                req=self.edit('invalid-'+str(i),event=event)
                receipt=self.both(p,'edit_transaction','transactions edit',req)['data']
                self.admin('worker','--mode','drain')
                scope=dict(book_id='fixture-book',import_id=receipt['import_id'])
                state=self.both(p,'import_status','import status',scope)['data']
                self.assertFalse(state['all_records_committed']);self.assertEqual(state['commitment_record_counts']['committed'],0)
                self.assertTrue(self.both(p,'read_diagnostics','import errors',scope)['data']['items'])
                self.assertEqual(p.call('capabilities',{'book_id':'fixture-book'})['data']['ledger_state']['last_sequence'],'2')
            req=self.edit();receipt=p.call('edit_transaction',req)['data'];self.admin('worker','--mode','drain')
            changed={**req,'reason':'different'}
            self.both(p,'edit_transaction','transactions edit',changed,4)
            stale=p.call('edit_transaction',self.edit('stale',void_at='2026-01-21T00:00:00Z'))['data']
            self.admin('worker','--mode','drain')
            self.assertFalse(p.call('import_status',dict(book_id='fixture-book',import_id=stale['import_id']))['data']['all_records_committed'])
            self.assertEqual(p.call('capabilities',{'book_id':'fixture-book'})['data']['ledger_state']['last_sequence'],'4')
    def test_delete_retains_own_void_bank_cash_and_refund(self):
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            settlement=dict(book_id='fixture-book',idempotency_key='payment',request=dict(account='fixture-account',trade_record=self.buy,cash='-400',currency='USD',effective_at='2026-01-15T00:00:00Z',source=dict(source_id='fixture-source',reference='synthetic bank')))
            p.call('submit_settlement',settlement);self.admin('worker','--mode','drain')
            p.call('edit_transaction',self.edit());self.admin('worker','--mode','drain')
            replacement=next(v for v in self.query(p)['items'] if v['transaction']['activation_effective_at'] is not None)
            deletion=self.edit('delete',replacement['record_id'],void_at='2026-01-25T00:00:00Z');deletion.pop('replacement_event_utf8');deletion['reason']='Delete synthetic duplicate'
            receipt=self.both(p,'delete_transaction','transactions delete',deletion)['data'];self.admin('worker','--mode','drain')
            self.assertEqual(self.both(p,'delete_transaction','transactions delete',deletion)['data'],receipt)
            rows=self.query(p,through='5',effective='2026-01-26T00:00:00Z')['items']
            original=next(v for v in rows if v['record_id']==self.buy)['transaction']
            replaced=next(v for v in rows if v['record_id']==replacement['record_id'])['transaction']
            self.assertEqual(original['void_effective_at'],'2026-01-20T00:00:00.000000000Z')
            self.assertEqual(replaced['void_effective_at'],'2026-01-25T00:00:00.000000000Z')
            self.assertEqual(replaced['status'],'voided')
            deleted=rows[-1]['record_id']
            evidence=self.both(p,'transaction_evidence','transactions evidence',dict(book_id='fixture-book',record_id=deleted))['data']
            self.assertEqual(json.loads(evidence['payload_utf8'])['source']['operation'],'delete_transaction')
            args={**self.inputs(p),'accepted_through':'5','settlement_through':'1'}
            report=self.report(p,args,'actual_settlements');self.assertEqual(len(report['items']),1)
            self.assertEqual(report['items'][0]['cash'],'-400')
            self.assertTrue(any(v['code']=='SETTLEMENT_ON_VOIDED_TRADE' for v in report['manifest']['breaks']))
            refund=copy.deepcopy(settlement);refund['idempotency_key']='refund';refund['request'].update(cash='400',reverses='payment',effective_at='2026-01-26T00:00:00Z')
            self.both(p,'submit_refund','settlement refund',refund);self.admin('worker','--mode','drain')
            cash=self.report(p,{**args,'settlement_through':'2'},'actual_settlements')
            self.assertEqual(len(cash['items']),2)
    def test_legacy_undated_void_and_native_source_metadata(self):
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            cancel=dict(record_id='legacy-cancel',economic_event_id='buy-event',account_id='fixture-account',action='cancel',causal_record_id='buy')
            p.call('submit_import',self.submit('legacy',json.dumps(cancel)+'\n'));self.admin('worker','--mode','drain')
            rows=self.query(p,through='3')['items']
            original=next(v for v in rows if v['record_id']==self.buy)['transaction']
            self.assertEqual(original['status'],'voided');self.assertIsNotNone(original['void_version_id'])
            self.assertIsNone(original['void_effective_at']);self.assertIsNone(original['void_at'])
            meta=self.submit()['metadata'];meta['replaces']={'import_id':rows[0]['admission']['import_id'],'record_ids':['synthetic-prior-source-record']}
            metadata_file=self.directory/'native-metadata.json';metadata_file.write_text(json.dumps(meta))
            payload_file=self.directory/'native-input.jsonl';payload_file.write_text(json.dumps(trade('meta','meta-event','1'))+'\n')
            self.admin('receive','--key','native-metadata','--metadata',str(metadata_file),'--payload',str(payload_file));self.admin('worker','--mode','drain')
            added=self.query(p,through='4')['items'][-1]
            evidence=self.both(p,'transaction_evidence','transactions evidence',dict(book_id='fixture-book',record_id=added['record_id']))['data']
            self.assertEqual(evidence['source_metadata']['replaces'],meta['replaces'])
    def test_concurrent_retry_and_permissions(self):
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                receipts=list(pool.map(lambda _:self.cli('transactions edit',self.edit())['data'],range(8)))
            self.assertTrue(all(v==receipts[0] for v in receipts));self.admin('worker','--mode','drain')
            self.assertEqual(p.call('capabilities',{'book_id':'fixture-book'})['data']['ledger_state']['last_sequence'],'4')
            self.both(p,'edit_transaction','transactions edit',{**self.edit(),'book_id':'other-book'},3)
            for label,updates in [('readonly',{'submit':False}),('source',{'submit_sources':['other-source']}),('account',{'accounts':[]})]:
                binding={**self.binding,**updates};profile=self.directory/(label+'.json');profile.write_text(json.dumps({'books':[binding]}))
                with Peer(MCP,profile) as restricted:
                    if label=='readonly':
                        names=[v['name'] for v in restricted.request('tools/list')['result']['tools']]
                        self.assertNotIn('luca_edit_transaction',names);self.assertNotIn('luca_delete_transaction',names)
                    self.assertEqual(self.cli('transactions edit',self.edit(),3,profile)['error']['exit_code'],3)
            self.both(p,'transaction_evidence','transactions evidence',dict(book_id='fixture-book',record_id='missing'),4)

if __name__=='__main__':unittest.main()
