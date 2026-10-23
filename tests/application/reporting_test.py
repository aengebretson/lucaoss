"""Direct C++ embedding, CLI, and a real stdio MCP peer use disposable books."""
import concurrent.futures
import copy
import json
from pathlib import Path
import subprocess
import sys
import time
import unittest

import adapters_test as foundations
CLI, MCP, INGEST, CATALOG = foundations.CLI, foundations.MCP, foundations.INGEST, foundations.CATALOG
from support import Peer, trade
DIRECT = Path(sys.argv.pop(1))

class Reporting(unittest.TestCase):
    setUp = foundations.Adapters.setUp
    tearDown = foundations.Adapters.tearDown
    cli = foundations.Adapters.cli
    submit = foundations.Adapters.submit
    def both(self, peer, name, command, args, expected=0):
        result = self.cli(command,args,expected)
        self.assertEqual(peer.call(name,args),result)
        foundations.validate(result,self.catalog['luca_'+name]['outputSchema'])
        path = self.directory / 'direct-request.json'
        path.write_text(json.dumps(args))
        run = subprocess.run([str(DIRECT),str(self.profile),name,str(path)],text=True,capture_output=True,timeout=30)
        self.assertEqual(run.returncode,expected,(run.stdout,run.stderr))
        self.assertEqual(json.loads(run.stdout),result)
        return result
    def inputs(self,p):
        return dict(book_id='fixture-book',accepted_through='2',policy_through='1',settlement_through='0',
            effective_at='2026-02-01T00:00:00Z',recorded_through='2200-01-01T00:00:00Z',
            mapping_digest=p.call('capabilities',{'book_id':'fixture-book'})['data']['mapping_digest'])
    def settlement(self,key='payment',cash='-400',reverses=None):
        request=dict(account='fixture-account',trade_record=self.buy,cash=cash,currency='USD',effective_at='2026-01-15T00:00:00Z',
                     source=dict(source_id='fixture-source',reference='synthetic bank transfer'))
        if reverses is not None:request['reverses']=reverses
        return dict(book_id='fixture-book',idempotency_key=key,request=request)
    def prepare_book(self,p):
        p.call('submit_import',self.submit())
        self.admin('worker','--mode','drain')
        self.buy=p.call('transactions',dict(book_id='fixture-book',accepted_through='2'))['data']['items'][0]['record_id']
    def report(self,p,args,collection):
        return self.both(p,'accounting_report','report',{**args,'collection':collection})['data']
    def test_reports_filters_pagination_exports_and_reproducibility(self):
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            args=self.inputs(p)
            old={}
            for collection in ['journals','trial_balance','obligations','actual_settlements','reconciliation']:
                r=self.report(p,args,collection);old[collection]=r
                self.assertFalse(r['manifest']['coverage']['staged_inputs_included'])
                self.assertEqual(r['manifest']['coverage']['staging_completeness'],'unknown')
            controls=old['reconciliation']['items']
            self.assertTrue(all(c['difference']['amount']=='0.000000' for c in controls))
            pnl=next(c for c in controls if c['control']=='economic_realized_pnl')
            self.assertEqual(pnl['expected']['amount'],'-30.000000')
            self.assertEqual(old['obligations']['items'][0]['outstanding_cash']['amount'],'-1000.000000')
            first=self.report(p,{**args,'limit':1},'journals')
            self.assertTrue(first['has_more'])
            second=self.report(p,{**args,'cursor':first['next_cursor'],'limit':1},'journals')
            self.assertEqual(first['manifest'],second['manifest'])
            self.assertNotEqual(first['items'],second['items'])
            self.both(p,'accounting_report','report',{**args,'collection':'trial_balance','cursor':first['next_cursor']},4)
            exported=self.report(p,{**args,'format':'jsonl'},'journals')
            lines=[json.loads(line) for line in exported['export']['content'].splitlines()]
            self.assertEqual(lines[0]['manifest'],old['journals']['manifest'])
            self.assertEqual([line['row'] for line in lines if line['kind']=='row'],old['journals']['items'])
            chart=self.report(p,{**args,'chart_account':'income.realized-pnl'},'trial_balance')
            self.assertEqual(chart['items'][0]['net']['amount'],'-30.000000')
            self.assertEqual(chart['manifest']['currency_controls'],old['trial_balance']['manifest']['currency_controls'])
            p.call('submit_settlement',self.settlement())
            self.admin('worker','--mode','drain')
            p.call('submit_import',self.submit('later',json.dumps(trade('later','later-event','1'))+'\n'))
            self.admin('worker','--mode','drain')
            for collection,value in old.items(): self.assertEqual(self.report(p,args,collection),value)
        with Peer(MCP,self.profile) as p:
            self.assertEqual(self.report(p,args,'journals'),old['journals'])
    def test_partial_delayed_settlement_refund_errors_restart_and_retry(self):
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            args=self.inputs(p)
            receipt=self.both(p,'submit_settlement','settlement submit',self.settlement())['data']
            self.assertTrue(receipt['durable_acceptance'])
            pending=self.both(p,'settlement_status','settlement status',dict(book_id='fixture-book',idempotency_key='payment'))['data']
            self.assertEqual(pending['state'],'pending')
        self.admin('worker','--mode','drain')
        with Peer(MCP,self.profile) as p:
            self.assertEqual(self.both(p,'submit_settlement','settlement submit',self.settlement())['data'],receipt)
            status=self.both(p,'settlement_status','settlement status',dict(book_id='fixture-book',idempotency_key='payment'))['data']
            self.assertEqual(status['state'],'committed',status)
            self.assertEqual(status['event']['sequence'],'1')
            invalid_schema={'api_version':'luca.foundations/1','operation':'settlement_status','ok':True,'error':None,'data':copy.deepcopy(status)}
            invalid_schema['data']['event']['sequence']=1
            with self.assertRaises(AssertionError):foundations.validate(invalid_schema,self.catalog['luca_settlement_status']['outputSchema'])
            r=self.report(p,{**args,'settlement_through':'1'},'obligations')
            self.assertEqual(r['items'][0]['outstanding_cash']['amount'],'-600.000000')
            delayed=self.report(p,{**args,'settlement_through':'1','effective_at':'2026-01-14T23:59:59Z'},'actual_settlements')
            self.assertEqual(delayed['items'],[])
            refund=self.settlement('refund','400','payment')
            self.both(p,'submit_refund','settlement refund',refund)
            self.admin('worker','--mode','drain')
            self.assertEqual(self.report(p,{**args,'settlement_through':'2'},'obligations')['items'][0]['outstanding_cash']['amount'],'-1000.000000')
            bad=self.settlement('invalid','1','payment')
            self.both(p,'submit_refund','settlement refund',bad)
            self.admin('worker','--mode','drain')
            invalid=self.both(p,'settlement_status','settlement status',dict(book_id='fixture-book',idempotency_key='invalid'))['data']
            self.assertEqual(invalid['state'],'invalid')
            self.assertEqual(invalid['code'],'INVALID_SETTLEMENT_REVERSAL')
            self.both(p,'submit_settlement','settlement submit',self.settlement(cash='-401'),4)
            self.both(p,'accounting_report','report',{**args,'settlement_through':'3','collection':'journals'},4)
            denied=self.settlement('denied');denied['request']['source']['source_id']='other-source'
            self.both(p,'submit_settlement','settlement submit',denied,3)
            denied=self.settlement('denied');denied['request']['account']='other-account'
            self.both(p,'submit_settlement','settlement submit',denied,3)
        with Peer(MCP,self.profile) as p:
            self.assertEqual(p.call('settlement_status',dict(book_id='fixture-book',idempotency_key='invalid'))['data'],invalid)
    def test_transaction_filters_history_cutoffs_and_scope(self):
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            args=self.inputs(p);args.pop('policy_through');args.pop('settlement_through')
            args.update(period_from='2026-01-10T00:00:00Z',period_to='2026-01-11T00:00:00Z',filters=dict(account='fixture-account',instrument='ABC',source='fixture-source',type='equity_trade',status='active'))
            rows=self.both(p,'query_transactions','transactions query',args)['data']
            self.assertEqual(len(rows['items']),2)
            self.assertEqual(rows['items'][0]['transaction']['record_id'],self.buy)
            self.assertEqual(rows['items'][0]['admission']['context']['rules'],'cash-equity/2')
            empty=self.both(p,'query_transactions','transactions query',{**args,'period_from':'2026-01-11T00:00:00Z','period_to':'2026-01-12T00:00:00Z'})['data']
            self.assertEqual(empty['items'],[])
            empty=self.both(p,'query_transactions','transactions query',{**args,'recorded_through':'2020-01-01T00:00:00Z'})['data']
            self.assertEqual(empty['items'],[])
            self.both(p,'query_transactions','transactions query',{**args,'book_id':'other-book'},3)
            self.both(p,'query_transactions','transactions query',{**args,'filters':{'status':'invalid'}},2)
            self.both(p,'query_transactions','transactions query',{**args,'effective_at':'2026-02-30T00:00:00Z'},2)
            self.both(p,'query_transactions','transactions query',{**args,'accepted_through':'10001'},2)
    def test_cash_recognition_requires_both_legs_actual_settlement(self):
        self.admin('policy-put',request={'revision':{'id':'cash-policy','scope':'legal_entity','subject':'fixture-entity','legal_entity':'fixture-entity','effective_from':'2026-01-01T00:00:00Z','lot_selection':'LIFO','recognition':'cash'},'expected_sequence':1,'actor':'fixture-admin'})
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            args=self.inputs(p);args['policy_through']='2'
            zero=self.report(p,args,'trial_balance')
            self.assertFalse(any(v['chart_account']=='income.realized-pnl' and v['net']['amount']!='0.000000' for v in zero['items']))
            p.call('submit_settlement',self.settlement())
            tx=p.call('transactions',dict(book_id='fixture-book',accepted_through='2'))['data']['items'][1]['record_id']
            close=self.settlement('closing','165');close['request']['trade_record']=tx
            p.call('submit_settlement',close)
            self.admin('worker','--mode','drain')
            partial=self.report(p,{**args,'settlement_through':'2'},'trial_balance')
            income=next(v for v in partial['items'] if v['chart_account']=='income.realized-pnl')
            deferred=next(v for v in partial['items'] if v['chart_account']=='control.deferred-realized-pnl')
            self.assertEqual(income['net']['amount'],'-12.000000')
            self.assertEqual(deferred['net']['amount'],'-18.000000')
            refund=self.settlement('refund','400','payment')
            p.call('submit_refund',refund)
            self.admin('worker','--mode','drain')
            reversed_=self.report(p,{**args,'settlement_through':'3'},'trial_balance')
            self.assertFalse(any(v['chart_account']=='income.realized-pnl' and v['net']['amount']!='0.000000' for v in reversed_['items']))
    def test_dated_void_history_preserves_instrument_and_knowledge(self):
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            base=self.inputs(p);base.pop('policy_through');base.pop('settlement_through')
            before=self.both(p,'query_transactions','transactions query',base)['data']
            cancel=dict(record_id='void-buy',economic_event_id='buy-event',account_id='fixture-account',action='cancel',causal_record_id='buy',void_at='2026-01-20T00:00:00Z')
            p.call('submit_import',self.submit('cancel',json.dumps(cancel)+'\n'))
            self.admin('worker','--mode','drain')
            historical=self.both(p,'query_transactions','transactions query',{**base,'accepted_through':'3','effective_at':'2026-01-19T23:59:59Z','filters':{'instrument':'ABC','status':'active'}})['data']
            self.assertEqual(len(historical['items']),2)
            all_rows=self.both(p,'query_transactions','transactions query',{**base,'accepted_through':'3','filters':{'instrument':'ABC'}})['data']['items']
            self.assertEqual(len(all_rows),3)
            original=next(v for v in all_rows if v['record_id']==self.buy)
            self.assertEqual(original['transaction']['void_at'],'2026-01-20T00:00:00.000000000Z')
            voids=self.both(p,'query_transactions','transactions query',{**base,'accepted_through':'3','filters':{'instrument':'ABC','type':'cancellation'}})['data']['items']
            self.assertEqual(len(voids),1)
            self.assertEqual(voids[0]['transaction']['instrument_id'],'ABC')
            self.assertEqual(self.both(p,'query_transactions','transactions query',base)['data'],before)
    def test_actual_revalidation_context_and_negative_output_schemas(self):
        with Peer(MCP,self.profile) as p:
            tx=trade('retry','retry-event');tx['event']['instrument_id']='DEF'
            receipt=p.call('submit_import',self.submit('retry',json.dumps(tx)+'\n'))['data']
            self.admin('worker','--mode','drain')
            record=p.call('read_records',dict(book_id='fixture-book',import_id=receipt['import_id']))['data']['items'][0]
            group=self.admin('group',receipt['import_id'],record['group_id'])
            self.admin('register-reference','new-ref',request={'accounts':['fixture-account'],'instruments':['ABC','DEF']})
            self.admin('register-context','new-context',request={'adapter':'luca.transactions.jsonl/1','normalization':'source-book-identity/1','rules':'cash-equity/2','engine':'luca/0.1','reference_snapshot':'new-ref'})
            self.admin('revalidate',receipt['import_id'],'--key','new-context',request={'validation_context_id':'new-context','groups':[{'group_id':record['group_id'],'expected_revision':group['revision']}]})
            self.admin('worker','--mode','drain')
            base=self.inputs(p);base['accepted_through']='1';base.pop('policy_through');base.pop('settlement_through')
            value=self.both(p,'query_transactions','transactions query',base)
            row=value['data']['items'][0]
            self.assertEqual(row['validation_context']['reference_snapshot'],'new-ref')
            self.assertEqual(row['original_submission_context']['reference_snapshot'],'fixture-ref')
            self.assertEqual(row['validation_context'],row['admission']['context'])
            schema=self.catalog['luca_query_transactions']['outputSchema']
            bad=copy.deepcopy(value);del bad['data']['items'][0]['transaction']['record_id']
            with self.assertRaises(AssertionError):foundations.validate(bad,schema)
            args={**self.inputs(p),'accepted_through':'1','collection':'journals'}
            value=self.both(p,'accounting_report','report',args)
            schema=self.catalog['luca_accounting_report']['outputSchema']
            for alter in ['amount','input','numeric','coverage','export']:
                bad=copy.deepcopy(value)
                if alter=='amount':del bad['data']['items'][0]['amount']
                elif alter=='input':del bad['data']['manifest']['input']['accepted_through']
                elif alter=='numeric':bad['data']['items'][0]['amount']['amount']=10.0
                elif alter=='coverage':bad['data']['manifest']['coverage']['staged_inputs_included']=True
                else:bad['data']['export']={'media_type':'text/plain','encoding':'UTF-8','content':'wrong'}
                with self.assertRaises(AssertionError):foundations.validate(bad,schema)
    def test_atomic_cutoffs_and_completeness_diagnostics(self):
        with Peer(MCP,self.profile) as p:
            tx=trade();tx['event'].pop('charges')
            other=trade('eur','eur-event','2');other['event']['quote_currency']='EUR'
            req=self.submit('atomic',json.dumps(tx)+'\n'+json.dumps(other)+'\n');req['metadata']['atomicity']='whole_import'
            p.call('submit_import',req);self.admin('worker','--mode','drain')
            args=self.inputs(p)
            self.both(p,'accounting_report','report',{**args,'accepted_through':'1','collection':'journals'},2)
            query={k:v for k,v in args.items() if k not in ['policy_through','settlement_through']}
            self.both(p,'query_transactions','transactions query',{**query,'accepted_through':'1'},2)
            complete=self.report(p,args,'trial_balance')
            self.assertFalse(complete['manifest']['charges_complete'])
            self.assertEqual({c['currency'] for c in complete['manifest']['currency_controls']},{'USD','EUR'})
            eur=self.report(p,{**args,'currency':'EUR'},'trial_balance')
            self.assertTrue(all(v['net']['currency']=='EUR' for v in eur['items']))
            self.assertEqual(eur['manifest']['currency_controls'],complete['manifest']['currency_controls'])
    def test_native_replacement_metadata_is_lossless_in_query_schema(self):
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            args=self.inputs(p);args.pop('policy_through');args.pop('settlement_through')
            old=self.both(p,'query_transactions','transactions query',args)['data']['items'][0]
            meta=self.submit()['metadata'];meta['replaces']={'import_id':old['admission']['import_id'],'record_ids':[]}
            metadata_file=self.directory/'native-metadata.json';metadata_file.write_text(json.dumps(meta))
            payload_file=self.directory/'native-input.jsonl';payload_file.write_text(json.dumps(trade('native-meta','native-meta-event','1'))+'\n')
            self.admin('receive','--key','native-metadata','--metadata',str(metadata_file),'--payload',str(payload_file))
            self.admin('worker','--mode','drain')
            rows=self.both(p,'query_transactions','transactions query',{**args,'accepted_through':'3'})['data']['items']
            self.assertEqual(rows[-1]['source_metadata']['replaces'],meta['replaces'])
    def test_concurrent_retry_identity_and_readonly_tools(self):
        with Peer(MCP,self.profile) as p:
            self.prepare_book(p)
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                results=list(pool.map(lambda _: self.cli('settlement submit',self.settlement())['data'],range(8)))
            self.assertTrue(all(v==results[0] for v in results))
            args=self.inputs(p)
            expected=self.report(p,args,'journals')
            running=copy.deepcopy(self.binding);running['process']=True
            worker_profile=self.directory/'running.json';worker_profile.write_text(json.dumps({'books':[running]}))
            with Peer(MCP,worker_profile) as worker:
                for i in range(6):worker.call('submit_import',self.submit('background-'+str(i),json.dumps(trade('bg'+str(i),'bg-event'+str(i),'1'))+'\n'))
                with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                    values=list(pool.map(lambda _:self.cli('report',{**args,'collection':'journals'})['data'],range(8)))
                self.assertTrue(all(v==expected for v in values))
        readonly=copy.deepcopy(self.binding);readonly['submit']=False
        profile=self.directory/'readonly.json';profile.write_text(json.dumps({'books':[readonly]}))
        with Peer(MCP,profile) as p:
            names=[v['name'] for v in p.request('tools/list')['result']['tools']]
            self.assertNotIn('luca_submit_settlement',names);self.assertNotIn('luca_submit_refund',names)
            self.assertEqual(self.cli('settlement submit',self.settlement(),3,profile)['error']['code'],'PERMISSION_DENIED')

if __name__=='__main__':unittest.main()
