#!/usr/bin/env python3
"""Read-only, bounded-memory byte comparison of published SXI core members."""
import hashlib,json,pathlib,struct
logs=pathlib.Path(__file__).resolve().parent
native=pathlib.Path('/tmp/pfp-agc-gates/yeast235-native.sxi')
control=pathlib.Path('/tmp/sxi-stream/yeast/yeast235-control.sxi')
def descriptors(f):
    h=f.read(64);count=struct.unpack_from('<I',h,32)[0]
    result={}
    for _ in range(count):
        ident,codec,offset,length,n,crc,res=struct.unpack('<IIQQQII',f.read(40))
        result[ident]=(codec,offset,length,n)
    return result
rows=[]
with native.open('rb') as a,control.open('rb') as b:
    ad,bd=descriptors(a),descriptors(b)
    for ident in range(1,6):
        ac,ao,al,an=ad[ident];bc,bo,bl,bn=bd[ident]
        assert (ac,al,an)==(bc,bl,bn),(ident,ad[ident],bd[ident])
        a.seek(ao);b.seek(bo);remaining=al;ha=hashlib.sha256();hb=hashlib.sha256()
        while remaining:
            x=a.read(min(1<<20,remaining));y=b.read(len(x))
            assert x and x==y,(ident,al-remaining)
            ha.update(x);hb.update(y);remaining-=len(x)
        rows.append(dict(member=ident,bytes=al,count=an,byte_equal=True,sha256=ha.hexdigest(),control_sha256=hb.hexdigest()))
report=dict(native=str(native),control=str(control),native_bytes=native.stat().st_size,control_bytes=control.stat().st_size,all_five_equal=True,members=rows)
(logs/'yeast-published-members.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
