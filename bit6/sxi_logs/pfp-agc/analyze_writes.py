import collections,gzip,json,pathlib,re
logs=pathlib.Path(__file__).resolve().parent
path=logs/'reader-write.trace';stream=path.open() if path.exists() else gzip.open(str(path)+'.gz','rt')
pending={};writes=collections.Counter();shared=[];truncates=[];unknown=[]
with stream:
 for line in stream:
  pid,body=line.split(' ',1);body=body.lstrip()
  if '<unfinished ...>' in body: pending[pid]=body.split('<unfinished ...>')[0];continue
  if body.startswith('<... '):
   assert pid in pending,line
   body=pending.pop(pid)+body.split(' resumed>',1)[1]
  if re.match(r'(write|writev|pwrite64|pwritev)\(',body):
   m=re.match(r'\w+\(\d+<([^>]+)>.*\)\s+=\s+(-?\d+)',body)
   if not m:unknown.append(body)
   elif int(m[2])>0:writes[m[1]]+=int(m[2])
  if body.startswith('mmap(') and 'MAP_SHARED' in body and '<' in body: shared.append(body)
  if body.startswith(('ftruncate(','truncate(')):truncates.append(body)
assert not unknown,unknown[:3]
assert not shared,shared[:3]
assert not truncates,truncates[:3]
product={p:n for p,n in writes.items() if p.startswith('/tmp/pfp-agc-gates/traced/')}
result=dict(writes_by_path=dict(writes),product_bytes_written=sum(product.values()),largest_product_file_bytes_written=max(product.values()),over_1GB=[p for p,n in product.items() if n>1_000_000_000],shared_file_mappings=shared,truncates=truncates,unknown_write_records=unknown)
assert not result['over_1GB']
assert not any('collection.txt' in p for p in product)
(logs/'reader-writes.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
