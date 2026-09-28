import pathlib,time,json,re,struct,subprocess,sys
logs=pathlib.Path(__file__).resolve().parent
work=pathlib.Path('/tmp/k10-endpoints-debug/retained')
heartbeat=0
while pathlib.Path('/proc/3538947/status').exists():
    now=time.time()
    if now-heartbeat>=55:
        heartbeat=now
        print(json.dumps(dict(update='retained-debug-only',output_bytes={name:(work/name).stat().st_size if (work/name).exists() else 0 for name in ('fresh.ri4','fresh.head_sa')})),flush=True)
    time.sleep(5)
for _ in range(10):
    timing=(logs/'retained-endpoints.time').read_text()
    if 'Exit status:' in timing:break
    time.sleep(1)
assert 'Exit status: 0' in timing,timing
text=(logs/'retained-endpoints.log').read_text()
assert re.search(r'CYCLIC_SEAM_REPAIRED classes=7 rows=1161 discovery_steps=8 ',text)
assert 'ENDPOINT_PASS raw_n=30151407555 raw_r=1859825862 n=30151407545 k=1 r=1859825862 ' in text
with (work/'fresh.ri4').open('rb') as f:
    magic,n,k,r=struct.unpack('<4Q',f.read(32))
    assert magic==0x0000000452585349 and (n,k,r)==(30151407545,1,1859825862)
    f.seek(2080+5*r);bits,width=struct.unpack('<QB',f.read(9))
assert width==35 and bits==r*width
assert (work/'fresh.head_sa').stat().st_size==8*r
assert (work/'fresh.ri4').stat().st_size==2089+5*r+8*((bits+63)//64)
gate=json.loads((logs/'gate-summary.json').read_text())
assert gate['separator_checks']==47 and gate['cyclic_endpoint_cases']==2214
assert gate['yeast_endpoint_bytes']==gate['yeast235_endpoint_bytes']=='identical'
gate.update(k10_debug='PASS (debug only; no retained downstream stages)',k10_debug_n=n,k10_debug_r=r,k10_debug_sizes={name:(work/name).stat().st_size for name in ('fresh.ri4','fresh.head_sa')},from_zero='starting')
(logs/'gate-summary.json').write_text(json.dumps(gate,indent=2)+'\n')
print(json.dumps(dict(update='PHASE_1_PASS',**gate)),flush=True)
raise SystemExit(subprocess.run([sys.executable,str(logs/'run_from_zero.py')]).returncode)
