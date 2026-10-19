#!/usr/bin/env python3
"""Audit entry events independently of CF_ENTRY's inferred source edge.

Strict mode requires uniquely corresponding, equal entry commands. Contract
mode only checks the configured procedure. Neither proves source-path equality.
Unmarked legacy entries are detected by fiber role and a large E-only feed
(at least half the expected prefeed). Their completeness cannot be established
without an independent source trace; small ordinary unretracts are not entries.
"""
import argparse
import hashlib
import json
import math
import re
from pathlib import Path

WORD = re.compile(r'\b([XYZEFPS])([-+]?(?:\d+(?:\.\d*)?|\.\d+))')
DEFAULT_CONTRACT = dict(prefeed_mm=23.5, prefeed_F=600., hop_mm=2.,
                        landing_F=180., dwell_ms=0., first_powered_F=600.)
FIBER_ROLES = {'Fiber wall', 'Fiber infill', 'Continuous fiber contour', 'Continuous fiber infill'}


def same(a, b, tolerance=1e-8):
    return a is not None and b is not None and abs(a-b) <= tolerance


def parse(path, legacy, contract=None):
    contract = contract or DEFAULT_CONTRACT
    xyz, e, feed, layer = [0., 0., 0.], 0., None, 0
    relative_e, absolute = False, True
    role, block, rows = '', None, []

    def begin(number):
        return dict(layer=layer, role=role, begin_line=number, landing=[], dwell=[],
                    prefeed=[], extra_e=[], events=[], source={}, complete=False)

    def finish(reason):
        nonlocal block
        if block is not None:
            block['end_reason'] = reason
            rows.append(block)
            block = None

    for number, raw in enumerate(path.read_text().splitlines(), 1):
        raw = raw.strip()
        if raw in (';LAYER_CHANGE', '; CHANGE_LAYER'):
            finish('layer_changed_before_entry_completed')
            layer += 1
        if raw.startswith(';TYPE:'):
            role = raw[6:]
        elif raw.startswith('; FEATURE: '):
            role = raw[len('; FEATURE: '):]
        if raw == ';FIBER_BEGIN' or raw.startswith(';FIBER_BEGIN '):
            finish('next_entry_before_completion')
            block = begin(number)
            block['begin_metadata'] = raw
        if raw.startswith(';FIBER_END'):
            finish('fiber_ended_before_entry_completed')
        if raw.startswith(';CF_ENTRY ') and block is not None:
            block['source'] = {k: float(v) for k, v in re.findall(r'(\w+)=([-+\d.eE]+)', raw)}
        code = raw.split(';', 1)[0].strip()
        op = code.split()[0] if code else ''
        values = {k: float(value) for k, value in WORD.findall(code)}
        if op == 'G90':
            absolute = True
        elif op == 'G91':
            absolute = False
        elif op == 'M82':
            relative_e = False
        elif op == 'M83':
            relative_e = True
        elif op == 'G92':
            e = values.get('E', e)
            xyz = [values.get(axis, xyz[i]) for i, axis in enumerate('XYZ')]
        if op not in ('G0', 'G1', 'G2', 'G3', 'G4'):
            if block is not None and code:
                block['events'].append(dict(line=number, op=op, raw=raw, kind='state'))
            continue
        if op == 'G4':
            if block is not None:
                pause = values.get('P', values.get('S', 0.)*1000.)
                event = dict(line=number, op=op, ms=pause, raw=raw, kind='dwell')
                block['dwell'].append(event)
                block['events'].append(event)
            continue
        feed = values.get('F', feed)
        de = values.get('E', 0.) if relative_e else values.get('E', e)-e
        target = [values.get(k, xyz[i]) if absolute else xyz[i]+values.get(k, 0.)
                  for i, k in enumerate('XYZ')]
        has_xyz, has_e = any(k in values for k in 'XYZ'), 'E' in values
        e_only = has_e and not has_xyz
        # Do not reset an entry upon an unexpected second E-only command.
        if legacy and block is None and role in FIBER_ROLES and e_only and de >= max(contract['prefeed_mm']*.5, 1e-6):
            block = begin(number)
        if block is not None:
            event = dict(line=number, op=op, start=xyz[:], end=target[:], E=de,
                         has_E=has_e, F=feed, raw=raw, kind='state')
            if e_only:
                event['kind'] = 'prefeed' if not block['prefeed'] else 'extra_e'
                block[event['kind']].append(event)
            elif has_xyz and block['prefeed']:
                if de > 0:
                    event['kind'] = 'powered'
                    if 'first_powered' not in block:
                        block['first_powered'] = event
                        if contract['hop_mm'] > 0:
                            block['complete'] = True
                    else:
                        block['first_post_dwell_powered'] = event
                        block['complete'] = True
                else:
                    event['kind'] = 'landing'
                    block['landing'].append(event)
            elif has_xyz:
                event['kind'] = 'approach'
            block['events'].append(event)
            if block['complete']:
                finish('entry_completed')
        xyz, e = target, e+de
    finish('end_of_file_before_entry_completed')
    return rows


def audit(rows, contract=None):
    contract = contract or DEFAULT_CONTRACT
    failures = []
    for row in rows:
        issues, prefeed = [], row['prefeed']
        if not row['complete']:
            issues.append('incomplete_entry')
        if len(prefeed) != 1 or not same(prefeed[0]['E'], contract['prefeed_mm'], .00001):
            issues.append('prefeed_amount_or_count')
        if len(prefeed) != 1 or not same(prefeed[0]['F'], contract['prefeed_F']):
            issues.append('prefeed_feedrate')
        if row['extra_e']:
            issues.append('additional_E_only_command')
        if any(event.get('op') == 'S0' for event in row['events']):
            issues.append('cut_inside_entry')
        powered = row.get('first_powered')
        if powered is None or not same(powered['F'], contract['first_powered_F']):
            issues.append('first_powered_feedrate_or_missing')
        dwell = row['dwell']
        if len(dwell) != 1 or not same(dwell[0]['ms'], contract['dwell_ms']):
            issues.append('dwell_amount_or_count')
        if contract['hop_mm'] > 0:
            if len(row['landing']) != 1:
                issues.append('landing_must_be_one_XYZ_command')
            else:
                motion = row['landing'][0]
                if motion['op'] != 'G1' or motion['has_E']:
                    issues.append('landing_must_be_G1_without_E_word')
                if not same(motion['F'], contract['landing_F']):
                    issues.append('landing_feedrate')
                if not same(motion['start'][2]-motion['end'][2], contract['hop_mm'], .001):
                    issues.append('landing_height')
                if len(dwell) == 1 and powered is not None and not (
                        prefeed and prefeed[0]['line'] < motion['line'] < dwell[0]['line'] < powered['line']):
                    issues.append('prefeed_landing_dwell_powered_order')
        else:
            if row['landing']:
                issues.append('unexpected_lowering_without_hop')
            following = row.get('first_post_dwell_powered')
            if powered is not None and len(dwell) == 1 and following is not None and not (
                    prefeed and prefeed[0]['line'] < powered['line'] < dwell[0]['line'] < following['line']):
                issues.append('powered_first_edge_dwell_order')
        if issues:
            failures.append(dict(layer=row['layer'], begin_line=row['begin_line'], issues=issues))
    return failures


def metadata_audit(rows, tolerance=.000501):
    """Internal consistency only; never an oracle for legacy equivalence."""
    failures, checked = [], 0
    for row in rows:
        source = row['source']
        required = ('start_x', 'start_y', 'first_x', 'first_y', 'source_station')
        if not all(key in source for key in required) or len(row['landing']) != 1:
            continue
        checked += 1
        a, b = [source['start_x'], source['start_y']], [source['first_x'], source['first_y']]
        length, distance = math.dist(a, b), source['source_station']
        expected = [a[i]+(b[i]-a[i])*distance/length for i in range(2)] if length else a
        if any(abs(x-y) > tolerance for x, y in zip(expected, row['landing'][0]['end'][:2])):
            failures.append(dict(line=row['begin_line'], reason='metadata_target_differs_from_command'))
    return dict(scope='internal_metadata_consistency_only', checked=checked, failures=failures)


def entry_start(row):
    return row['prefeed'][0]['start'] if row['prefeed'] else None


def command_differences(old, new):
    def equal(a, b):
        if isinstance(a, list) and isinstance(b, list):
            return len(a) == len(b) and all(equal(x, y) for x, y in zip(a, b))
        if isinstance(a, (int, float)) and isinstance(b, (int, float)):
            return same(a, b)
        return a == b

    differences = []
    for key in ('prefeed', 'landing', 'dwell'):
        a, b = old[key], new[key]
        if len(a) != len(b):
            differences.append(key+'_count')
            continue
        for left, right in zip(a, b):
            fields = ('ms',) if key == 'dwell' else ('start', 'end', 'E', 'F', 'op', 'has_E')
            for field in fields:
                if not equal(left.get(field), right.get(field)):
                    differences.append(key+'.'+field)
    for key in ('first_powered', 'first_post_dwell_powered'):
        a, b = old.get(key), new.get(key)
        if bool(a) != bool(b):
            differences.append(key+'_presence')
        elif a and any(not equal(a[field], b[field]) for field in ('start', 'end', 'E', 'F', 'op', 'has_E')):
            differences.append(key)
    if old['extra_e'] or new['extra_e']:
        differences.append('additional_E_only_command')
    return differences


def match_entries(old, new, tolerance=.001):
    """Require unique layer/start correspondence in both directions, never zip.

    Same-seam ambiguity is reported instead of guessed. Standard G-code cannot
    establish unquantized source identity, even when commands are identical.
    """
    candidates = []
    for a in old:
        start = entry_start(a)
        candidates.append([j for j, b in enumerate(new) if start is not None and entry_start(b) is not None
                           and a['layer'] == b['layer'] and math.dist(start, entry_start(b)) <= tolerance])
    reverse = [[i for i, items in enumerate(candidates) if j in items] for j in range(len(new))]
    pairs = [(i, js[0]) for i, js in enumerate(candidates) if len(js) == 1 and len(reverse[js[0]]) == 1]
    paired_old, paired_new = {i for i, _ in pairs}, {j for _, j in pairs}
    return dict(basis='unique_layer_and_prefeed_XYZ_not_source_provenance',
                source_path_equivalence_established=False,
                pairs=[dict(standard_index=i, current_index=j, standard_line=old[i]['begin_line'],
                            current_line=new[j]['begin_line'], differences=command_differences(old[i], new[j]))
                       for i, j in pairs],
                unmatched_standard=[i for i in range(len(old)) if i not in paired_old],
                unmatched_current=[j for j in range(len(new)) if j not in paired_new],
                ambiguous_standard=[i for i, js in enumerate(candidates) if len(js) > 1 or
                                    (len(js) == 1 and len(reverse[js[0]]) > 1)])


def compare(standard, current, contract=None, current_legacy=False):
    contract = dict(contract or DEFAULT_CONTRACT)
    old, new = parse(standard, True, contract), parse(current, current_legacy, contract)
    old_failures, new_failures = audit(old, contract), audit(new, contract)
    matching = match_entries(old, new)
    contract_passed = bool(old and new) and not old_failures and not new_failures
    identical = bool(old and new) and not matching['unmatched_standard'] and not matching['unmatched_current'] and not any(
        pair['differences'] for pair in matching['pairs'])
    return dict(standard=str(standard), current=str(current), contract=contract,
                standard_sha256=hashlib.sha256(standard.read_bytes()).hexdigest(),
                current_sha256=hashlib.sha256(current.read_bytes()).hexdigest(),
                standard_count=len(old), current_count=len(new), standard_failures=old_failures,
                current_failures=new_failures, standard_entries=old, current_entries=new,
                matching=matching, metadata_consistency=metadata_audit(new),
                legacy_detection_scope='fiber_role_and_large_E_only_feed_not_source_trace',
                entry_contract_passed=bool(contract_passed),
                standard_entry_commands_identical=bool(identical),
                strict_match_passed=bool(contract_passed and identical))


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('standard', type=Path)
    cli.add_argument('current', type=Path)
    cli.add_argument('output', type=Path)
    cli.add_argument('--mode', choices=('strict', 'contract'), default='strict')
    cli.add_argument('--current-format', choices=('current', 'legacy'), default='current')
    for key, value in DEFAULT_CONTRACT.items():
        cli.add_argument('--'+key.replace('_', '-'), type=float, default=value)
    args = cli.parse_args()
    report = compare(args.standard, args.current, {key: getattr(args, key) for key in DEFAULT_CONTRACT},
                     current_legacy=args.current_format == 'legacy')
    report['current_format'] = args.current_format
    report['requested_mode'] = args.mode
    report['passed'] = report['strict_match_passed'] if args.mode == 'strict' else report['entry_contract_passed']
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({key: report[key] for key in ('standard_count', 'current_count', 'entry_contract_passed',
                                                  'standard_entry_commands_identical', 'strict_match_passed',
                                                  'requested_mode', 'passed')}, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
