#!/usr/bin/env python3
"""Model committed-prefix spools and ordered segment reduction, not durability."""
import json, random, threading, time
from concurrent.futures import ThreadPoolExecutor

def trial(seed):
    rng = random.Random(seed)
    p = [1,4,8,16][seed%4]
    n = 2400 if seed%50==0 else rng.randrange(0,250)
    # Identical BWT characters across producer seams, singleton targets,
    # repeated cuts, and empty ranges are all legal.
    chars = [i%3 for i in range(n)] if seed%50==0 else [rng.randrange(3) if rng.randrange(3)==0 else 0 for _ in range(n)]
    positions = list(range(n)); rng.shuffle(positions)
    targets = set(rng.sample(positions,min(n,rng.randrange(25))))
    cuts = sorted([0,n]+[rng.randrange(n+1) for _ in range(p-1)])
    cv = threading.Condition()
    disk = [[] for _ in range(p)]
    used = [0]*p; quota = 3 * (1 + seed%4); active = [0]; blocked = [0]*p
    committed = [0]*p; done = [False]*p
    actual = []; observed = []; errors = []
    bands = []; acked = []; terminal = [False]; peaks = [0,0]; completion = []
    def downstream():
        # Adversarially withhold ACKs until W is exhausted. This consumer
        # depends on canonical bands only, never on upstream range completion.
        if n==2400:
            with cv: cv.wait_for(lambda: len(bands)==1100 or terminal[0])
        while True:
            with cv:
                cv.wait_for(lambda: bands or terminal[0])
                if not bands: break
                acked.append(bands.pop(0)); cv.notify_all()
            if len(acked)%17==0: time.sleep(.00001)
    def producer(i):
        records = []; pending = None
        def flush():
            nonlocal pending
            if pending: records.append(tuple(pending)); pending = None
        for row in range(cuts[i],cuts[i+1]):
            pos,c = positions[row],chars[row]
            target = pos in targets
            if target or (pending and pending[0]!=c): flush()
            if pending is None: pending = [c,0,pos,pos]
            pending[1]+=1; pending[3]=pos
            if target: flush()
        flush()
        # Append is not publication. Advance only after a complete window.
        for start in range(0,len(records),3):
            batch = records[start:start+3]
            with cv:
                while sum(used)+len(batch)>quota-(0 if i==active[0] else 3):
                    blocked[i]+=1; cv.wait()
                used[i]+=len(batch); peaks[0]=max(peaks[0],sum(used))
                assert sum(used)<=quota
            disk[i].extend(batch)
            if (seed+i+start)%7==0: time.sleep(.00001)
            with cv:
                committed[i]+=len(batch); cv.notify_all()
        with cv: done[i]=True; completion.append(i); cv.notify_all()
    def reducer():
        row=0
        for i in range(p):
            record=0
            assert row==cuts[i]
            with cv: active[0]=i; cv.notify_all()
            while True:
                with cv:
                    cv.wait_for(lambda: committed[i]>record or done[i])
                    available,ended=committed[i],done[i]
                for c,length,head,tail in disk[i][record:available]:
                    assert length>0 and row+length<=cuts[i+1]
                    segment=positions[row:row+length]
                    assert segment[0]==head and segment[-1]==tail
                    assert all(value==c for value in chars[row:row+length])
                    assert length==1 or not targets.intersection(segment)
                    with cv:
                        cv.wait_for(lambda: len(bands)<1100)
                        bands.append(tuple(segment)); peaks[1]=max(peaks[1],len(bands)); cv.notify_all()
                    actual.extend(segment)
                    observed.append((c,length,head,tail))
                    row+=length
                # Consumer stalls represent canonical W=1100 admission waits;
                # independent downstream ACKs eventually release the publisher.
                if (seed+i+record)%5==0: time.sleep(.00001)
                with cv:
                    used[i]-=available-record; cv.notify_all()
                record=available
                if ended:
                    assert row==cuts[i+1]
                    break
        assert row==n
        with cv: terminal[0]=True; cv.notify_all()
    def guard(fn,*args):
        try: fn(*args)
        except BaseException as e: errors.append(repr(e))
    sink=threading.Thread(target=guard,args=(downstream,));sink.start()
    consumer=threading.Thread(target=guard,args=(reducer,));consumer.start()
    producers=[threading.Thread(target=guard,args=(producer,i)) for i in range(p)]
    rng.shuffle(producers)
    for t in producers:t.start()
    for t in producers:t.join(5);assert not t.is_alive()
    consumer.join(5);assert not consumer.is_alive()
    assert not errors,errors
    sink.join(5);assert not sink.is_alive()
    assert [x for band in acked for x in band]==positions
    assert all(x==0 for x in used)
    assert actual==positions
    def coalesce(runs):
        out=[]
        for c,length,head,tail in runs:
            if out and out[-1][0]==c: out[-1][1]+=length;out[-1][3]=tail
            else:out.append([c,length,head,tail])
        return out
    assert coalesce(observed)==coalesce((chars[i],1,positions[i],positions[i]) for i in range(n))
    if n==2400: assert peaks[1]==1100
    return n,sum(blocked),peaks,tuple(completion),p
trials=[trial(seed) for seed in range(400)]
rows=sum(t[0] for t in trials)
print(json.dumps(dict(result='PASS', deadlocks=0, lost_rows=0, duplicate_rows=0, blocks=sum(t[1] for t in trials), peak_bands=max(t[2][1] for t in trials), P8_completion_orders=len({t[3] for t in trials if t[4]==8}), schedules=400, spool_budget='global_with_active_batch_reserve', boundary='inclusive', publisher_stalls=True, W=1100, rows=rows, P=[1,4,8,16], exact_rows=True, exact_runs=True, singleton_targets=True, repeated_cuts=True, publication='committed_prefix_only', terminal='all_ranges_only', persistent_recovery=False),sort_keys=True))
