"""Close the two homogeneous residuals from *banked* evidence. No target open.
An existence certificate for alternative finite-key ranges, not a change to the
sampler, an observed EF extent, a physical fit check or an index-build profile.
"""
from collections import Counter
import gzip
import json
import os
from pathlib import Path
import re
import resource
import stat
import sys
import time

from overlap_finite_epoch64_study import upper as ef_upper
from overlap_m20_prefix_cuts import parse_dump, proof
from overlap_run_shells import shells
from overlap_run_shells_test import fixtures
from overlap_target_m20_cuts import R, CERT, DOCS, geometry
from overlap_target_prefix_certificate import N,M,SLOTS,pin,sha,stamp,need
from overlap_v10_product_compare import optional_io_stat

BANK=R/'target-m20-cuts'
PINS={'observations.gz':'614adfc1fa7f909419352fa497659f1e0e8dedf5a9f6d555c2834a80bfac0855',
      'result.json':'1d06033bb060050165bc7dac67bbf8de3e873f8e52a1c2014010c801e7641728'}

def receipt(name):
    p=BANK/name;need(p==p.resolve() and p.stat().st_size<=256<<10,'unexpected receipt extent')
    b=p.read_bytes();need(sha(b)==PINS[name],'receipt digest: '+name);return b


def certificate(guard):
    need((BANK/'exits.log').read_text()=='observer=0\nreview=0\nlauncher=0\n','archive verifier did not complete')
    z=json.loads(receipt('result.json')); data=gzip.decompress(receipt('observations.gz'))
    need(len(data)==z['observation_bytes']==483486 and sha(data)==z['observation_sha256'],'wrong dump')
    head,nodes=parse_dump(data);occ,lo,hi,hard=geometry(); guard()
    need([int(k) for k in (BANK/'requests').read_text().splitlines()]==hard,'observed parents do not match')
    max_phrase=json.loads(pin('scanplan-100g/scan.json'))['census']['max_phrase']
    need(max_phrase==201614,'changed scan horizon')
    policy=[];upper=[];cur=None;regular=0;raw_regular=0;residuals=[];max_tie=0;count=0
    def flush():
        nonlocal cur,regular
        if cur is not None:
            policy.append(cur);upper.append(cur[-1]);regular+=1;cur=None
    def visit(key,rank,rows,l,h):
        nonlocal cur,raw_regular,max_tie,count
        count+=1
        if count%1024==0:guard()
        if h<=SLOTS:
            if cur is not None and cur[-1]+h>SLOTS:flush()
            if cur is None:cur=['prefix',key,key,rank,rank,0,0]
            need(cur[4]==rank,'not consecutive certified raw extents');cur[2]=key;cur[4]+=rows;cur[5]+=l;cur[6]+=h
            raw_regular+=rows;max_tie=max(max_tie,h);return
        flush()
        need(len(key)==9 and len(set(key))==1 and key[0]>0,'residual is not homogeneous')
        x=key[0]-1;need(x!=30 and all(v['byte']!=x for v in residuals),'unsupported framing or duplicate residual')
        _,n,ch=nodes[bytes([x])*8];need(ch[key[0]][0]==rows and n==sum(v[0] for v in ch.values()),'wrong depth-eight histogram')
        exits=tuple(sum(v[0] for c,v in ch.items() if (c<key[0] if sign<0 else c>key[0])) for sign in (-1,1))
        need(sum(exits)+rows==n,'missing run exits');cuts=shells(x,rows,exits,max_phrase,SLOTS)
        byside=Counter();mass=0
        for cut in cuts:
            policy.append(['shell',x,cut.side,cut.first,cut.past,cut.upper]);upper.append(cut.upper);byside[cut.side]+=1;mass+=cut.upper
        max_tie=max(max_tie,min(rows,max(exits)))  # an exact key has ONE side and length
        residuals.append(dict(byte=x,first_parent_rank=rank,past_parent_rank=rank+rows,rows9=rows,exits8_low_including_END=exits[0],exits8_high=exits[1],
                              bands_low_high=[byside[-1],byside[1]],sum_individual_band_uppers_not_mass=mass,largest_shell_band_upper=max(c.upper for c in cuts)))
    check=proof(occ,lo,hi,DOCS,SLOTS,head,nodes,visit);flush()
    need(json.loads(json.dumps(check))==z['proof'] and regular==check['greedy_packs_across_only_consecutive_certified_leaves'] and raw_regular==check['raw_rows_in_certified_leaves'],'shallow proof did not replay')
    need(raw_regular+sum(s['rows9'] for s in residuals)==N and len(residuals)==2 and len(policy)==len(upper)<=16384 and max(upper)<=SLOTS and 0<max_tie<=SLOTS,'incomplete or oversized static range cover')
    wire=b''.join((json.dumps(p,separators=(',',':'))+'\n').encode() for p in policy);need(len(wire)<=128<<10,'static cut encoding cap')
    # Upper-packed groups: this is one legal wave schedule. A later exact
    # census can skip empty bands/repack consecutive nonempty jobs. A single
    # range taking a whole wave does not promise four-lane utilization.
    def waves():
        used=lanes=0;hist=Counter()
        for d in upper:
            if lanes==4 or used+d>SLOTS:hist[lanes]+=1;used=lanes=0
            need(0<d<=SLOTS,'range arena');used+=d;lanes+=1
        if lanes:hist[lanes]+=1
        return hist
    hist=waves();need(sum(k*v for k,v in hist.items())==len(upper),'wave coverage')
    dims=json.loads((CERT/'result.json').read_bytes())['result']['vocabulary_interval'];u=dims['U_upper'];k=dims['k_lower'];v=32*(M+(u-k)//32);b=len(policy);cap=N//2
    total=ef_upper(u,v,b);w=ef_upper(SLOTS,v,min(b,4));need(0<w<cap,'no positive epoch fill guarantee');epochs=(total+cap-w-1)//(cap-w)
    result=dict(shallow_result_sha256=PINS['result.json'],scan_max_phrase=max_phrase,n=N,slots=SLOTS,complete_alternative_cover=True,includes_suffix_at_N=False,global_END_rank_separate=0,
                prefix_packs=regular,rows_in_prefix_packs=raw_regular,shells=residuals,ranges_upper_including_potentially_empty=b,max_range_upper=max(upper),largest_indivisible_tie_upper=max_tie,
                conservative_wave_histogram_by_active_ranges=hist,wave_count_using_only_uppers=sum(hist.values()),table_sha256=sha(wire),table_jsonl_bytes=len(wire),
                post_census_ef_model=dict(m=M,k_lower=k,U_upper=u,virtual_universe_upper=v,posting_cap=cap,whole_wave_byte_upper=w,total_ef_byte_upper=total,nonlast_fill_strictly_above=cap-w,epoch_count_upper=epochs),
                scope='Static alternative partition and model only. No actual D per range, interior shell ranks, active lanes, target columns, posting bytes, physical allocation or runtime measured. Existing sampled splitter unchanged.')
    return result,wire


def main(d,end):
    need(__debug__ and d.parent==Path('/tmp') and d==d.resolve() and re.fullmatch(r'run-shell-proof-[A-Za-z0-9]{6}',d.name) and stat.S_IMODE(d.stat().st_mode)==0o700 and d.stat().st_uid==os.getuid() and time.time()<end<=time.time()+90,'private bounded receipts-only study')
    cg=Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().strip().split('::')[1]);get=lambda k:(cg/k).read_text().strip()
    need((get('cpu.max'),get('memory.max'),get('memory.swap.max'))==('100000 100000',str(192<<20),'0') and len(os.sched_getaffinity(0))==1,'receipt replay grant')
    def guard():
        usage=resource.getrusage(resource.RUSAGE_SELF);need(time.time()<end and not (d/'stop.requested').exists() and usage.ru_inblock*512<32<<20 and usage.ru_oublock*512<2<<20,'STOP/deadline/receipt-only IO cap')
    t=time.monotonic();io0=optional_io_stat(cg);guard();tested=fixtures();guard();a=time.monotonic();z,wire=certificate(guard);guard();b=time.monotonic()
    with (d/'range-table.jsonl').open('xb') as f:f.write(wire);f.flush();os.fsync(f.fileno())
    need((d/'range-table.jsonl').read_bytes()==wire,'cut-table handoff');events=get('memory.events');need({'oom 0','oom_kill 0'}<=set(events.splitlines()),'receipt replay OOM');guard();ru=resource.getrusage(resource.RUSAGE_SELF)
    result=dict(proof=z,generated= tested,utc=time.strftime('%FT%TZ',time.gmtime()),generated_s=a-t,target_receipt_replay_s=b-a,peak_shared=int(get('memory.peak')),memory_events=events,cpu_max=get('cpu.max'),cores=sorted(os.sched_getaffinity(0)),memory_cap=192<<20,swap_cap=0,
                attributed_R_W_before_result=[ru.ru_inblock*512,ru.ru_oublock*512],io_stat_before=io0,io_stat_after=optional_io_stat(cg),original_text_or_index_bytes_requested=0)
    stamp(d/'result.json',result);print(json.dumps(z,sort_keys=True))

if __name__=='__main__':
    need(len(sys.argv)==3,'use the private service');main(Path(sys.argv[1]),int(sys.argv[2]))
