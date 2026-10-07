#!/usr/bin/env python3
"""Independent final G-code material check. Requires Shapely >= 2.

The oracle uses GEOS, not slicer's integer domain/offset helpers. G-code is
quantized to .001 mm; .002 mm contact resolution and .003 mm intrusion depth
allowance cover both reconstructed boundaries. No total-area waiver is used.

Exit status checks intrusion, source availability and optional fiber identity only.
Coverage is reported but NOT accepted: its pattern-specific contract is pending.
The restored baseline is expected to fail intrusion checks; do not use WILL_FAIL.
"""
import argparse
import hashlib
import json
import math
import re
from collections import defaultdict
from pathlib import Path
from shapely.geometry import LineString, Polygon, GeometryCollection, box
from shapely.ops import unary_union
from shapely.prepared import prep

EMPTY = GeometryCollection()

def sweep(points, radius):
    return LineString(points).buffer(radius, cap_style=2, join_style=1, quad_segs=64)

def parse(folder):
    config = json.loads((folder / 'effective_config.json').read_text())
    offsets = [tuple(map(float, s.split('x'))) for s in config['extruder_offset'].split(',')]
    layers = defaultdict(lambda: {'fiber': [], 'resin': []})
    xy = [0., 0.]
    e = 0.
    layer = 0
    tool = 0
    width = .42
    role = ''
    active = deposit = False
    relative_e = False
    absolute = True
    block = None
    resin_run = 0
    for number, raw in enumerate((folder / 'model.gcode').open(), 1):
        line = raw.strip()
        if line.startswith(';LAYER_CHANGE'): layer += 1
        if line.startswith(';TYPE:'): role = line[6:]
        if line.startswith(';WIDTH:'): width = float(line[7:])
        if line.startswith(';FIBER_BEGIN '):
            active = True
            deposit = False
            block = {'meta': dict(re.findall(r'(\w+)=([^ ]+)', line)), 'points': []}
        if line in (';FIBER_LANDING_BEGIN', ';FIBER_START', ';FIBER_TAIL_BEGIN'):
            deposit = True
            if not block['points']:
                block['points'].append([xy[i] + offsets[tool][i] for i in (0, 1)])
        if line in (';FIBER_DEPLETED', ';FIBER_FINISH_BEGIN'): deposit = False
        if line == ';FIBER_END':
            if len(block['points']) > 1: layers[layer]['fiber'].append(block)
            active = deposit = False
        words = line.split(';')[0].split()
        if not words: continue
        op = words[0]
        if re.fullmatch(r'T\d+', op): tool = int(op[1:])
        if op == 'G90': absolute = True
        if op == 'G91': absolute = False
        if op == 'M83': relative_e = True
        if op == 'M82': relative_e = False
        if op in ('G2','G3'): raise AssertionError('Arc G-code requires explicit oracle expansion')
        if op not in ('G0', 'G1', 'G92'): continue
        v = {w[0]: float(w[1:]) for w in words[1:] if w[0] in 'XYZE' and len(w) > 1}
        end = [v.get(k, xy[i]) if absolute or op == 'G92' else xy[i] + v.get(k, 0) for i, k in enumerate('XY')]
        de = v.get('E', 0) if relative_e else v.get('E', e) - e
        if op == 'G92': e = v.get('E', e)
        elif 'E' in v: e = e + v['E'] if relative_e else v['E']
        if op != 'G92' and xy != end and layer:
            a = [xy[i] + offsets[tool][i] for i in (0, 1)]
            b = [end[i] + offsets[tool][i] for i in (0, 1)]
            if active and deposit: block['points'].append(b)
            elif not active and de > 0 and role in ('Resin infill', 'Gap infill'):
                layers[layer]['resin'].append({'points': [a, b], 'width': width, 'line': number, 'role': role, 'run': resin_run})
        if op == 'G92' or (op in ('G0','G1') and (active or de <= 0) and xy != end):
            resin_run += 1
        xy = end
    return config, layers

def resin_paths(segments):
    # Preserve round joins along each uninterrupted, constant-width extrusion.
    # Sweeping every G1 separately with flat caps would omit material at bends.
    paths = []
    for segment in segments:
        if (paths and paths[-1]['run'] == segment['run'] and
            paths[-1]['width'] == segment['width'] and paths[-1]['role'] == segment['role'] and
            paths[-1]['points'][-1] == segment['points'][0]):
            paths[-1]['points'].append(segment['points'][1])
        else:
            paths.append(dict(segment, points=list(segment['points'])))
    return paths

def protected_domain(area, physical, overlap):
    # Explicit quantization budget: contact resolution affects overlap seeds
    # only. Raw residual and true physical material are never relabelled.
    residual = area.difference(physical)
    contact = physical.union(physical.buffer(.002, quad_segs=32).buffer(-.002, quad_segs=32)).intersection(area)
    seeds = area.difference(contact)
    resin = residual.union(seeds.buffer(overlap, quad_segs=64).intersection(area)) if overlap else residual
    return area.difference(resin)

def inspect(folder, baseline=None, expected_layers=None):
    config, layers = parse(folder)
    domains = defaultdict(list)
    for d in json.loads((folder / 'domains.json').read_text()):
        if d['kind'] == 1:
            rings = d['boundaries']
            domains[d['layer']].append(Polygon(rings[0], rings[1:]).buffer(0))
    baseline_layers = parse(baseline)[1] if baseline else None
    report = {'scope': 'fiber_intrusion_and_optional_fiber_identity_only',
              'coverage_acceptance': 'not_evaluated', 'passed': True, 'layers': [], 'quantization_intrusion_depth_mm': .003}
    layer_count = json.loads((folder / 'report.json').read_text())['layer_count']
    if expected_layers is not None and layer_count != expected_layers: report['passed'] = False
    report['layer_count'] = layer_count
    for layer, regions in sorted(domains.items()):
        data = layers[layer]
        area = unary_union(regions)
        physical = unary_union([sweep(b['points'], float(config['fiber_width']) / 2) for b in data['fiber']]).intersection(area)
        protected = protected_domain(area, physical, float(config['fiber_resin_overlap'])).buffer(-.003, quad_segs=32)
        protected_index = prep(protected)
        resin = []
        violations = []
        for s in resin_paths(data['resin']):
            footprint = sweep(s['points'], s['width'] / 2)
            resin.append(footprint)
            if not protected_index.intersects(footprint): continue
            intrusion = footprint.intersection(protected)
            if not intrusion.is_empty and intrusion.area > 1e-10:
                violations.append({'line': s['line'], 'length_mm': LineString(s['points']).length, 'intrusion_mm2': intrusion.area, 'bounds': intrusion.bounds})
        actual = unary_union(resin)
        residual = area.difference(physical)
        fiber_signature = sorted(hashlib.sha256(json.dumps(b['points']).encode()).hexdigest() for b in data['fiber'])
        unchanged = True
        if baseline_layers is not None:
            before = sorted(hashlib.sha256(json.dumps(b['points']).encode()).hexdigest() for b in baseline_layers[layer]['fiber'])
            unchanged = before == fiber_signature
        row = {'layer': layer, 'fiber_blocks': len(data['fiber']), 'fiber_unchanged': unchanged,
               'fiber_length_mm': sum(LineString(b['points']).length for b in data['fiber']),
               'resin_length_mm': sum(math.dist(*s['points']) for s in data['resin']),
               'physical_mm2': physical.area, 'residual_mm2': residual.area,
               'residual_covered_mm2': residual.intersection(actual).area,
               'uncovered_residual_mm2': residual.difference(actual).area, 'violations': violations,
               'passed': not violations and unchanged}
        printable_core = residual.buffer(-.42, quad_segs=32)
        row['printable_core_mm2'] = printable_core.area
        row['uncovered_printable_core_mm2'] = printable_core.difference(actual.buffer(.003)).area
        # Coverage is diagnostic until pattern-specific acceptance is designed
        # independently. Do not tune thresholds around the experimental output.
        row['coverage_acceptance'] = 'not_evaluated'
        print('layer', layer, 'violations', len(violations), 'fiber unchanged', unchanged, flush=True)
        report['layers'].append(row)
        report['passed'] &= row['passed']
    no_fiber = config['generate_reinforced_infills'] == '0' and config['generate_reinforced_perimeters'] == '0'
    report['passed'] &= (not any(d['fiber'] for d in layers.values())) if no_fiber else bool(domains)
    (folder / 'resin-check.json').write_text(json.dumps(report, indent=2) + '\n')
    print(('PASS' if report['passed'] else 'FAIL'), folder, 'layers', len(domains),
          'violations', sum(len(r['violations']) for r in report['layers']),
          'changed fiber layers', [r['layer'] for r in report['layers'] if not r['fiber_unchanged']])
    return report['passed']

def self_test():
    area = box(0, 0, 40, 4)
    physical = box(0, 0, 40, 3)
    protected = protected_domain(area, physical, .05).buffer(-.003)
    bad = sweep([(1,1), (39,1)], .21)
    good = sweep([(1,3.5), (39,3.5)], .21)
    assert bad.intersection(protected).area > 10
    assert good.intersection(protected).is_empty
    legacy = unary_union([box(0,y,40,y+1).buffer(-.05) for y in range(3)])
    assert area.difference(legacy).intersection(box(1,.98,39,1.02)).area > 1
    assert protected_domain(area,area,.05).area == area.area
    # Preserve real residuals, holes and finite-width safety independently of
    # the removed production planner. These controls do not tune pass coverage.
    holed = box(0,0,10,10).difference(box(4,4,6,6))
    protected = protected_domain(holed, holed.intersection(box(0,0,10,8)), .05)
    assert protected.intersection(box(4,4,6,6)).area == 0
    assert protected.intersection(box(0,8.1,10,10)).is_empty
    strips = unary_union([box(0,0,10,2),box(0,2.1,10,4)])
    guard = protected_domain(box(0,0,10,4),strips,0).buffer(-.003)
    assert sweep([(1,2.05),(9,2.05)],.21).intersection(guard).area > 0
    assert sweep([(1,2.05),(9,2.05)],.02).intersection(guard).is_empty
    core = box(0,0,10,3)
    omitted = box(4,0,6,3)
    assert core.difference(core.difference(omitted)).area == 6
    print('PASS checker negative controls: injected strand, legacy erosion, omitted pocket')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('folder', nargs='?', type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--expected-layers', type=int)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test: self_test()
    else: raise SystemExit(0 if inspect(args.folder,args.baseline,args.expected_layers) else 1)
