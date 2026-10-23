"""Reviewed period P&L through direct C++, CLI and an initialized stdio MCP peer."""
import concurrent.futures
import copy
import datetime
import json
import sqlite3
import unittest
import reporting_test as reports
from support import Peer, trade

class Pnl(unittest.TestCase):
    setUp=reports.Reporting.setUp
    tearDown=reports.Reporting.tearDown
    cli=reports.Reporting.cli
    both=reports.Reporting.both
    submit=reports.Reporting.submit
    prepare_book=reports.Reporting.prepare_book
    settlement=reports.Reporting.settlement
    def args(self,p,**extra):
        a=reports.Reporting.inputs(self,p);a.pop('effective_at')
        a.update(period_from='2026-01-02T00:00:00Z',period_until='2026-02-01T00:00:00Z',
                 collection='rows',reporting_timezone='UTC',correction_recognition='recorded_time',**extra)
        return a
    def pnl(self,p,a,expected=0):
        return self.both(p,'query_pnl','pnl query',a,expected)
    def test_period_balances_daily_evidence_exports_and_reproduction(self):
        with Peer(reports.MCP,self.profile) as p:
            self.prepare_book(p);a=self.args(p)
            result=self.pnl(p,a)['data'];m=result['manifest']
            self.assertEqual(m['totals'][0]['activity']['economic_realized']['amount'],'30.000000')
            self.assertEqual(m['totals'][0]['opening']['recognized_realized']['amount'],'0.000000')
            self.assertEqual(m['totals'][0]['closing']['recognized_realized']['amount'],'30.000000')
            self.assertFalse(m['retention_available']);self.assertTrue(m['supported_complete'])
            self.assertIsNone(m['unrealized_pnl']);self.assertEqual(m['coverage']['staging_completeness'],'unknown')
            daily=self.pnl(p,{**a,'collection':'daily'})['data']
            self.assertEqual(daily['items'][0]['day'],'2026-01-10')
            self.assertEqual(daily['items'][0]['activity'],m['totals'][0]['activity'])
            self.assertEqual(daily['manifest']['report_id'],m['report_id'])
            evidence=self.pnl(p,{**a,'collection':'evidence'})['data']
            self.assertEqual([v['kind'] for v in evidence['items']],['ledger','ledger','policy'])
            self.assertEqual(evidence['items'][0]['ledger']['record_id'],self.buy)
            self.assertIn('canonical',evidence['items'][0]['ledger'])
            self.assertEqual(evidence['manifest']['input'],m['input'])
            exported=self.pnl(p,{**a,'collection':'evidence','format':'jsonl','limit':1})['data']
            rows=[]
            while True:
                chunks=[json.loads(v) for v in exported['export']['content'].splitlines()]
                rows.extend(v['row'] for v in chunks if v['kind']=='row')
                if not exported['has_more']:break
                exported=self.pnl(p,{**a,'collection':'evidence','format':'jsonl','limit':1,'cursor':exported['next_cursor']})['data']
            self.assertEqual(rows,evidence['items'])
            self.pnl(p,{**a,'cursor':self.pnl(p,{**a,'collection':'evidence','limit':1})['data']['next_cursor']},4)
            p.call('submit_import',self.submit('later',json.dumps(trade('later','later-event','1'))+'\n'))
            self.admin('worker','--mode','drain')
            self.assertEqual(self.pnl(p,a)['data'],result)
        with Peer(reports.MCP,self.profile) as p:self.assertEqual(self.pnl(p,a)['data'],result)
    def test_full_pool_filters_totals_before_paging_and_scope(self):
        self.admin('register-reference','extra-ref',request={'accounts':['fixture-account'],'instruments':['ABC','XYZ']})
        self.admin('register-context','extra',request={'adapter':'luca.transactions.jsonl/1','normalization':'source-book-identity/1','rules':'cash-equity/2','engine':'luca/0.1','reference_snapshot':'extra-ref'})
        with Peer(reports.MCP,self.profile) as p:
            rows=[trade(),trade('sell','sell-event','-3','110'),trade('x-buy','x-buy-event'),trade('x-sell','x-sell-event','-3','110')]
            for row in rows[2:]:row['event']['instrument_id']='XYZ'
            submission=self.submit(body=''.join(json.dumps(v)+'\n' for v in rows));submission['metadata']['validation_context_id']='extra'
            p.call('submit_import',submission);self.admin('worker','--mode','drain')
            a=self.args(p,accepted_through='4',limit=1)
            first=self.pnl(p,a)['data'];self.assertTrue(first['has_more'])
            self.assertEqual(first['manifest']['totals'][0]['activity']['economic_realized']['amount'],'60.000000')
            second=self.pnl(p,{**a,'cursor':first['next_cursor']})['data']
            self.assertEqual(first['manifest'],second['manifest'])
            filtered=self.pnl(p,{**a,'instrument':'ABC','account':'fixture-account'})['data']
            self.assertEqual(filtered['manifest']['totals'][0]['activity']['economic_realized']['amount'],'30.000000')
            self.assertEqual(filtered['manifest']['report_id'],first['manifest']['report_id'])
            self.assertEqual(self.pnl(p,{**a,'account':'absent'})['data']['items'],[])
            self.pnl(p,{**a,'collection':'daily','instrument':'ABC'},2)
            self.pnl(p,{**a,'collection':'evidence','account':'fixture-account'},2)
            self.pnl(p,{**a,'book_id':'other-book'},3)
            self.pnl(p,{**a,'mapping_digest':'0'*64},4)
            readonly=copy.deepcopy(self.binding);readonly['submit']=False;readonly['submit_sources']=[]
            profile=self.directory/'readonly.json';profile.write_text(json.dumps({'books':[readonly]}))
            self.assertEqual(self.cli('pnl query',a,profile=profile)['data'],first)
            denied=copy.deepcopy(readonly);denied['inspect']=False
            profile=self.directory/'denied.json';profile.write_text(json.dumps({'books':[denied]}))
            self.assertEqual(self.cli('pnl query',a,3,profile=profile)['error']['code'],'PERMISSION_DENIED')
    def test_partial_cash_refund_completeness_knowledge_and_retained_errors(self):
        self.admin('policy-put',request={'revision':{'id':'cash','scope':'legal_entity','subject':'fixture-entity','legal_entity':'fixture-entity','effective_from':'2026-01-01T00:00:00Z','lot_selection':'LIFO','recognition':'cash'},'expected_sequence':1,'actor':'fixture-admin'})
        with Peer(reports.MCP,self.profile) as p:
            self.prepare_book(p);a=self.args(p,policy_through='2')
            no_cash=self.pnl(p,a)['data'];total=no_cash['manifest']['totals'][0]
            self.assertEqual(total['activity']['economic_realized']['amount'],'30.000000')
            self.assertEqual(total['activity']['recognized_realized']['amount'],'0.000000')
            self.assertEqual(total['activity']['deferred_realized']['amount'],'30.000000')
            self.assertFalse(no_cash['manifest']['supported_complete'])
            p.call('submit_settlement',self.settlement(cash='-400'));self.admin('worker','--mode','drain')
            close=self.settlement('close','330');close['request']['trade_record']=p.call('transactions',dict(book_id='fixture-book',accepted_through='2'))['data']['items'][1]['record_id']
            p.call('submit_settlement',close);self.admin('worker','--mode','drain')
            partial=self.pnl(p,{**a,'settlement_through':'2'})['data']
            self.assertEqual(partial['manifest']['totals'][0]['activity']['recognized_realized']['amount'],'12.000000')
            evidence=self.pnl(p,{**a,'settlement_through':'2','collection':'evidence'})['data']
            self.assertEqual(len([v for v in evidence['items'] if v['kind']=='settlement']),2)
            refund=self.settlement('refund','400','payment');p.call('submit_refund',refund);self.admin('worker','--mode','drain')
            after=self.pnl(p,{**a,'settlement_through':'3'})['data']
            self.assertEqual(after['manifest']['totals'][0]['activity']['recognized_realized']['amount'],'0.000000')
            invalid=self.settlement('bad-refund','1','payment');p.call('submit_refund',invalid);self.admin('worker','--mode','drain')
            self.assertEqual(p.call('settlement_status',dict(book_id='fixture-book',idempotency_key='bad-refund'))['data']['state'],'invalid')
            self.assertEqual(self.pnl(p,{**a,'settlement_through':'3'})['data'],after)
            just_before=dict(a,recorded_through='2020-01-01T00:00:00Z')
            empty=self.pnl(p,just_before,2)
            self.assertEqual(empty['error']['code'],'ENTITY_POLICY_REQUIRED')
            bad=copy.deepcopy(after);bad['manifest']['totals'][0]['activity']['economic_realized']['amount']=1.5
            with self.assertRaises(AssertionError):reports.foundations.validate({'api_version':'luca.foundations/1','operation':'query_pnl','ok':True,'data':bad,'error':None},self.catalog['luca_query_pnl']['outputSchema'])
        with Peer(reports.MCP,self.profile) as p:self.assertEqual(self.pnl(p,{**a,'settlement_through':'3'})['data'],after)
    def test_version_recognition_is_recorded_activity_and_void_gate(self):
        self.admin('register-context','versions',request={'adapter':'luca.transactions.envelope.jsonl/1','normalization':'source-book-identity/1','rules':'cash-equity/2','engine':'luca/0.1','reference_snapshot':'fixture-ref'})
        with Peer(reports.MCP,self.profile) as p:
            self.prepare_book(p)
            event=trade(quantity='11')['event'];event['price']='90'
            edit=dict(book_id='fixture-book',idempotency_key='edit',original_record_id=self.buy,validation_context_id='versions',void_at='2027-01-10T00:00:00Z',reason='Synthetic correction',replacement_event_utf8=json.dumps(event))
            p.call('edit_transaction',edit);self.admin('worker','--mode','drain')
            a=self.args(p,accepted_through='4')
            jan=self.pnl(p,a)['data'];self.assertEqual(jan['manifest']['totals'][0]['activity']['economic_realized']['amount'],'30.000000')
            gate=self.pnl(p,{**a,'period_from':'2027-01-10T00:00:00Z','period_until':'2027-01-11T00:00:00Z'})['data']
            self.assertEqual(gate['manifest']['totals'][0]['opening']['economic_realized']['amount'],'30.000000')
            self.assertEqual(gate['manifest']['totals'][0]['activity']['economic_realized']['amount'],'30.000000')
            self.assertEqual(gate['manifest']['totals'][0]['closing']['economic_realized']['amount'],'60.000000')
            self.pnl(p,{**a,'accepted_through':'3'},2)
            history=p.call('policy_history',dict(book_id='fixture-book'))['data']['knowledge'][0]['recorded_at']
            known=self.pnl(p,{**a,'recorded_through':history})['data']
            self.assertEqual(known['items'],[])
    def test_diagnostics_before_filters_and_concurrent_native_processing(self):
        with Peer(reports.MCP,self.profile) as p:
            self.prepare_book(p);a=self.args(p);original=self.pnl(p,a)['data']
            unknown=trade('unknown','unknown-event','1');unknown['event'].pop('charges')
            receipt=p.call('submit_import',self.submit('unknown',json.dumps(unknown)+'\n'))['data']
            with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
                processing=pool.submit(self.admin,'worker','--mode','drain')
                readers=[pool.submit(self.cli,'pnl query',a) for _ in range(2)]
                self.assertEqual(p.call('query_pnl',a)['data'],original)
                self.assertEqual([v.result()['data'] for v in readers],[original,original])
                processing.result()
            self.assertTrue(p.call('import_status',dict(book_id='fixture-book',import_id=receipt['import_id']))['data']['all_records_committed'])
            incomplete=self.pnl(p,{**a,'accepted_through':'3','instrument':'ABSENT'})['data']
            self.assertEqual(incomplete['items'],[])
            self.assertFalse(incomplete['manifest']['supported_complete'])
            self.assertIn('CHARGES_PROVISIONAL',[v['code'] for v in incomplete['manifest']['diagnostics']])
    def test_replay_work_limit_fails_without_partial_result(self):
        rows=[]
        for i in range(450):
            row=trade('b'+str(i),'be'+str(i),'1' if i%2==0 else '-1')
            row['event']['effective_at']=(datetime.datetime(2026,1,10)+datetime.timedelta(seconds=i)).isoformat()+'Z'
            row['atomic_group']='bounded-group';rows.append(row)
        payload=self.directory/'bounded.jsonl';payload.write_text(''.join(json.dumps(v)+'\n' for v in rows))
        metadata=self.directory/'bounded-meta.json';metadata.write_text(json.dumps(self.submit()['metadata']))
        receipt=self.admin('receive','--key','bounded','--metadata',str(metadata),'--payload',str(payload))
        self.admin('worker','--mode','drain')
        self.assertTrue(self.admin('status',receipt['import_id'])['all_records_committed'])
        with Peer(reports.MCP,self.profile) as p:
            rejected=self.pnl(p,self.args(p,accepted_through='450'),2)
            self.assertIsNone(rejected['data'])
            self.assertEqual(rejected['error']['code'],'PERIOD_REPLAY_LIMIT_EXCEEDED')
    def test_bounds_unknown_policy_and_closed_retention_fences(self):
        with Peer(reports.MCP,self.profile) as p:
            self.prepare_book(p);a=self.args(p)
            for updates,code in [({'accepted_through':'1001'},'PERIOD_REPLAY_LIMIT_EXCEEDED'),({'settlement_through':'1001'},'PERIOD_REPLAY_LIMIT_EXCEEDED'),({'policy_through':'257'},'PERIOD_REPLAY_LIMIT_EXCEEDED'),({'period_until':a['period_from']},'INVALID_PERIOD_CONTEXT')]:
                self.assertEqual(self.pnl(p,{**a,**updates},2)['error']['code'],code)
            self.pnl(p,{**a,'reporting_timezone':'America/Chicago'},2)
            self.pnl(p,{**a,'correction_recognition':'effective_time'},2)
            caps=p.call('capabilities',dict(book_id='fixture-book'))['data'];self.assertFalse(caps['period_retention_available'])
        # Simulates a migrated schema-5 policy's documented missing knowledge input
        # in this disposable book; no production database or financial rule changes.
        database=next((self.directory/'store').rglob('*.sqlite3'))
        with sqlite3.connect(database) as db:db.execute('UPDATE policy_revisions SET recorded_ns=NULL')
        with Peer(reports.MCP,self.profile) as p:self.assertEqual(self.pnl(p,a,4)['error']['code'],'POLICY_KNOWLEDGE_UNAVAILABLE')

if __name__=='__main__':unittest.main()
