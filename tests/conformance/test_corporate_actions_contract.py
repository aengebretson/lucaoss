"""Contract checks and adversarial mutations; picked up by existing CTest discovery."""
import copy
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).parent/'corporate-actions'
spec=importlib.util.spec_from_file_location('corporate_action_fixture_checks',ROOT/'validate.py')
checks=importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)

class CorporateActionsContractTest(unittest.TestCase):
    def setUp(self):
        self.cash=checks.load(ROOT/'cash-dividends.json')
        self.other=checks.load(ROOT/'other-actions.json')
        self.rules=checks.load(ROOT/'eligibility-rules.json')

    def validate(self, document, name='cash-dividends'):
        return checks.validate(document, ROOT/(name+'.schema.json'))

    def reject(self, mutate, document=None, name='cash-dividends'):
        document=copy.deepcopy(document or self.cash)
        mutate(document)
        with self.assertRaises(checks.ContractError):
            self.validate(document,name)

    def test_all_fixtures_and_deterministic_repeat(self):
        for document,name,count in ((self.cash,'cash-dividends',29),(self.other,'other-actions',16),
                                    (self.rules,'eligibility-rules',12)):
            before=copy.deepcopy(document)
            self.assertEqual(self.validate(document,name),count)
            self.assertEqual(self.validate(document,name),count)
            self.assertEqual(document,before)

    def test_closed_schema_and_exact_number_types(self):
        for mutate in (
            lambda d:d['cases'][0].update(unknown=True),
            lambda d:d['cases'][0]['policy'].pop('right_evidence'),
            lambda d:d['cases'][0]['evaluations'][0]['expected'].update(cash=1.0),
            lambda d:d['cases'][0]['action_revisions'][0].update(rate='NaN'),
            lambda d:d['cases'][0]['evaluations'][0]['context'].update(ledger_through=True),
            lambda d:d['cases'][0]['action_revisions'][0].update(record_on='2026-02-30'),
            lambda d:d['cases'][0]['action_revisions'][0].update(effective_at='2026-10-01T09:00:00'),
        ):
            self.reject(mutate)

    def test_tampered_source_and_policy_evidence(self):
        self.reject(lambda d:d['cases'][0]['sources'][0].update(payload='altered'))
        self.reject(lambda d:d['cases'][0]['policy'].update(recognition='cash'))
        self.reject(lambda d:d['cases'][0]['evaluations'][0]['trace'].update(source_ids=[]))
        self.reject(lambda d:d['cases'][0]['evaluations'][0]['trace'].update(policy_digest='0'*64))
        self.reject(lambda d:d['cases'][0]['evaluations'][0]['trace'].update(policy_revision_ids=['missing']))

    def test_amounts_balances_positions_and_rounding(self):
        for mutate in (
            lambda e:e.update(gross='40.000001'),
            lambda e:e.update(position_after='126'),
            lambda e:e['balance_lines'][0].update(amount='39.000000'),
            lambda e:e['balance_lines'][0].update(currency='EUR'),
            lambda e:e.update(unpaid='0.000000'),
            lambda e:e['gross_exact'].update(numerator='41'),
            lambda e:e['rounding_residual'].update(numerator='1'),
        ):
            self.reject(lambda d:mutate(d['cases'][0]['evaluations'][0]['expected']))
        self.assertEqual(checks.round_money(checks.F('0.0000005')),checks.F(0))
        self.assertEqual(checks.round_money(checks.F('0.0000015')),checks.F('0.000002'))
        self.assertEqual(checks.round_money(checks.F('-0.0000015')),checks.F('-0.000002'))

    def test_cutoffs_revisions_and_identity(self):
        self.reject(lambda d:d['cases'].append(copy.deepcopy(d['cases'][0])))
        self.reject(lambda d:d['cases'][0]['action_revisions'][0].update(predecessor='missing'))
        self.reject(lambda d:d['cases'][0]['evaluations'][0]['resolved_inputs'].update(action_revision_id='missing'))
        self.reject(lambda d:d['cases'][0]['evaluations'][0]['context'].update(ledger_through=0))
        self.reject(lambda d:d['cases'][2]['evaluations'][0]['context'].update(economic_as_of='2026-10-19T00:00:00Z'))
        self.reject(lambda d:d['cases'][2]['payments'][0].update(account_id='synthetic:other-account'))
        self.reject(lambda d:d['cases'][2]['evaluations'][0]['resolved_inputs']['payment_ids'].append('CD-03:p1'))

    def test_duplicates_refunds_and_blocked_expectations(self):
        self.reject(lambda d:d['cases'][12]['submissions'][1].update(expected='accepted'))
        self.reject(lambda d:d['cases'][13]['submissions'][1].update(expected='replayed'))
        self.reject(lambda d:d['cases'][19]['payments'][-1].update(reverses='missing'))
        self.reject(lambda d:d['cases'][21]['evaluations'][0]['expected'].update(code='OK'))
        self.reject(lambda d:d['cases'][7].update(expectation_status='specified_fixture'))

    def test_other_family_ratio_basis_election_and_finality(self):
        mutations=(
            (0,lambda c:c['expected'].update(whole_unit_entitlement='251')),
            (1,lambda c:c['expected'].update(fraction_denominator='0')),
            (1,lambda c:c['expected'].update(cash_in_lieu='4.000000')),
            (6,lambda c:c['expected'].update(remaining_carrying='801')),
            (7,lambda c:c['inputs'].update(election='unresolved')),
            (8,lambda c:c['expected'].update(successor_carrying='500')),
            (11,lambda c:c['inputs'].update(finality=False)),
            (12,lambda c:c['expected'].update(gain='0')),
            (13,lambda c:c['expected'].update(remaining_quantity='0')),
            (14,lambda c:c['expected'].update(retired_quantity='21')),
        )
        for index,mutate in mutations:
            self.reject(lambda d:mutate(d['cases'][index]),self.other,'other-actions')

    def test_unknown_schema_keyword_fails_closed(self):
        with self.assertRaises(checks.ContractError):
            checks.schema_check('abc',{'type':'string','newUnimplementedKeyword':True})

    def test_market_dates_quantities_and_external_breaks(self):
        mutations=(
            (0,lambda c:c['expected'].update(ex_on='2026-10-14')),
            (1,lambda c:c['expected'].update(recognition_on='2026-10-18')),
            (2,lambda c:c['expected'].update(ex_on='2026-10-15')),
            (3,lambda c:c['expected'].update(code='OK')),
            (5,lambda c:c['expected'].update(eligible_quantity='100')),
            (5,lambda c:c['expected'].update(observed_minus_computed='0.000000')),
            (6,lambda c:c['expected'].update(eligible_quantity='110')),
            (9,lambda c:c['expected'].update(record_settled_quantity='130')),
            (10,lambda c:c['expected'].update(code='OK')),
            (11,lambda c:c['expected'].update(code='OK')),
        )
        for index,mutate in mutations:
            self.reject(lambda d:mutate(d['cases'][index]),self.rules,'eligibility-rules')

    def test_schema_range_and_missing_payment_evidence(self):
        self.reject(lambda d:d['cases'][2]['evaluations'][0]['resolved_inputs'].update(payment_ids=[]))
        self.reject(lambda d:d['cases'][0]['evaluations'][0]['expected'].update(gross='9223372036854.775808'))
        self.reject(lambda d:d['cases'][0]['evaluations'][0]['expected'].update(gross='40.0000001'))
        self.reject(lambda d:d['cases'][0]['expected']['lot_allocations'][0].update(commissions='0.000000'),
                    self.other,'other-actions')

    def test_review_regressions_currency_ex_date_future_sources_and_blocked_actions(self):
        def replace_record(case, record, field, value):
            record[field] = value
            source = next(s for s in case['sources'] if s['id'] == record['source_id'])
            source['payload'] = json.dumps(record,sort_keys=True,separators=(',',':'))
            source['sha256'] = hashlib.sha256(source['payload'].encode()).hexdigest()
        self.reject(lambda d:replace_record(d['cases'][2], d['cases'][2]['payments'][0], 'currency','EUR'))
        self.reject(lambda d:replace_record(d['cases'][0], d['cases'][0]['action_revisions'][0], 'ex_on','2026-10-30'))
        def future_source(document, index, source_id):
            next(s for s in document['cases'][index]['sources'] if s['id']==source_id)['received_at']='2027-01-01T00:00:00Z'
        self.reject(lambda d:future_source(d,2,'CD-03:source-p1'))
        self.reject(lambda d:future_source(d,0,'synthetic:ex-date-derivation'))
        self.reject(lambda d:d['cases'][15]['expected'].update(retired_quantity='100',remaining_quantity='0'),
                    self.other,'other-actions')
        self.reject(lambda d:d['cases'][0]['expected'].update(gain='500'),self.other,'other-actions')
        self.reject(lambda d:d['cases'][1]['trace'].update(allocation_evidence_id='missing'),self.other,'other-actions')

    def test_coverage_matrix_contains_every_fixture(self):
        with (ROOT.parents[2]/'docs/corporate-actions/coverage.csv').open() as stream:
            rows=list(csv.DictReader(stream))
        cases={c['id']:c for document in (self.cash,self.other,self.rules) for c in document['cases']}
        self.assertEqual({r['fixture_id'] for r in rows},set(cases))
        self.assertEqual(len(rows),len(cases))
        for row in rows:
            self.assertEqual(row['engine_support'],'unsupported')
            self.assertEqual(row['decision_ids'],';'.join(cases[row['fixture_id']]['decision_ids']))

    def test_duplicate_json_members_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'bad.json';path.write_text('{"id":"a","id":"b"}')
            with self.assertRaises(checks.ContractError):checks.load(path)

if __name__=='__main__':unittest.main()
