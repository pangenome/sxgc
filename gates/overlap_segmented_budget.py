"""Offline segmented-front file ledger, verified against the 10 GB worker's actual character counters. No launch."""
import hashlib
import json
from pathlib import Path
import re
import sys
from functools import lru_cache
from overlap_segmented_gate import baseline,need,RECEIPTS,device_values

CHUNK=16<<20;SEG=1<<20;ROWS=1<<20;FAN=16
ceil=lambda x,y:(x+y-1)//y
def passes(runs):
    n=0
    while runs>FAN:runs=ceil(runs,FAN);n+=1
    return n
@lru_cache(maxsize=128)
def routed(m):
    if m<=ROWS:return 0
    span=ceil(m,64);q,r=divmod(m,span)
    return m+q*routed(span)+routed(r)

def ledger(n,m,longest,k=None,tape=None,runs=None):
    """Exactly two raw scans; intermediate runs copy all records (no dedup until final).
    Hypothetical 100 GB mode does not exist; this model is for the same partition/merge policy.
    """
    need(0<m<2**32 and m<=n<2**43 and 0<longest<=1<<20 and (k is None)==(tape is None),'width/dimensions')
    B=n+16*m-8;rmax=8+longest;lo=max(ceil(B,CHUNK),ceil(m,ROWS));hi=1+B//(CHUNK-rmax+1)+m//ROWS
    need(2<=lo<=hi<=65536,'external run-count not admitted by model')
    K=m if k is None else k;T=n+12*K-8 if tape is None else tape;need(0<K<=m and 0<T<=n+12*K-8,'dictionary bound')
    temp=8*routed(m);dup=8*ceil(m,64) if ceil(m,64)>ROWS else 0
    # Each of <=FAN input segments can remain charged while its consumed prefix has output.
    # In final merge a distinct record can expand by 4 B and no duplicate can expand.
    final=B+FAN*max(SEG,rmax)+4*K
    # Text-order routing duplicates at most the current non-leaf child file before retire.
    # Other rows occupy <=8 B each (or 4 B if already emitted). No overlapping tree path of duplicates.
    reorder=T+8*m+dup
    if runs is not None:need(lo<=runs<=hi,'observed run count');p0=p1=passes(runs)
    else:p0,p1=passes(lo),passes(hi)
    return dict(n=n,m=m,record_bytes=B,initial_run_bounds=[lo,hi],intermediate_passes=[p0,p1],private_segment_cap=max(SEG,rmax),
        run_read_or_write_bytes_bounds=[(1+p0)*B,(1+p1)*B],assignment_routing_read_or_write_bytes=temp,parse_bytes=4*m,tape_measured_or_upper=T,
        two_raw_scans_plus_runs_and_routes_logical_read_bounds=[2*n+(1+p)*B+temp for p in (p0,p1)],
        runs_routes_and_two_output_columns_logical_write_upper=(1+p1)*B+temp+T+4*m,merge_and_final_scratch_upper=final,
        ordinal_restore_scratch_upper=reorder,counted_file_payload_peak_upper=max(final,reorder),counted_limit=2*n,
        decoder_tape_plus_metadata_measured_or_upper=T+8*K+ceil(K,8),decoder_note='Payload only; no decoder grant, RSS, OS/page tables, or time inferred.')

def review(b):
    def get(p):return json.loads(p.read_bytes())
    def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
    source=get(b/'source.json');need(__debug__ and source['launcher_rc']==0 and set(source['artifacts'])==set('preflight.json binaries.log front.log segmented.json decode.log decode.json'.split()) and all(sha(b/p)==v for p,v in source['artifacts'].items()),'provenance')
    f=get(b/'segmented.json');d=get(b/'decode.json');v=f['census'];n=v['n'];m=v['m'];raw=get(RECEIPTS/'scanplan-10g/scan.json')['census'];base=get(RECEIPTS/'packed-front-10g/front-end.json')['census'];one=RECEIPTS/'segmented-front-1g';pair=lambda d:hashlib.sha256((d/'segmented.json').read_bytes()+(d/'decode.json').read_bytes()).hexdigest()
    need(pair(b)==source['pair_sha256'] and f['reviewed_one_sha256']==pair(one) and (b/'binaries.log').read_text().splitlines()[0]==source['revision']==f['revision'] and [z.split()[0] for z in (b/'binaries.log').read_text().splitlines()[1:3]]==[z.split()[0] for z in (one/'binaries.log').read_text().splitlines()[1:3]],'source or native pin')
    need(f['status']=='FRONT_COMPARED' and d['status']=='INTERMEDIATE_VERIFIED' and f['rc']==d['rc']==0 and f['promotion'] is d['promotion'] is False and d['decode_verified'] is True,'wrong status');need(f['raw_sha256']==d['raw_sha256']==raw['sha256']==f['expected_raw_sha256'] and f['mincore_resident_pages']==0 and set(d['cold_private_columns_zero_resident'])=={'dictionary.packed','parse.u32'},'cold/identity')
    for cg,ram in [(f['cgroup_front_after'],192<<20),(d['cgroup_after'],16<<30)]:need(cg['memory.max']==str(ram) and cg['cpu.max']=='100000 100000' and cg['memory.swap.max']=='0' and 0<int(cg['memory.peak'])<=ram and {'oom 0','oom_kill 0'}<=set(cg['memory.events'].splitlines()),'resource grant')
    need(all(v[key]==raw[key] for key in ('n','m','max_phrase','starts_above_u32','alphabet')) and all(v[key]==base[key] for key in ('n','m','k','descriptors','dictionary_bytes','D')) and v['parse_bytes']==4*m and v['parse_extent']==m+6*v['k'] and f['equal_column_sha256']['dictionary.packed']==get(RECEIPTS/'vocab-10g/verify.json')['packed_tape_sha256'] and re.fullmatch('[0-9a-f]{64}',f['equal_column_sha256']['parse.u32']),'independent column census')
    wall,io,sz=baseline('10g');need(f['wall_ratio']==f['front_wall_s']/wall and f['archived_front_io']==io and all(f['attributed_io_ratio'][k]==f['process'][k]/v and 0<f['process'][k]<=16*n for k,v in io.items()) and max(device_values(f['cgroup_front_io_delta']),default=0)<=16*n,'baseline/IO')
    x=ledger(n,m,v['max_phrase'],v['k'],v['dictionary_bytes'],v['initial_runs']);rd=x['two_raw_scans_plus_runs_and_routes_logical_read_bounds'][0];wr=x['runs_routes_and_two_output_columns_logical_write_upper']
    need(x['intermediate_passes']==[v['intermediate_passes']]*2 and sum(sz.values())<=v['peak_counted']<=x['counted_file_payload_peak_upper']<=2*n and 0<f['sampled_counted_logical']<=2*n and x['decoder_tape_plus_metadata_measured_or_upper']==d['metadata_limit']+sz['dictionary.packed'],'file ledger')
    need(f['process']['rchar']>=rd and f['process']['wchar']>=wr,'more modeled payload than worker issued');need(f['target_under_2x']==(f['wall_ratio']<2) and f['front_under_3x']==(f['wall_ratio']<3),'front score')
    bigscan=RECEIPTS/'scanplan-100g/scan.json';need(sha(bigscan)=='49370600ae37702965c4a736a2de118566e87c00f70cf487484366051094be69','100 GB raw-only pin');big=get(bigscan)['census'];y=ledger(big['n'],big['m'],big['max_phrase']);return dict(actual_10g_ledger=x,rchar_minus_modeled_input_bytes=f['process']['rchar']-rd,wchar_minus_modeled_output_bytes=f['process']['wchar']-wr,
        front_wall_per_byte_vs_segmented_1g=(f['front_wall_s']/n)/(get(one/'segmented.json')['front_wall_s']/get(one/'segmented.json')['n']),
        hypothetical_same_algorithm_on_measured_100g_shape=y,scope='10 GB component verified. 100 GB model uses measured n,m,max length, k<=m,U<=n; not a measured lexical build, decoder grant or end-to-end admission.')

if __name__=='__main__':print(json.dumps(review(Path(sys.argv[1])),sort_keys=True,indent=2))
