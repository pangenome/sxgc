"""Inventory and credit model only. No builder/finish, raw reads, unlinks or scale dispatch.

extract: authenticate the frozen 100 MB v8 container/log and reduce its directories.
review: replay from the small banked reduction. See docs/overlap_finish_credits.md.
"""
from bisect import bisect_left
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

from overlap_resident_gate import ROOT, REPO, members

BANK = REPO/'gates/receipts'
HERE = BANK/'finish-credits-100m'
OLD = BANK/'external-product-100m'
D = ROOT/'sandbox/ext-product-QhQZxU'
N, R = 100_000_503, 38_647_521
ROW, RUN, BLOCK = 262144, 65536, 1024
FLUSH = 4 << 20
PRODUCT = '6325c843c0d9aa9be00d69260aafdd794936e2e79f16174efb00c43db831f341'
LOG = '75a6e89636bb85e28ddf801f59cf60f0031ed6c0806c1c862819c97c56c19295'
EPOCH = 'fc87f879608719e016221b6a383b2079e09d158885be494e09f90a2cafbc7cab'
REDUCTION = '7e5ae1e3158af9f5cc443cd001794264187cdbcb0ab1ec25a367c84580434c18'
RETAIN = ['aligned.u32','cells.bin','loc.u32','physical.refs','plan','pos-next.u32x2','pred.u8']


def need(ok, why):
    if not ok: raise ValueError(why)


def ceil(n, d): return (n+d-1)//d


def pin(path, size, digest):
    st = path.lstat()
    need(path.is_file() and path.resolve() == path and st.st_size == size, f'unexpected input {path}')
    with path.open('rb') as f: need(hashlib.file_digest(f,'sha256').hexdigest() == digest, f'SHA {path}')
    return st


def credits(rows, ends, copied, slice_rows):
    """ends[i]=first row of piece i (last entry=EOF); copied[i] is a LOWER
    bound on the exclusive row frontier after staging the previous piece.
    Counts are credits, NOT byte sizes or a time/scheduling prediction.
    """
    count = len(ends)-1
    need(count > 0 and slice_rows > 0 and len(copied) == count and ends[0] == copied[0] == 0 and ends[-1] == rows, 'frontier geometry')
    need(all(ends[i] < ends[i+1] and (0 if i<1 else copied[i-1]) <= copied[i] <= ends[i] for i in range(count)), 'nonmonotone frontier')
    sw, pw, necessary = [], [], []
    for i in range(count):
        # Internal piece is emitted on the first row of the NEXT run, before
        # feeding THAT row to SXLS. EOF is the opposite order (last slice first).
        before = ends[i+1]//slice_rows if i+1<count else ceil(rows,slice_rows)
        sw.append(before-ceil(copied[i],slice_rows))
        necessary.append(before-ceil(ends[i],slice_rows))
        # After piece i credit returns, at worst finish needs LCP up to its
        # exclusive row end, rounded UP to the last slice's publication.
        through = min(rows, ceil(ends[i+1],slice_rows)*slice_rows)
        # Count only internal piece cuts STRICTLY below `through` (lookahead).
        pieces_needed = bisect_left(ends, through, 1, count)-1
        pw.append(pieces_needed-(i+1))
    return dict(slice_sufficient=max(1,max(sw)), piece_sufficient=max(1,max(pw)),
                slice_necessary=max(1,max(necessary)), slice_worst_piece=sw.index(max(sw)),
                largest_piece_rows=max(b-a for a,b in zip(ends, ends[1:])))


def flushed(directory, payload, rows):
    """Mirror only the append boundary; never infer flushed bytes from member size alone."""
    pos = committed = 0; marks = []; blocks = ceil(rows,BLOCK)
    need(len(directory) == 16*blocks, 'm14 block count')
    for i,(offset,size,esc) in enumerate(struct.iter_unpack('<QII',directory)):
        need(offset == pos and size and size%8 == 0 and esc<=min(BLOCK, rows-i*BLOCK), 'm14 block geometry')
        pos += size
        if pos-committed >= FLUSH:
            committed = pos; marks.append([i+1,pos])
    need(pos == payload, 'm14 extent'); return marks


def releases(log, rows, runs):
    sync = None; sweep = True; groups = []; seen = 0; prev_bytes = 0
    for line in log.splitlines():
        if line.startswith('XSA_FINISH_PHASE sweep '):
            need(sweep, 'second sweep end'); sweep = False
        elif line.startswith('CONTAINER_SYNC '):
            m = re.fullmatch(r'CONTAINER_SYNC why=(piece-release|pre-close) member=m1 member_bytes=(\d+) unsynced_bytes=(\d+)', line)
            need(m and int(m[2])-prev_bytes == int(m[3])>0, 'sync coverage'); prev_bytes = int(m[2])
            sync = dict(during_sweep=sweep, lo=seen, hi=seen, extent=0, durable_m1=prev_bytes)
            groups.append(sync)
        elif line.startswith('PIECE_ABSORBED '):
            m = re.fullmatch(r'PIECE_ABSORBED idx=(\d+) container_off=(\d+) bytes=(\d+)',line)
            count = min(RUN, runs-seen*RUN)
            extent = 44+5*count+16*ceil(rows.bit_length()*count,64)
            need(sync and m and int(m[1])==seen and int(m[2])==sync['durable_m1'] and int(m[3])==extent and count>0 and sweep==sync['during_sweep'], 'piece coverage')
            seen+=1; sync['hi']=seen; sync['extent']+=extent
    need(not sweep and seen==ceil(runs,RUN) and all(z['hi']>z['lo'] for z in groups), 'missing release/phase'); return groups


def extract():
    # Fixed evidence reader, bounded to this one product. Hash the whole file;
    # seek only to the small m12 directory and m14 head/trailer for geometry.
    prod, logpath = D/'product.sxi2', D/'finish.log'
    st = pin(prod,107_519_008,PRODUCT); logstat = pin(logpath,66_989,LOG)
    z = members(prod); n = N+1; log = logpath.read_text()
    for line in ('XSA_FINISH_PHASE sweep ', f'ENDPOINT_STREAM_PASS n={n} k=1 r={R} sentinel_rows=1 seam_classes=0',
                 'XSA_FINISH_TIER lce_trust=0 ', f'CONTAINER_FINAL bytes={st.st_size} sha256={PRODUCT} members=9'):
        need(sum(l.startswith(line) for l in log.splitlines())==1, 'finish profile: '+line)
    groups = releases(log,n,R)
    need(groups[-1]['durable_m1']==z[1][2] and z[1][3]==z[14][3]==R, 'product/log mismatch')
    rc = json.loads((OLD/'product.json').read_text()); need(rc==json.loads((D/'product.json').read_text()) and rc['sha256']==PRODUCT and rc['rc']==[0,0,0] and rc['sink_reserve']==80000000 and not rc['stop'], 'external product receipt')
    retained = {k:(D/'in'/k).stat().st_size for k in RETAIN}
    plan = (OLD/'plan.log').read_text(); need(plan==(D/'plan.log').read_text() and f' counted_with_plan={sum(retained.values())} ' in plan, 'retained input plan')
    with prod.open('rb') as f:
        codec, off, size, blocks = z[12]; f.seek(off)
        b,h,r,rows,p,k,l2,l1=struct.unpack('<8Q', f.read(64))
        need((codec,rows,r,b,p)==(112,n,R,BLOCK,RUN) and blocks==h==ceil(r,b) and k==ceil(r,p) and l2==64 and l1==64+8*k and size==l1+8*h, 'm12 header')
        ends=list(struct.unpack(f'<{k}Q', f.read(8*k))); samples=struct.unpack(f'<{h}Q', f.read(8*h))
        need(ends==list(samples[::RUN//BLOCK]) and samples[0]==0 and samples[-1]<n and all(a<b for a,b in zip(samples,samples[1:])), 'm12 sample alignment')
        copied=[samples[max(0,i*RUN-2)//BLOCK] for i in range(k)]
        codec,off,size,count=z[14]; f.seek(off); need(struct.unpack('<4QII',f.read(40))==(BLOCK,n,40,0,1,117) and codec==117 and count==R, 'm14 header')
        payload=size-40-16*h; f.seek(off+40+payload); flush=flushed(f.read(16*h),payload,r)
    # Ignore atime; refuse replacement or mutation during the reduction.
    for path,before in ((prod,st),(logpath,logstat)):
        after=path.stat(); need((after.st_dev,after.st_ino,after.st_size,after.st_ctime_ns)==(before.st_dev,before.st_ino,before.st_size,before.st_ctime_ns), 'original changed')
    sx=json.loads((BANK/'sxls-pair-100m/codec.json').read_text())['trial']; need(sx['exact_bytes']==1 and sx['rows']==n and sx['slices']==ceil(n,ROW) and f"escapes={sx['escapes']} max_lcp=" in log, 'unmatched slice evidence')
    sl=ROW+16*sx['max_slice_escapes']; need(f' max_slice_bytes={sl} ' in log, 'largest SXLS mismatch')
    return dict(rows=n,runs=R,product_sha256=PRODUCT,finish_log_sha256=LOG,retained=retained,
                piece_start=ends+[n],copied_lower=copied,flush_blocks_bytes=flush,release_groups=groups,
                lcp_payload=payload,sparse=8*h,m5_member=z[5][2],slice_full_upper=sl+48,
                external_sink_budget=rc['sink_reserve'])


def review(e):
    n=e['rows']; runs=e['runs']; need((n,runs,e['product_sha256'],e['finish_log_sha256'])==(N+1,R,PRODUCT,LOG),'wrong scope')
    epoch_path=BANK/'finite-epoch64-100m/score.json'; need(hashlib.sha256(epoch_path.read_bytes()).hexdigest()==EPOCH,'wrong epoch'); epoch=json.loads(epoch_path.read_text()); held=epoch['remaining_reserved_for_other_stages']
    need(epoch['component_goal_met'] and epoch['preflight_stage_files_upper']+held==2*N,'unmatched reservation')
    c=credits(n,e['piece_start'],e['copied_lower'],ROW); bitmap=ceil(n,8); grid=lambda tau:40+8*(n//tau+1)
    floors=[]
    for z in e['release_groups']:
        if not z['during_sweep']: continue # PFCK is not needed by m5
        nr=z['hi']*RUN-2; need(0<nr<runs and z['hi']-z['lo']==64, 'not full sweep release')
        parts=dict(pieces=z['extent'],pfck=grid(22),bitmap=bitmap,sparse=8*ceil(nr,BLOCK),
                   lcp_flushed=max([0]+[v for k,v in e['flush_blocks_bytes'] if k<=nr//BLOCK]))
        floors.append(dict(lo=z['lo'],hi=z['hi'],verified_runs_lower=nr,parts=parts,bytes=sum(parts.values())))
    peak=max(floors,key=lambda v:v['bytes']); base=sum(e['retained'].values())
    # Keep eight reader/frontier slots over the 64-release batch, as in
    # the matched product plans. One partial publish is in the writer window.
    # Immutable same-row geometry, before introducing an online byte fence.
    P,S=6,5; need(P>=c['piece_sufficient'] and S>=c['slice_sufficient'],'credit cycle possible')
    piece=44+5*RUN+16*ceil(n.bit_length()*RUN,64)+36
    common=dict(bitmap=bitmap,sparse=e['sparse'],lcpruns=e['lcp_payload'],control_and_bounded_transport=2<<20)
    sweep=dict(common,pieces=(P+64+8)*piece,slices=S*e['slice_full_upper'])
    # Force-release, close and unlink all pieces at m1 close BEFORE m5 BitsOut.
    # Still charge PFCK and bitmap for all of both phases, no unlink discount.
    close=dict(common,ef_spools=e['m5_member']); trials=[]
    for tau in (22,64,128):
        sp=dict(sweep,pfck=grid(tau)); cp=dict(close,pfck=grid(tau)); bound=max(sum(sp.values()),sum(cp.values()))
        trials.append(dict(tau=tau,piece_window=P,slice_window=S,sweep=sp,post_m1=cp,consumer_model_upper=bound,slack_vs_held=held-bound))
    return dict(historical_floor=peak,sweep_release_floors=[x['bytes'] for x in floors],retained_prepared_files=base,
                historical_joint_lower=base+peak['bytes'],joint_over_2n=base+peak['bytes']-2*N,
                shortfall_vs_epoch_finish_reserve=peak['bytes']-held,epoch_held=held,credits=c,models=trials,
                scope='Exact v8 historical lower bound and conditional credit/extent models on the identical row stream. Not a measured new finish, hard quota, RAM proof, v10 model or launch gate.')


if __name__=='__main__':
    need(__debug__ and sys.argv[1:] in (['extract'],['review']), 'extract|review; no payload launcher')
    if sys.argv[1]=='extract': result=extract()
    else:
        data=(HERE/'reduction.json').read_bytes(); need(hashlib.sha256(data).hexdigest()==REDUCTION,'changed reduction')
        result=review(json.loads(data))
    print(json.dumps(result,sort_keys=True,indent=2))
