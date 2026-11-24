"""Review a completed scan-only receipt and its historical admission commit. No launch or large-file IO."""
import json
import math
import os
from pathlib import Path
import re
import subprocess as sp
import sys
import overlap_scanplan_hundred as h
from overlap_vocab_gate import prospective
from overlap_vocab_plan import counts,digest

need=h.need;N=100_000_000_000;U32=(1<<32)-1;I32=(1<<31)-1

def dimensions(c):
    n=c['n'];m=c['m'];p=c['max_phrase'];stride=c['max_owned_stride'];start=c['eof_start'];high=c['starts_above_u32'];cells=c['cells']
    for k in ('n','m','max_phrase','max_owned_stride','eof_start','eof_raw_length','starts_above_u32','global_end_rows','alphabet'):
        need(type(c[k]) is int,'noninteger census: '+k)
    need(n>=8 and 1<=m<=n-7 and 0<=start<=n-8 and c['eof_raw_length']==n-start<=stride<=p<=n and p-stride<=8 and stride*m>=n,'trigger/coverage bounds')
    need(max(0,m-U32-1)<=high<=min(m-1,max(0,n-8-U32)) and (high>0)==(start>U32),'wrong wide starts')
    v=counts(cells);need(sum(v)==n and sum(v[::257])==c['global_end_rows']==1 and len({i//257 for i,w in cells})==c['alphabet'],'row cells or END')
    cell,peak=max(cells,key=lambda pair:pair[1]);return dict(n=n,m=m,max_raw_phrase=p,max_stride=stride,eof_start=start,eof_owned=n-start,starts_above_u32=high,
        raw_positions_above_u32_including_END=max(0,n-U32),row43_extent_ok=n<2**43,encoded_bytes_fit_254_alphabet=c['alphabet']<=254,
        alphabet=c['alphabet'],nonempty_occurrence_cells=len(cells),occurrence_histogram_sha256=digest(v),largest_occurrence_cell=cell,largest_occurrence_mass=peak,
        # This many slots suffices for any single 2-symbol descriptor cell, not a measured minimum.
        conservative_unsplit_cell_arena_bytes=8*peak,separate_worst_complete_group_cursors_bytes=20*min(m,peak),
        observed_m_allows_u32_sampled_ranks=m<U32,narrow_parse_impossible_from_m_alone=m>=1<<31,
        largest_k_for_i32_extent=(I32-m)//6 if I32-m>=6 else None,
        lexical_k_upper=m,U_lower=max(stride,len(cells)),U_upper=n,D_upper=n+9*m-7,packed_tape_upper=n+12*m-8,
        hypothetical_largest_k_for_140gib_parse_arrays={str(q):max(0,min(m,((140<<30)-20*m)//(q*8))) for q in (4,6)},
        parse_ids_bytes=4*m,largest_tape_plus_parse_ids_bound=n+16*m-8)

def phase(s,name):
    p=s[name];rc=N+h.gate.MARGIN;wc=rc if name=='copy_cold' else h.gate.MARGIN
    need((p['read_cap'],p['write_cap'])==(rc,wc) and 0<p['wall_s']<86400 and 0<p['worker_cpu_s']<86400,'invalid phase accounting')
    def io(rd,wr):need(type(rd) is type(wr) is int and 0<=rd<=rc and 0<=wr<=wc,'IO cap')
    io(p['worker_io']['read_bytes'],p['worker_io']['write_bytes'])
    need(p['worker_io']['write_bytes']>=N if name=='copy_cold' else p['worker_io']['read_bytes']>0,'missing worker IO')
    if p['per_device_io'] is not None:
        need(isinstance(p['per_device_io'],dict) and bool(p['per_device_io']),'empty device evidence')
        for k,v in p['per_device_io'].items():need(re.fullmatch(r'\d+:\d+',k),'device key');io(v['rbytes'],v['wbytes'])
    return p

def blob(rev,path):
    spec=f'{rev}:{path}';need(int(h.git('cat-file','-s',spec))<=8<<20 and h.git('cat-file','-t',spec)=='blob','oversize/not a receipt')
    return sp.check_output(['git','-C',h.ROOT,'cat-file','blob',spec])

def verify(s):
    need(__debug__ and s['status']=='MEASURED_100G_SCAN_ONLY' and 'error' not in s and s['launch_authorized'] is s['product_promotion'] is False,'not a successful scan-only receipt')
    a=s['admission'];rev=a['commit'];need(re.fullmatch('[0-9a-f]{40}',rev),'invalid published revision');h.git('merge-base','--is-ancestor',rev,'HEAD')
    need(a['receipt_tree']==h.git('rev-parse',rev+':gates/receipts'),'admitted bank changed')
    paths=[*h.DOCS,h.DEFAULT,h.ONE_DEFAULT,h.SCAN,h.FRONT];files={p:blob(rev,p) for p in paths};need(a['hashes']=={p:h.sha(v) for p,v in files.items()} and a['hashes'][h.SCAN]==h.CALIBRATION_SHA and a['hashes'][h.FRONT]==h.FRONT_SHA,'admitted evidence differs')
    choice=json.loads(files[h.DEFAULT]);one=json.loads(files[h.ONE_DEFAULT]);docs=files[h.DOCS[0]].decode()
    for rung,v,p in [('1g',one,h.ONE_DEFAULT),('10g',choice,h.DEFAULT)]:h.verify_default(files[p],docs,h.defaults.decision(rung),rung)
    need(choice['profile']==a['profile'] and choice['normalized_speed_target_met'] is a['under_2x'],'selected profile')
    cal=json.loads(files[h.SCAN]);t=h.calibration_clock(cal,json.loads(files[h.FRONT])['census']);margin=math.ceil(15*t+180)
    need(a['calibration_clock_s']==t and a['required_s']==margin and s['brute_fixture']=='passed' and s['kernel_sha256']==a['calibration_kernel']==cal['kernel_sha256'],'kernel/calibration')
    need(s['metadata_match'] is s['immutable_read_fd'] is True and s['source']==str(h.contract.ROOT/'pile-head100.txt') and
         s['census']['sha256']==s['copy_sha256']==s['manifest_sha256']==h.contract.CORPUS and s['census']['n']==s['source_stat'][2]==s['private_copy_budget']==s['private_copy_bytes']==N,'input/identity')
    need(s['free_before']>=N+(1<<30) and s['free_after_scan']>=h.gate.MARGIN and s['mincore_resident_pages']==0 and s['mincore_checked_pages']==math.ceil(N/os.sysconf('SC_PAGE_SIZE')),'cold/free')
    need(0<=s['private_copy_allocated']<=s['whole_run_allocated_before_receipt'] and s['counted_column_scratch_bytes']==0 and s['conservative_scan_reservation']==h.reservation(N)<=s['counted_limit']==2*N,'accounting')
    for k in ('cgroup_before','cgroup_after'):h.cg_check(s[k])
    c=s['census'];d=dimensions(c);need(s['models']==[h.plan(c,x) for x in (256<<20,1<<30)],'range model mismatch')
    prep,scan=(phase(s,k) for k in ('copy_cold','raw_scan'));wall=prep['wall_s']+scan['wall_s'];end=s['closed_unix_ns'];start=s['opened_unix_ns'];ns=10**9
    need(type(end) is type(start) is int and 0<start<end<=s['deadline']*ns<=h.CUTOFF*ns and (end-start)/ns+.5>=wall and start+margin*ns<=s['deadline']*ns,'elapsed/admission clock')
    left=(h.CUTOFF*ns-end)/ns;linear=10*choice['raw_s'];need(0<linear and left>=0,'time model')
    try:vocab=prospective(N,c['m'],c['max_phrase'])
    except RuntimeError as e:vocab=dict(refused=str(e))
    return dict(scope='100 GB immutable raw scan only. No distinct vocabulary, product, default or Pile launch.',admitted_revision=rev,
        raw_sha256=h.contract.CORPUS,default_10g=choice['profile'],dimensions=d,copy_and_cold=prep,raw_scan=scan,
        scan_wall_per_byte_vs_10g=scan['wall_s']/(10*cal['raw_scan']['wall_s']),copy_plus_scan_per_byte_vs_10g=wall/(10*t),
        actual_m_over_tenfold_10g=c['m']/(10*cal['census']['m']),copy_inclusive_reservation=s['conservative_scan_reservation'],whole_private_allocated=s['whole_run_allocated_before_receipt'],
        conservative_whole_occurrence_ranges={str(sz):(None if (r:=h.plan(c,sz))['ranges'] is None else len(r['ranges'])) for sz in (256<<20,1<<30,2<<30,4<<30,16<<30,32<<30)},
        hypothetical_vocab_file_ledger_only=vocab,tenfold_measured_v8_wall_s=linear,seconds_left_at_scan_close=left,
        speed_factor_needed_vs_linear_just_to_fit_clock=linear/left if left>0 else None,
        note='10× measured v8 time is a sensitivity, not a v10 ETA or impossibility proof. 140 GiB is the 10 GB array comparison only. k and descriptor loads require full-word dedup; an occurrence upper-bound refusal does not prove a large tie. No admitted 100 GB vocabulary backend or measured full product fit.')

if __name__=='__main__':
    p=Path(sys.argv[1]);need(len(sys.argv)==2 and not p.is_symlink() and p.is_file() and p.stat().st_size<=8<<20,'expected small scan.json')
    print(json.dumps(verify(json.loads(p.read_bytes())),indent=2,sort_keys=True))
