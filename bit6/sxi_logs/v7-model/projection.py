"""Exact arithmetic for the intentionally optimistic v7 size model."""
import json
import pathlib

root = pathlib.Path(__file__).parent
run_member_bytes = {'yeast235': 98_951_370, 'pile-frag': 358_311_934,
                    'k10': 2_227_364_376}


def skeleton(r):
    return {'bitvector': (r + 7) // 8,
            'rank_directory': 8 * ((r + 511) // 512),
            'c_table': 2048}


def project(d):
    r, b = d['r'], d['bearing_runs']
    s, e = d['artifact_bytes'], d['ef_chi_bytes']
    sk = skeleton(r)
    # Rounded integer size of f*(S-E)+E+sk; no floating-point rounding.
    projected = ((s - e) * b + r // 2) // r + e + sum(sk.values())
    m1 = d.get('run_member_bytes', run_member_bytes.get(d['corpus']))
    n = d.get('n')
    lf_anchor_bytes = None if n is None else (b * (n - 1).bit_length() + 7) // 8
    full_runs_floor = None if m1 is None else \
        ((s - e - m1) * b + r // 2) // r + e + m1 + sum(sk.values())
    return {'corpus': d['corpus'], 'r': r, 'bearing_runs': b,
            'bearing_fraction': b / r, 'chi_over_r': d['chi'] / r,
            'v6_2_bytes': s, 'ef_chi_bytes': e, 'skeleton': sk,
            'projected_v7_bytes': projected,
            'optimistic_size_reduction': 1 - projected / s,
            'optimistic_compression_factor': s / projected,
            'full_run_table_floor_bytes': full_runs_floor,
            'explicit_lf_anchor_bytes': lf_anchor_bytes,
            'with_explicit_lf_anchors_bytes': None if lf_anchor_bytes is None
                else projected + lf_anchor_bytes,
            'full_v6_fallback_bytes': s + sum(sk.values())}


rows = [project(json.loads((root / f'{name}.result.json').read_text()))
        for name in ('yeast235', 'pile-frag', 'k10')]
frag = rows[1]
pile_r = 460_000_000_000
pile_s = (frag['v6_2_bytes'] * pile_r + frag['r'] // 2) // frag['r']
pile_e = (frag['ef_chi_bytes'] * pile_r + frag['r'] // 2) // frag['r']
pile_b = (frag['bearing_runs'] * pile_r + frag['r'] // 2) // frag['r']
pile_m1 = (run_member_bytes['pile-frag'] * pile_r + frag['r'] // 2) // frag['r']
pile = project({'corpus': 'pile-1.31TB-linear', 'r': pile_r, 'bearing_runs': pile_b,
                'artifact_bytes': pile_s, 'ef_chi_bytes': pile_e, 'n': 1_310_000_000_000,
                'run_member_bytes': pile_m1,
                'chi': (306_164_765 * pile_r + frag['r'] // 2) // frag['r']})
pile_n = 1_310_000_000_000
frag_n = 1_082_130_213
width_increase = (pile_n - 1).bit_length() - (frag_n - 1).bit_length() + \
                 (pile_r - 1).bit_length() - (frag['r'] - 1).bit_length()
# Member 8 packs one SA-width successor and one run-width ID per run.
phi_width_uplift = (pile_r * width_increase + 7) // 8
pile_width_adjusted = project({'corpus': 'pile-1.31TB-width-adjusted', 'r': pile_r,
                               'bearing_runs': pile_b,
                               'artifact_bytes': pile_s + phi_width_uplift,
                               'ef_chi_bytes': pile_e, 'n': pile_n,
                               'run_member_bytes': pile_m1,
                               'chi': (306_164_765 * pile_r + frag['r'] // 2) // frag['r']})
out = {'measured_artifact_projections': rows,
       'pile_linear_extrapolation': pile,
       'pile_width_adjusted_extrapolation': pile_width_adjusted,
       'pile_phi_width_uplift_bytes': phi_width_uplift,
       'r_466_skeleton': skeleton(2_739_737_289),
       'note': 'Lower bound only: assumes all non-chi bytes scale with bearing-run fraction; '
               'does not encode omitted run boundaries, symbol rank, select, or phi repair.'}
(root / 'projection.json').write_text(json.dumps(out, indent=2) + '\n')
print(json.dumps(out, indent=2))
