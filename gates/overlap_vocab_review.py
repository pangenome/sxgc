"""Replay the portable 10 GB vocab bank against independent small witnesses. No raw/tape reads or dispatch."""
import json
import math
from pathlib import Path
import re
import sys
import overlap_vocab_gate as gate
from overlap_defaults import decision
from overlap_ten_matched_score import metrics

need=gate.need
NAMES=set('census.out reference.out inputs.json front.json verify.json model.json fixtures.log binaries.log exits.log'.split())

def min_levels(B,buckets=256,segment=32<<20,fan=8):
    """Some bucket has at least ceil(B/(buckets*segment)) raw segments, even without skew."""
    need(B>0 and buckets>0 and segment>0 and fan>1,'invalid shape');q=(B+buckets*segment-1)//(buckets*segment)
    n=q;t=0
    while n>1:t+=1;n=(n+fan-1)//fan
    return dict(min_segments_in_largest_bucket=q,min_merge_levels=t,raw_bytes_in_buckets_needing_multilevel_at_least=max(0,B-buckets*segment*fan))

def check_io(rd,wr,caps):need(type(rd) is type(wr) is int and 0<=rd<=caps[0] and 0<=wr<=caps[1],'IO cap')
def metered(v,caps):
    need(v['rc']==0 and v['stop'] is None and v['rw_caps']==list(caps) and math.isfinite(v['wall_s']) and v['wall_s']>0 and v['process']['cpu_s']>0,'component exit/clock')
    for cg in (v['before'],v['after']):need((cg['memory.max'],cg['memory.swap.max'],cg['cpu.max'])==(str(192<<20),'0','100000 100000') and 0<int(cg['memory.peak'])<=192<<20 and {'oom 0','oom_kill 0'}<=set(cg['memory.events'].splitlines()),'component grant/OOM')
    need(v['device_io']==gate.io_delta(gate.stat_io(v['before']['io.stat']),gate.stat_io(v['after']['io.stat'])),'device IO mismatch');check_io(v['process']['read_bytes'],v['process']['write_bytes'],caps)
    for x in (v['device_io'] or {}).values():check_io(x['rbytes'],x['wbytes'],caps)

def review(b):
    load=lambda n:gate.load(b/n);s=load('source.json');d=Path(s['original_directory']);need(__debug__ and d.parent==gate.SANDBOX and re.fullmatch('vocab-10g-[A-Za-z0-9]{6}',d.name) and s['launcher_rc']==0 and not (b/'stop.requested').exists(),'not successful fixed census')
    need(s['artifacts'].keys()==NAMES and all((b/f).is_file() and not (b/f).is_symlink() and (b/f).stat().st_size<gate.RESERVE and gate.sha(b/f)==h for f,h in s['artifacts'].items()),'portable provenance')
    prog=(b/'binaries.log').read_text().splitlines();i=load('inputs.json');front=load('front.json');verify=load('verify.json');model=load('model.json');c,prior=gate.banks();n=c['n'];m=c['m'];p=gate.prospective(n,m,c['max_phrase'])
    need(prog[0]==i['revision']==s['revision'] and re.fullmatch('[0-9a-f]{40}',s['revision']) and s['native']=={k:prog[j+1].split('  ')[0] for j,k in enumerate(('census','reference'))} and all(f'{v}  {d/k}' in prog for k,v in s['native'].items()),'native pin')
    need((b/'exits.log').read_text().splitlines()==['census=0','reference=0'] and front['product'] is verify['product'] is False and front['status']=='RAW_CENSUS_COMPARED' and verify['status']=='VERIFIED_10G_VOCABULARY_ONLY' and verify['reference_cold_claim'] is False,'not calibrated')
    need(front['mincore_resident_pages']==0 and i['plan']==p and i['raw']==str(gate.SOURCE) and i['banked_tape']==str(gate.TAPE) and i['source_stat'][2]==front['private_bytes']==front['private_stat'][2]==n and i['source_stat'][:2]!=front['private_stat'][:2] and i['tape_stat'][2]==prior['dictionary_bytes'] and i['raw_sha256']==front['raw_sha256']==c['sha256'],'input pins')
    z=front['copy_cold'];cap=n+gate.MARGIN;need(z['read_cap']==z['write_cap']==cap and z['wall_s']>0 and z['worker_io']['write_bytes']>=n,'copy');check_io(z['worker_io']['read_bytes'],z['worker_io']['write_bytes'],(cap,cap))
    for x in (z['per_device_io'] or {}).values():check_io(x['rbytes'],x['wbytes'],(cap,cap))
    v,dc,oc=gate.readout(b/'census.out');need(v==front['summary'] and all(v[k]==c[k] for k in ('n','m','starts_above_u32','max_phrase','max_owned_stride','eof_start','eof_raw_length','global_end_rows','alphabet')) and oc==gate.counts(c['cells']),'raw occurrence witness')
    need((v['k'],v['U'],v['D'],v['tape'])==(prior['k'],prior['descriptors'],prior['D'],prior['dictionary_bytes']) and v['buckets']==256 and v['segment']==32<<20 and 0<v['raw_segments']<=v['files']<=p['created_segments_upper'] and 0<v['merge_levels']<=p['merge_levels_upper'],'census extent')
    need(p['first_pass_bytes']<=v['scratch_write']==v['scratch_read']<=p['logical_scratch_traffic_upper']//2 and v['peak']+gate.RESERVE==front['counted_peak_plus_control']<=p['counted_bound']<=2*n,'scratch ledger')
    metered(front['census'],(16*n,16*n));metered(verify['reference'],(v['tape']+gate.MARGIN,gate.MARGIN));need(front['census']['process']['read_bytes']>0 and front['census']['wall_s']<=gate.COMPONENT_CAP,'cold component/time')
    fields=(b/'reference.out').read_text().split();need(fields[0]=='VOCAB_REFERENCE','wrong reference');r=dict(x.split('=',1) for x in fields[1:]);need(all(int(r[k])==v[t] for k,t in [('k','k'),('descriptors','U'),('D','D'),('tape','tape'),('max_phrase','max_phrase')]) and verify['packed_tape_sha256']==r['tape_sha256'] and re.fullmatch('[0-9a-f]{64}',r['tape_sha256']),'reference extent')
    exact=gate.study(n,m,v['k'],v['U'],v['D'],dc,oc);exact['lexical_codec_and_navigation']=gate.lexical_block_model(n,m,v['k'],v['U'],v['max_phrase'],r);need(json.dumps(exact,sort_keys=True)==json.dumps(model,sort_keys=True) and verify['model_sha256']==s['artifacts']['model.json'] and front['out_sha256']==s['artifacts']['census.out'] and exact['row_cells_sha256']==front['row_digest'] and exact['distinct_cells_sha256']==front['distinct_digest']==r['descriptor_cells_sha256'],'tape/model witness')
    need(int(r['nonempty'])==sum(x!=0 for x in dc) and int(r['largest'])==max(dc),'descriptor cells')
    # Join two independent, previously banked measurements without a new payload read. Max_group is
    # the number of cursor-bearing descriptors in one exact finite key, NOT largest 2-code cell mass.
    profile=decision('10g')['profile'];w=metrics(gate.RECEIPTS/'scale-10g'/profile)['stream'];need(w['rows']==n+1 and w['descriptors']==v['U'] and 1<=w['max_group']<=max(dc),'matched producer witness')
    bigscan=gate.RECEIPTS/'scanplan-100g/scan.json';need(gate.sha(bigscan)=='49370600ae37702965c4a736a2de118566e87c00f70cf487484366051094be69','changed 100 GB scan');big=gate.load(bigscan)['census']
    return dict(scope='verified 10 GB component; scaled working sets and cursor costs are models, no 100 GB vocabulary, fit or time verdict',profile_for_measured_tie=profile,
        actual_10g=dict(n=n,m=m,k=v['k'],U=v['U'],D=v['D'],copy_cold_s=z['wall_s'],census_s=front['census']['wall_s'],separate_reference_s=verify['reference']['wall_s'],
            counted_scratch_with_margin=front['counted_peak_plus_control'],logical_scratch_read_plus_write=v['scratch_read']+v['scratch_write'],
            largest_two_code_descriptor_cell=model['largest_descriptor_cell'],largest_finite_group_descriptors=w['max_group'],compact_bytes_if_20_per_cursor_for_one_largest_finite_group=20*w['max_group'],
            finite_groups=w['groups'],singleton_group_lower=max(0,2*w['groups']-v['U']),minimum_largest_group_windows_if_8192_cursors=(w['max_group']+8191)//8192),
        certain_partition_work_at_100g_if_same_census=min_levels(big['n']+12*big['m']-8),
        codec={s:{'exact_wire_bytes_this_tape':a['payload']+a['directory'],'tenfold_only_emit_before_order_bytes':a['tenfold_sensitivity_only']['emit_before_order'],
            'tenfold_only_room_in_140gib_before_order_bytes':a['tenfold_sensitivity_only']['preorder_room']} for s,a in exact['lexical_codec_and_navigation']['lexical_blocks'].items()})

if __name__=='__main__':
    print(json.dumps(review(Path(sys.argv[1])),indent=2,sort_keys=True))
