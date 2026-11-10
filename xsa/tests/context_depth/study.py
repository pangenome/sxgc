#!/usr/bin/env python3
"""Existing CLI only. Positions are exclusive context ends; all counts include self."""
import argparse, json, math, random, struct, subprocess, time, pathlib

class Server:
    def __init__(self, exe, index, command, extra, out):
        self.log = open(out / (command + '.stderr'), 'w')
        self.p = subprocess.Popen([exe, command, index, '--serve', '-j', '1'] + extra,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log)
        self.times = []
    def ask(self, b):
        t = time.perf_counter_ns()
        self.p.stdin.write(struct.pack('<I', len(b)) + b); self.p.stdin.flush()
        h = self.p.stdout.read(4)
        if len(h) != 4: raise RuntimeError('server died; inspect stderr')
        n, = struct.unpack('<I', h)
        r = self.p.stdout.read(n)
        assert len(r) == n
        self.times.append((time.perf_counter_ns()-t)/1000)
        return r
    def close(self):
        self.p.stdin.close(); assert self.p.wait() == 0; self.log.close()

def quant(v):
    v = sorted(v)
    return dict(zip(['p50','p90','p99','max'], [v[math.ceil(len(v)*q)-1] for q in [.5,.9,.99,1]]))

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--exe', required=True); p.add_argument('--index', required=True)
    p.add_argument('--corpus', required=True); p.add_argument('--bytes', type=int, default=100000000000)
    p.add_argument('--samples', type=int, default=1000); p.add_argument('--out', required=True)
    a = p.parse_args(); out = pathlib.Path(a.out); out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20261110)
    positions = sorted(rng.sample(range(1024, a.bytes+1), a.samples))
    (out/'positions.json').write_text(json.dumps(positions))
    br = Server(a.exe,a.index,'branches',[],out)
    me = Server(a.exe,a.index,'mem',['--min-len','1'],out)
    f = open(a.corpus,'rb'); rows=[]; depths=[2**i for i in range(11)]
    raw = open(out/'observations.jsonl','w')
    for i,pos in enumerate(positions):
        cache={}
        def context(d):
            f.seek(pos-d); b=f.read(d); assert len(b)==d; return b
        def query(d):
            if d not in cache:
                r=br.ask(context(d)); ident,l,z,w,eof,other=struct.unpack_from('<6Q',r)
                nb,=struct.unpack_from('<H',r,48)
                counts=[struct.unpack_from('<Q',r,51+9*j)[0] for j in range(nb)]
                assert sum(counts)+eof+other==w and other==0
                assert w>=1, ('source mismatch',pos,d)
                cache[d]={'count':w,'branches':nb,'entropy':-sum((c/w)*math.log2(c/w) for c in counts+([eof] if eof else [])), 'top1':max(counts,default=0)/w}
            return cache[d]
        # MEM confirms source bytes and orientation: Q=T[pos-16:pos], unchanged.
        b=context(16); length,start,count=struct.unpack('<IIQ',me.ask(b))
        assert (length,start,count)==(16,0,query(16)['count'])
        support={}
        for k in [2,10,100,1000]:
            lo=0; hi=1
            while query(hi)['count']>=k+1:
                lo=hi
                if hi==pos: break
                hi=min(pos,hi*2)
            if lo!=pos:
                while hi-lo>1:
                    mid=(hi+lo)//2
                    if query(mid)['count']>=k+1: lo=mid
                    else: hi=mid
            support[str(k)]=lo
        curve={str(d):query(d) for d in depths}
        row={'position':pos,'support':support,'curve':curve}
        raw.write(json.dumps(row)+'\n'); raw.flush(); rows.append(row)
        if i%25==0: print(i, pos, support, flush=True)
    br.close(); me.close(); raw.close()
    result={'samples':len(rows),'support':{str(k):quant([r['support'][str(k)] for r in rows]) for k in [2,10,100,1000]},
            'curve':{str(d):{metric:quant([r['curve'][str(d)][metric] for r in rows]) for metric in ['count','branches','entropy','top1']} for d in depths},
            '1024_elsewhere_ge2':sum(r['curve']['1024']['count']>=3 for r in rows),
            '1024_total_ge2':sum(r['curve']['1024']['count']>=2 for r in rows),
            'latency_us':{'branches':quant(br.times[1:]),'mem16':quant(me.times[1:])},
            'query_counts':{'branches':len(br.times),'mem16':len(me.times)}}
    (out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__': main()
