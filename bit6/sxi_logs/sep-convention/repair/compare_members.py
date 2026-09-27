"""Stream SXI members for the yeast re-adjudication; never read corpus text."""
import hashlib,json,pathlib,struct,sys

def inspect(path):
    with open(path,'rb') as f:
        header=f.read(64)
        n,k,r=struct.unpack_from('<3Q',header,8)
        count=struct.unpack_from('<I',header,32)[0]
        table=f.read(count*40)
        members={}
        for i in range(count):
            member,codec,offset,size,items,crc,_=struct.unpack_from('<IIQQQII',table,40*i)
            f.seek(offset);left=size;digest=hashlib.sha256()
            while left:
                data=f.read(min(left,1<<20))
                if not data:raise RuntimeError('truncated member')
                digest.update(data);left-=len(data)
            members[str(member)]=dict(codec=codec,bytes=size,count=items,crc32=crc,sha256=digest.hexdigest())
    return dict(path=str(path),n=n,k=k,r=r,members=members)

old,new=map(pathlib.Path,sys.argv[1:3])
previous,current=inspect(old),inspect(new)
changed=[key for key in sorted(set(previous['members'])|set(current['members']))
         if previous['members'].get(key)!=current['members'].get(key)]
result=dict(previous=previous,current=current,changed_members=changed,
            chi_changed=previous['members']['5']!=current['members']['5'],
            header_dimensions_equal=all(previous[k]==current[k] for k in ('n','k','r')))
pathlib.Path(sys.argv[3]).write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(dict(changed_members=changed,chi_changed=result['chi_changed'],chi=current['members']['5']['count'],header_dimensions_equal=result['header_dimensions_equal'])))
