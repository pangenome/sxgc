import json,pathlib,re
h=pathlib.Path(__file__).resolve().parent
out={}
pat=re.compile(r'op=(\S+) old=(\S+) new=(\S+) old_bytes=(\d+) bytes=(\d+) peak_kib=(\d+)')
for mode in ['baseline','memory']:
    live={};peak=0;at_peak={};records=[]
    for line in (h/('allocations-'+mode+'.log')).read_text().splitlines():
        if line.startswith('LIVE '):records.append(line)
        m=pat.search(line)
        if not m:continue
        op,old,new,oldbytes,bytes_,rss=m.groups();n=int(bytes_)
        if op in ('free','realloc'):live.pop(old,None)
        if op!='free' and new!='(nil)' and n>=10_000_000:live[new]=n
        total=sum(live.values())
        if total>peak:peak=total;at_peak=dict(live)
    out[mode]=dict(tracked_large_allocation_peak_bytes=peak,allocations_at_peak=sorted(at_peak.values(),reverse=True),persistent_snapshots=records)
(h/'allocation-summary.json').write_text(json.dumps(out,indent=2)+'\n')
