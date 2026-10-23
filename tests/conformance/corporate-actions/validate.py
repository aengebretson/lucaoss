#!/usr/bin/env python3
"""Offline contract/expectation checks. Does not post, ingest or model tax rules.

Uses a small, fail-closed interpreter for exactly the JSON Schema keywords in the
adjacent schemas; no third-party dependencies. Fraction arithmetic checks supplied
fixture expectations. This is not a production corporate-action engine.
"""
from __future__ import annotations
import argparse
from datetime import date, datetime
from fractions import Fraction as F
import hashlib
import json
from pathlib import Path
import re
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent

class ContractError(ValueError):
    pass

def check(ok, message):
    if not ok:
        raise ContractError(message)

def load(path):
    def pairs(items):
        result = {}
        for key, value in items:
            check(key not in result, 'duplicate JSON member: ' + key)
            result[key] = value
        return result
    return json.loads(Path(path).read_text(), object_pairs_hook=pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ContractError(value)))

def schema_check(value, schema, path='$'):
    supported = {'$schema', '$id', 'type', 'const', 'enum', 'anyOf', 'required',
                 'properties', 'additionalProperties', 'items', 'minItems',
                 'minLength', 'minimum', 'pattern', 'format'}
    check(not set(schema) - supported, path + ': unknown schema keyword')
    if 'anyOf' in schema:
        matches = 0
        for branch in schema['anyOf']:
            try:
                schema_check(value, branch, path)
                matches += 1
            except ContractError:
                pass
        check(matches > 0, path + ': no allowed shape')
        return
    if 'const' in schema:
        check(type(value) is type(schema['const']) and value == schema['const'], path + ': const')
    if 'enum' in schema:
        check(value in schema['enum'], path + ': enum')
    kind = schema.get('type')
    types = {'object': dict, 'array': list, 'string': str, 'integer': int,
             'boolean': bool, 'null': type(None)}
    if kind:
        check(type(value) is types[kind], path + ': ' + kind + ' required')
    if kind == 'object':
        check(set(schema['required']) <= set(value), path + ': missing field')
        check(schema.get('additionalProperties') is False, path + ': schema must close object')
        check(not set(value) - set(schema['properties']), path + ': unknown field')
        for key, child in value.items():
            schema_check(child, schema['properties'][key], path + '.' + key)
    if kind == 'array':
        check(len(value) >= schema.get('minItems', 0), path + ': minItems')
        for index, child in enumerate(value):
            schema_check(child, schema['items'], f'{path}[{index}]')
    if kind == 'string':
        check(len(value) >= schema.get('minLength', 0), path + ': minLength')
        if 'pattern' in schema:
            check(re.fullmatch(schema['pattern'], value) is not None, path + ': pattern')
        if schema.get('format') == 'date-time':
            check(re.fullmatch(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ', value) is not None,
                  path + ': UTC timestamp required')
            try:
                datetime.fromisoformat(value.replace('Z', '+00:00'))
            except ValueError as error:
                raise ContractError(path + ': invalid date-time') from error
        if schema.get('format') == 'date':
            try:
                check(date.fromisoformat(value).isoformat() == value, path + ': ISO date')
            except ValueError as error:
                raise ContractError(path + ': invalid date') from error
    if kind == 'integer':
        check(value >= schema.get('minimum', value), path + ': minimum')

def unique(records, key='id'):
    ids = [r[key] for r in records]
    check(len(set(ids)) == len(ids), 'duplicate identity: ' + key)
    return {r[key]: r for r in records}

def canonical(value):
    # Test-only digest convention, not production canonical serialization.
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()

def round_money(value):
    # Fraction.__round__ is exact, ties to even; never binary float.
    return F(round(value * 10**6), 10**6)

def rational(value):
    return F(int(value['numerator']), int(value['denominator']))

def bounded(value, scale):
    scaled = F(value) * 10**scale
    check(scaled.denominator == 1 and -(2**63) <= scaled < 2**63,
          'value exceeds kernel precision/range')

def lineage(case):
    sources = unique(case['sources'])
    for source in sources.values():
        check(hashlib.sha256(source['payload'].encode()).hexdigest() == source['sha256'],
              'source hash mismatch')
    check(bool(case['decision_ids']) == (case['expectation_status'] == 'conditional'),
          'conditional decision label mismatch')
    check(len(set(case['decision_ids'])) == len(case['decision_ids']), 'duplicate decision')
    decisions = set(re.findall(r'\bD-[A-Z]+\b', (ROOT.parents[2] / 'docs/corporate-actions/decisions.md').read_text()))
    check(set(case['decision_ids']) <= decisions, 'unknown decision ID')
    return sources

def cash_check(case):
    sources = lineage(case)
    actions = unique(case['action_revisions'])
    eligibilities = unique(case['eligibility_revisions'])
    payments = unique(case['payments'])
    unique(case['payments'], 'reference')
    unique(case['evaluations'])
    ident, policy = case['identity'], case['policy']
    expected_target = {k: ident[k] for k in policy['target']}
    check(policy['target'] == expected_target, 'policy target mismatch')
    definition = {k: v for k, v in policy.items() if k != 'digest'}
    check(hashlib.sha256(canonical(definition)).hexdigest() == policy['digest'], 'policy digest mismatch')
    check(policy['right_evidence'] in sources, 'missing right-established evidence')
    for records in (case['action_revisions'], case['eligibility_revisions']):
        seen = set()
        for record in records:
            check(record['predecessor'] is None or record['predecessor'] in seen,
                  'revision predecessor missing/out of order')
            if record['predecessor']:
                parent = next(r for r in records if r['id'] == record['predecessor'])
                check(parent['recorded_at'] <= record['recorded_at'], 'revision knowledge order')
            seen.add(record['id'])
            check(record['source_id'] in sources, 'missing source reference')
            check(json.loads(sources[record['source_id']]['payload']) == record,
                  'source payload/record mismatch')
            check(sources[record['source_id']]['received_at'] <= record['recorded_at'], 'source from future')
    for record in actions.values():
        check(F(record['rate']) >= 0, 'negative dividend rate')
    for e in eligibilities.values():
        bounded(e['quantity'], 8)
        bounded(e['settled_quantity'], 8)
        check(e['action_revision_id'] in actions, 'eligibility action missing')
        unique(e['adjustments'], 'record_id')
        check(F(e['settled_quantity']) + sum((F(a['quantity']) for a in e['adjustments']), F(0)) == F(e['quantity']),
              'eligible quantity explanation mismatch')
        check(all(a['source_id'] in sources for a in e['adjustments']), 'adjustment source missing')
    for index, p in enumerate(case['payments']):
        for field in ('gross', 'withholding', 'fees', 'cash'):
            bounded(p[field], 6)
        check(p['account_id'] == ident['account_id'] and p['entitlement_id'] == ident['entitlement_id'],
              'cross-account/entitlement allocation')
        check(p['source_id'] in sources, 'missing payment source')
        check(p['currency'] == case['action_revisions'][0]['currency'], 'cross-currency payment unsupported')
        check(sources[p['source_id']]['received_at'] <= p['recorded_at'], 'payment source from future')
        check(json.loads(sources[p['source_id']]['payload']) == p, 'payment source mismatch')
        check(F(p['withholding']) >= 0 and F(p['fees']) >= 0, 'negative deduction')
        check(F(p['gross']) - F(p['withholding']) - F(p['fees']) == F(p['cash']), 'payment components mismatch')
        if p['reverses']:
            prior = payments.get(p['reverses'])
            check(prior is not None and prior in case['payments'][:index], 'refund parent missing')
            check(F(p['gross']) < 0 < F(prior['gross']), 'refund direction')
            check(p['currency'] == prior['currency'] and p['actual_at'] >= prior['actual_at'], 'refund currency/time')
            refunded = sum((-F(x['gross']) for x in case['payments'][:index+1] if x['reverses'] == prior['id']), F(0))
            check(refunded <= F(prior['gross']), 'refund exceeds original payment')
    submissions = {}
    for row in case['submissions']:
        check(hashlib.sha256(row['payload'].encode()).hexdigest() == row['payload_digest'],
              'submission digest mismatch')
        old = submissions.get(row['key'])
        if row['expected'] == 'PERIOD_CLOSED':
            check('D-CLOSED' in case['decision_ids'], 'closed-period decision missing')
            continue
        outcome = 'accepted' if old is None else ('replayed' if old == row['payload_digest'] else 'conflict')
        check(outcome == row['expected'], 'idempotency outcome mismatch')
        if old is None:
            submissions[row['key']] = row['payload_digest']
        if outcome in ('accepted', 'replayed'):
            record = actions.get(row['payload_ref']) or eligibilities.get(row['payload_ref']) or payments.get(row['payload_ref'])
            check(record == json.loads(row['payload']), 'admitted submission/record mismatch')
        check(row['payload_ref'] in actions or row['payload_ref'] in eligibilities or row['payload_ref'] in payments,
              'submission target missing')
    for view in case['evaluations']:
        context, selected, expected, trace = (view[k] for k in ('context','resolved_inputs','expected','trace'))
        at, known = context['economic_as_of'], context['recorded_through']
        check(context['policy_through'] == policy['history_through'], 'policy cutoff mismatch')
        known_actions = [r for r in case['action_revisions'] if r['recorded_at'] <= known]
        active_actions = [r for r in known_actions if r['effective_at'] <= at]
        action = active_actions[-1] if active_actions else None
        check(selected['action_revision_id'] == (action['id'] if action else None), 'action selection/cutoff mismatch')
        known_elig = [e for e in case['eligibility_revisions'] if e['recorded_at'] <= known]
        available_elig = [e for e in known_elig if e['observed_at'] <= at]
        eligible = available_elig[-1] if available_elig else None
        check(selected['eligibility_revision_id'] == (eligible['id'] if eligible else None), 'eligibility selection/cutoff mismatch')
        if eligible:
            check(eligible['ledger_through'] <= context['ledger_through'], 'eligibility ledger from future')
            if action and action['status'] != 'cancelled':
                check(eligible['action_revision_id'] == action['id'], 'stale eligibility for revised action')
        ids = selected['payment_ids']
        check(len(ids) == len(set(ids)) and set(ids) <= set(payments), 'duplicate/missing selected payment')
        actual = [payments[i] for i in ids]
        check(all(p['actual_at'] <= at and p['recorded_at'] <= known for p in actual), 'payment cutoff violation')
        available_payments = {p['id'] for p in payments.values()
                              if p['actual_at'] <= at and p['recorded_at'] <= known}
        check(set(ids) == available_payments, 'available payment evidence omitted')
        exact = F(action['rate']) * F(eligible['quantity']) if action and eligible and action['status'] != 'cancelled' else F(0)
        gross = round_money(exact)
        if action and action['status'] != 'cancelled':
            recognized_local = datetime.fromisoformat(policy['right_established_at'].replace('Z','+00:00')).astimezone(
                ZoneInfo(action['record_timezone']))
            check(recognized_local.date().isoformat() == action['ex_on'] and
                  recognized_local.hour == recognized_local.minute == recognized_local.second == 0,
                  'recognition boundary/ex-date mismatch')
        bounded(expected['gross'], 6)
        check(rational(expected['gross_exact']) == exact, 'unrounded gross mismatch')
        check(F(expected['gross']) == gross and rational(expected['rounding_residual']) == exact - gross,
              'gross rounding mismatch')
        check(expected['position_before'] == expected['position_after'], 'cash dividend changed position')
        paid = sum((F(p['gross']) for p in actual), F(0))
        cash = sum((F(p['cash']) for p in actual), F(0))
        deductions = sum((F(p['withholding']) for p in actual), F(0))
        fees = sum((F(p['fees']) for p in actual), F(0))
        assigned = min(max(paid, F(0)), gross) if gross >= 0 else -min(max(-paid,F(0)), -gross)
        unpaid = gross-assigned
        check(F(expected['unpaid']) == unpaid, 'outstanding gross mismatch')
        code = ('FX_POLICY_REQUIRED' if action and action['currency'] != 'USD' else
                'RECOGNITION_POLICY_REQUIRED' if policy['recognition'] == 'unspecified' else
                'WITHHOLDING_POLICY_REQUIRED' if policy['withholding_class'] == 'unresolved' else
                'PERIOD_CLOSED' if any(s['expected']=='PERIOD_CLOSED' for s in case['submissions']) else 'OK')
        check(expected['code'] == code, 'expected diagnostic mismatch')
        blocked = code not in ('OK', 'PERIOD_CLOSED')
        recognized = (not blocked and at >= policy['right_established_at'] and
                      known >= sources[policy['right_evidence']]['received_at'])
        effective_assigned = assigned if recognized else F(0)
        accrued = gross if recognized and policy['recognition'] == 'accrual' else effective_assigned
        obligation = gross-effective_assigned if recognized and policy['recognition'] == 'accrual' else F(0)
        state = dict(cash=cash, receivable=max(obligation,F(0)), payable=max(-obligation,F(0)),
                     income=accrued, withholding_asset=deductions, fee_expense=fees,
                     suspense_liability=paid-effective_assigned)
        if blocked:
            # Current blocked cases contain no actual cash: never hide received money.
            check(not actual, 'blocked accounting with cash needs separate suspense fixture')
            state = dict.fromkeys(state, F(0))
        if deductions:
            check(policy['withholding_class']=='recoverable_asset', 'unsupported deduction classification')
        if fees:
            check(policy['fee_class']=='expense', 'unsupported fee classification')
        for key, amount in state.items():
            check(F(expected[key]) == amount, view['id'] + ': ' + key + ' mismatch')
        check(sum((F(l['amount']) * (1 if l['side']=='debit' else -1) for l in expected['balance_lines']),F(0)) == 0,
              'unbalanced expected lines')
        lines = unique(expected['balance_lines'], 'account')
        credit = {'payable','income','suspense_liability'}
        check(set(lines)=={k for k,v in state.items() if v}, 'line/state coverage mismatch')
        for key,line in lines.items():
            check(F(line['amount']) > 0 and line['currency']=='USD', 'invalid expected line amount/currency')
            signed = F(line['amount']) * (1 if line['side']=='debit' else -1)
            check(signed == state[key]*(-1 if key in credit else 1), 'line/state amount mismatch')
        check(trace['action_id']==ident['action_id'] and trace['entitlement_id']==ident['entitlement_id'], 'trace identity mismatch')
        check(trace['action_revision_ids']==[r['id'] for r in known_actions], 'action lineage mismatch')
        check(trace['eligibility_revision_ids']==[e['id'] for e in known_elig], 'eligibility lineage mismatch')
        refs = {r['source_id'] for r in known_actions+known_elig+actual}
        if eligible:
            refs.update(a['source_id'] for a in eligible['adjustments'])
        if known >= sources[policy['right_evidence']]['received_at'] and action:
            refs.add(policy['right_evidence'])
        check(set(trace['source_ids'])==refs and len(trace['source_ids'])==len(refs), 'source lineage mismatch')
        check(refs <= set(sources), 'dangling source lineage')
        check(all(sources[s]['received_at'] <= known for s in refs), 'selected source beyond knowledge cutoff')
        check(trace['policy_id']==policy['id'] and trace['policy_revision_ids']==policy['revision_ids'] and
              trace['policy_digest']==policy['digest'], 'resolved policy trace mismatch')

def other_check(case):
    sources=lineage(case)
    inp, out, kind = case['inputs'],case['expected'],case['kind']
    check(set(case['trace']['source_ids'])==set(sources), 'reorganization source trace mismatch')
    check(json.loads(next(iter(sources.values()))['payload'])==inp, 'reorganization input/source mismatch')
    q=F(inp['quantity']); basis=F(inp['carrying_amount'])
    bounded(inp['quantity'], 8)
    bounded(inp['carrying_amount'], 6)
    lots = unique(inp['input_lots'])
    check(set(lots) == set(case['trace']['input_lot_ids']), 'input lot lineage mismatch')
    check(sum((F(l['quantity']) for l in lots.values()), F(0)) == q, 'input lot quantity mismatch')
    check(sum((F(l['total']) for l in lots.values()), F(0)) == basis, 'input lot carrying mismatch')
    for lot in lots.values():
        check(sum((F(lot[k]) for k in ('principal','commissions','fees')), F(0)) == F(lot['total']),
              'input lot components mismatch')
    ratio=F(int(inp['ratio_numerator']),int(inp['ratio_denominator']))
    fraction=F(int(out['fraction_numerator']),int(out['fraction_denominator']))
    check(q>=0 and basis>=0 and 0<=fraction<1, 'invalid transformation quantity/basis/fraction')
    check(F(out['whole_unit_entitlement'])+fraction==q*ratio, 'ratio/fraction mismatch')
    if inp['fraction_price'] is None:
        check(out['cash_in_lieu'] is None, 'invented cash-in-lieu price')
    else:
        check(F(out['cash_in_lieu'])==round_money(fraction*F(inp['fraction_price'])), 'cash-in-lieu mismatch')
    check(F(out['consideration'])==F(inp['cash_consideration']), 'consideration mismatch')
    check(out['actual_cash']=='0.000000', 'scheduled cash treated as paid')
    weights=[F(w) for w in inp['allocation_weights']]
    if weights:
        check(all(w>=0 for w in weights) and sum(weights,F(0))==1, 'basis weights mismatch')
    components={c['name']:c['value'] for c in inp['additional_components']}
    check(len(components)==len(inp['additional_components']), 'duplicate component')
    code = ('BASIS_POLICY_REQUIRED' if kind == 'stock_dividend' else
            'IDENTITY_REVIEW_REQUIRED' if kind == 'identity_change' and 'continuity_approval' not in components else
            'ELECTION_AND_BASIS_REQUIRED' if kind == 'mixed_merger' and inp['election'] == 'unresolved' else
            'FINALITY_AND_BASIS_REQUIRED' if kind == 'final_liquidation' and not inp['finality'] else 'OK')
    check(out['code'] == code, 'family diagnostic mismatch')
    gain_families = {'cash_merger', 'full_call', 'final_liquidation', 'partial_redemption'}
    if kind not in gain_families or code != 'OK':
        check(out['gain'] is None, 'invented gain for unsupported classification')
    if out['code']!='OK':
        check(out['successor_carrying'] is None, 'unresolved basis became known')
        check(not out['lot_allocations'], 'blocked basis invented lot allocations')
        check(case['trace']['allocation_evidence_id'] is None, 'unresolved allocation evidence claimed')
        check(F(out['retired_quantity']) == 0, 'blocked action retired units')
        check(out['remaining_carrying'] is None, 'blocked classification invented remaining carrying')
        if kind=='stock_dividend':
            check(F(out['remaining_quantity'])==q*(1+ratio), 'stock dividend total mismatch')
        else:
            check(F(out['remaining_quantity']) == q, 'blocked action changed current units')
        return
    check(case['trace']['allocation_evidence_id'] in sources, 'allocation evidence missing')
    successor=F(out['successor_carrying']);remaining=F(out['remaining_carrying'])
    allocations = out['lot_allocations']
    unique(allocations, 'component_role')
    for allocation in allocations:
        check(allocation['input_lot_id'] in lots, 'allocation lot missing')
        lot = lots[allocation['input_lot_id']]
        for field in ('principal','commissions','fees'):
            check(F(allocation[field]) == round_money(F(lot[field]) * F(allocation['total']) / F(lot['total'])),
                  'per-component proportional allocation mismatch')
        check(sum((F(allocation[k]) for k in ('principal','commissions','fees')), F(0)) == F(allocation['total']),
              'output lot components mismatch')
    for lot_id, lot in lots.items():
        for field in ('principal','commissions','fees','total'):
            check(sum((F(a[field]) for a in allocations if a['input_lot_id'] == lot_id), F(0)) == F(lot[field]),
                  'per-component lot conservation mismatch')
    allocation_totals = {a['component_role']: F(a['total']) for a in allocations}
    if 'remaining' in allocation_totals:
        check(allocation_totals['remaining'] == remaining, 'remaining lot allocation mismatch')
    if 'successor' in allocation_totals:
        check(allocation_totals['successor'] == successor, 'successor lot allocation mismatch')
    if 'fraction' in allocation_totals:
        check(allocation_totals['fraction'] == F(out['fraction_carrying']), 'fraction lot allocation mismatch')
    if kind=='forward_split':
        check(successor==basis==remaining and F(out['remaining_quantity'])==q*ratio, 'split conservation')
    elif kind=='reverse_split':
        check(successor+F(out['fraction_carrying'])==basis, 'fraction basis conservation')
        check(successor==basis*weights[0] and F(out['fraction_carrying'])==basis*weights[1], 'fraction basis allocation')
        check(remaining==successor and F(out['remaining_quantity'])==F(out['whole_unit_entitlement']), 'reverse split remainder')
    elif kind=='identity_change':
        check(case['identity']['security_id']==case['identity']['successor_id'], 'stable identity changed')
        check('continuity_approval' in components and remaining==successor==basis and F(out['remaining_quantity'])==q,
              'identity continuity evidence/conservation')
    elif kind=='spinoff':
        check(remaining+successor==basis and remaining==basis*weights[0] and successor==basis*weights[1], 'spinoff allocation')
        check(F(out['remaining_quantity'])==q, 'spinoff retired parent')
    elif kind in ('stock_merger','mixed_merger'):
        check(inp['election'].startswith('agent_confirmed_'), 'missing election')
        cash_basis=F(components.get('cash_component_carrying','0'))
        check(successor+cash_basis==basis and successor==basis*weights[0], 'merger allocation')
        check(F(out['retired_quantity'])==q and remaining==0 and F(out['remaining_quantity'])==0, 'merger retirement')
    elif kind in ('cash_merger','full_call','final_liquidation'):
        check(F(out['retired_quantity'])==q and F(out['remaining_quantity'])==remaining==successor==0, 'full retirement')
        check(F(out['gain'])==F(out['consideration'])-basis, 'proceeds less carrying mismatch')
        if kind in ('full_call','final_liquidation'):
            check(inp['finality'], 'missing finality')
        if kind=='full_call':
            check(F(components['principal'])+F(components['premium'])+F(components['interest'])-F(components['fees'])==F(out['consideration']), 'call components mismatch')
    elif kind=='partial_liquidation':
        check(remaining==successor==basis-F(components['carrying_reduction']), 'partial carrying reduction')
        check(F(out['remaining_quantity'])==q and F(out['retired_quantity'])==0, 'continuing units mismatch')
    elif kind=='partial_redemption':
        relief=F(components['carrying_relief']);retired=F(components['retired_quantity'])
        check(F(out['remaining_quantity'])+retired==q and F(out['retired_quantity'])==retired, 'partial quantity conservation')
        check(remaining==successor==basis-relief and F(out['gain'])==F(out['consideration'])-relief, 'partial basis/gain mismatch')
    else:
        raise ContractError('unverified family: '+kind)

def eligibility_check(case):
    sources = unique(case['sources'])
    for source in sources.values():
        check(hashlib.sha256(source['payload'].encode()).hexdigest() == source['sha256'], 'rule source hash mismatch')
    check(set(case['trace']['source_ids']) == set(sources), 'rule trace mismatch')
    rule, ann, inp, out = (case[k] for k in ('rule', 'announcement', 'inputs', 'expected'))
    for suffix, record in (('announcement', ann), ('ledger', inp), ('rule', rule)):
        check(json.loads(sources[case['id']+':'+suffix]['payload']) == record, 'rule source/record mismatch')
    check(case['trace']['market_rule_id'] == rule['id'] and case['trace']['calendar_id'] == rule['calendar_id'],
          'market rule/calendar identity mismatch')
    check(case['decision_ids'] == ['D-MARKETS'], 'market scope approval dependency missing')
    check(rule['effective_from'] <= ann['announced_on'], 'rule not effective')
    business = rule['business_days']
    check(business == sorted(set(business)), 'calendar order/duplicates')
    check(all(rule['calendar_start'] <= d <= rule['calendar_end'] for d in business), 'calendar date out of range')
    observation = inp['observation']
    check(observation['source_id'] in sources and
          json.loads(sources[observation['source_id']]['payload']) == observation, 'observation evidence missing')
    trades = unique(inp['trades'])
    for trade in trades.values():
        check(trade['source_id'] in sources and
              json.loads(sources[trade['source_id']]['payload']) == trade, 'trade evidence missing')
        check(trade['trade_on'] >= inp['opening_at'], 'trade predates opening anchor')
        check(trade['scheduled_delivery_on'] >= trade['trade_on'], 'delivery scheduled before trade')
        if trade['actual_delivery_on']:
            check(trade['actual_delivery_on'] >= trade['trade_on'], 'delivery before trade')
    code, ex = 'OK', None
    if not all(rule['calendar_start'] <= ann[k] <= rule['calendar_end'] for k in ('record_on', 'pay_on')):
        code = 'CALENDAR_COVERAGE_REQUIRED'
    elif rule['foreign_security'] or (not ann['timely'] and not ann['designated_ex_on']):
        code = 'EX_DATE_DESIGNATION_REQUIRED'
    elif ann['designated_ex_on']:
        ex = ann['designated_ex_on']
        check(ex in business and ann['designation_source_id'] in sources, 'invalid designation/evidence')
    else:
        check(F(ann['reference_price']) > 0 and F(ann['rate']) >= 0, 'distribution value inputs invalid')
        if F(ann['rate']) / F(ann['reference_price']) >= F(1,4):
            candidates = [d for d in business if d > ann['pay_on']]
            if candidates: ex = candidates[0]
            else: code = 'CALENDAR_COVERAGE_REQUIRED'
        else:
            candidates = [d for d in business if d <= ann['record_on']]
            if candidates: ex = candidates[-1]
            else: code = 'CALENDAR_COVERAGE_REQUIRED'
    if code == 'OK' and any(t['route'] != 'regular' for t in trades.values()):
        code = 'ROUTE_POLICY_REQUIRED'
    check(out['code'] == code and out['ex_on'] == ex and out['recognition_on'] == ex,
          'derived ex-date/recognition/diagnostic mismatch')
    if code != 'OK':
        check(out['eligible_quantity'] is None and out['gross'] is None and out['observed_minus_computed'] is None,
              'blocked market rule invented an entitlement')
        return
    opening = F(inp['opening_quantity'])
    base = opening + sum((F(t['quantity']) for t in trades.values()
                          if t['actual_delivery_on'] and t['actual_delivery_on'] <= ann['record_on']), F(0))
    customer = opening + sum((F(t['quantity']) for t in trades.values() if t['trade_on'] < ex), F(0))
    interim = sum((F(t['quantity']) for t in trades.values() if t['actual_delivery_on'] and
                   ann['record_on'] < t['actual_delivery_on'] <= ex and t['trade_on'] < ex), F(0)) if ex != ann['record_on'] else F(0)
    participant = base + interim
    eligible = customer if rule['role'] == 'customer' else participant
    check(F(out['record_settled_quantity']) == base and F(out['net_claim_quantity']) == customer-participant,
          'record/interim/claim quantity mismatch')
    check(F(out['eligible_quantity']) == eligible, 'derived eligible quantity mismatch')
    gross = round_money(eligible * F(ann['rate']))
    difference = round_money(F(observation['quantity']) * F(ann['rate'])) - gross
    check(F(out['gross']) == gross and F(out['observed_minus_computed']) == difference,
          'gross/external reconciliation mismatch')

def validate(document, schema_path):
    schema_check(document,load(schema_path))
    unique(document['cases'])
    for case in document['cases']:
        try:
            checker = (cash_check if document['contract'].endswith('cash-dividends/1') else
                       eligibility_check if document['contract'].endswith('eligibility-rules/1') else other_check)
            checker(case)
        except (KeyError, ValueError, ZeroDivisionError) as error:
            raise ContractError(case['id']+': '+str(error)) from error
    return len(document['cases'])

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=ROOT)
    args=parser.parse_args()
    total=0
    for name in ('cash-dividends','eligibility-rules','other-actions'):
        count=validate(load(args.root/(name+'.json')),args.root/(name+'.schema.json'))
        total+=count
        print(f'{name}: {count} unsupported-engine fixture contracts checked')
    print(f'{total} contracts valid; this does not demonstrate corporate-action engine support')

if __name__=='__main__':
    main()
