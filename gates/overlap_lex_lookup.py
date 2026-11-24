"""Standalone fixed public-book/generated lookup study only. No corpus, suffix order, product or dispatch."""
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess as sp
import sys
import overlap_byte_fixture as fixed
from overlap_lex_direct_plan import wire,ten

need=fixed.need
PREFIX='lex-lookup-small-';SCRIPT='overlap_lex_lookup.sh';FLAG='--fixed-lookup-windows';DONE='CHECKED_RESIDENT_LOOKUPS_ONLY'
SHAPES=('book','wide','deep');SCHEDULES=('scatter','head','scan');MODES=('flat','restart','direct','direct','restart','flat')
MIN_QUERIES=1<<20

def admit(d,t):
    fixed.private(d,PREFIX);fixed.guard(d,t,120);fixed.lease(os.getppid(),t,SCRIPT);fixed.exclusive()
    rev=fixed.published_main(Path.home()/'sxgc')
    need(set(fixed.S.glob(PREFIX+'*'))=={d} and not any(d.iterdir()) and sp.check_output(['git','-C',fixed.REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',fixed.REPO,'status','--porcelain']),'prior attempt, dirty or unpublished source')
    need(fixed.memory_available()>=1<<30 and shutil.disk_usage(fixed.S).free>=256<<20 and fixed.BOOK.resolve()==fixed.BOOK and stat.S_ISREG(fixed.BOOK.lstat().st_mode) and fixed.BOOK.stat().st_size==fixed.SIZE,'book/host headroom')
    bounds=ten();need(bounds['direct_upper_over_uleb']<=1.10,'portable wire bound exceeds prerequisite')
    fixed.save(d/'inputs.json',dict(revision=rev,parent=os.getppid(),deadline=t,profile='resident single-symbol lookups only; no Pile input',book=str(fixed.BOOK),book_bytes=fixed.SIZE,book_sha256=fixed.DIGEST,windows=fixed.WINDOWS,width=fixed.WIDTH,wire_bound=bounds))

def measure(d,t):return fixed.measured_book_fixture(d,t,PREFIX,SCRIPT,FLAG,score,DONE)

def score(path):
    need(path.is_file() and not path.is_symlink() and path.stat().st_size<=32<<10,'fixture log size')
    lines=path.read_text().splitlines();need(len(lines)==66,'incomplete lookup fixture')
    def line(tag,pin,numbers):
        label,*fields=lines.pop(0).split();v=dict(f.split('=') for f in fields)
        need(label==tag and len(fields)==len(v) and set(v)==pin.keys()|set(numbers.split()) and all(v[k]==str(val) for k,val in pin.items()),'unexpected lookup record/order')
        out={k:(float(v[k]) if k.endswith('_s') else int(v[k])) for k in numbers.split()};need(all(math.isfinite(x) and x>=0 for x in out.values()),'negative/nonfinite clock/count');return out
    decks={};scatter={};vs_flat={};wire_ratio={}
    for shape in SHAPES:
        tag=dict(shape=shape);g=line('DECK',tag,'k raw owned literals flat restart direct');k,raw,own,lit=[g[j] for j in ('k','raw','owned','literals')];q=(k+15)//16
        need(0<k<=lit<=raw<=10_000_000-4*k and own==raw-8*(k-1) and g['flat']==raw+16+8*(k+1),'dictionary geometry')
        if shape=='book':need((k,own)==(5928,192164),'published window geometry')
        if shape=='wide':need(k==32768 and 9*k<=raw<=105*k,'seeded wide shape')
        if shape=='deep':need(k==16384 and raw==120*k and lit==q*(120+15*24),'seeded equal-prefix shape')
        w=wire(k,lit,1<<20,lit);need(w['base_min_bytes']<=g['direct']<=w['wire_upper'] and lit+8*(q+1)+(2*k-q)<=g['restart']<=lit+8*(q+1)+4*(2*k-q),'wire bounds')
        decks[shape]=dict(geometry=g,traces={});wire_ratio[shape]=g['direct']/g['restart'];vs_flat[shape]={}
        for how in SCHEDULES:
            sig=dict(tag,schedule=how);a=line('TRACE',sig,'queries cycles views restart_headers raw end absent hops max_hops literal_hits');n,cycles,h,hmax,found,first=[a[j] for j in ('queries','cycles','hops','max_hops','raw','literal_hits')]
            need(n=={'head':k,'scatter':own,'scan':raw+2*k}[how] and cycles==(MIN_QUERIES+n-1)//n and n==found+a['end']+a['absent'] and 0<=first<=found and found-first<=h<=hmax*(found-first) and 0<=hmax<=min(15,h) and (h==0)==(hmax==0),'query/hop ledger')
            views=k if how=='scan' else n;headers=a['restart_headers'];need(a['views']==views and views<=headers<=16*views,'view ledger')
            if how!='scatter':rem=k%16;need(headers==136*(k//16)+rem*(rem+1)//2,'restart headers')
            if how=='head':need(found==k and a['absent']==a['end']==0,'first byte contract')
            if how=='scan':need((found,a['end'],a['absent'])==(raw,1,2*k-1),'exhaustive word/END scan')
            if (shape,how)==('deep','head'):need(h==120*q and first==q and hmax==15,'counterexample must take all prefix steps')
            reps=[{},{}]
            for i,mode in enumerate(MODES):
                x=line('LOOKUP',dict(sig,rep=i//3,table=mode),'verified wall_s cpu_s');need(x['verified']==n*cycles and min(x['cpu_s'],x['wall_s'])>0,'timed verification');reps[i//3][mode]=x
            ratios={f'direct_over_{m}_{c}':[r['direct'][c]/r[m][c] for r in reps] for m in ('restart','flat') for c in ('wall_s','cpu_s')};vs_flat[shape][how]=max(ratios['direct_over_flat_wall_s'])
            if how=='scatter':scatter[shape]=max(ratios['direct_over_restart_wall_s'])
            decks[shape]['traces'][how]=dict(trace=a,observations=reps,**ratios)
    largest=max(x for row in vs_flat.values() for x in row.values());met=max(scatter['book'],scatter['wide'])<=2/3 and scatter['deep']<=1.1 and max(wire_ratio.values())<=1.1
    return dict(decks=decks,worst_scatter_direct_over_restart=scatter,worst_direct_over_flat_wall=vs_flat,direct_over_restart_wire=wire_ratio,scatter_hypothesis_met=met,
        all_lookup_workloads_under_2x_flat=largest<2,all_lookup_workloads_under_3x_flat=largest<3,product_promotion=False,scope='warm small resident lookup plus expected-symbol check only; no raw/prepared product clocks, physical locality or scale dispatch')

if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='score':print(json.dumps(score(Path(sys.argv[2])),sort_keys=True,indent=2))
    elif len(sys.argv)==4 and sys.argv[1] in ('admit','measure') and re.fullmatch(r'[1-9][0-9]{9}',sys.argv[3]):{'admit':admit,'measure':measure}[sys.argv[1]](Path(sys.argv[2]),int(sys.argv[3]))
    else:raise SystemExit('expected score FIXTURE_LOG or fixed launcher mode')
