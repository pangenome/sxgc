#!/usr/bin/env python3
"""Replay the retained yeast HTTP oracle under a 20 GB address-space ceiling."""
import json,pathlib,resource,socket,subprocess,time,urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[1];LOG=ROOT/'bit6/sxi_logs/query-product';X=ROOT/'xsa/target/release/xsa'
reads=pathlib.Path('/tmp/sxi-query-yeast/reads.fa').read_text().splitlines();rows=[{'name':reads[i][1:],'read':reads[i+1]} for i in range(0,len(reads),2)]
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
def cap():resource.setrlimit(resource.RLIMIT_AS,(20_000_000_000,20_000_000_000))
with (LOG/'http-final-server.log').open('wb') as log:
    server=subprocess.Popen([X,'serve','--sxi','/tmp/sxi-query-yeast235.sxi','-j','3','--bind',f'127.0.0.1:{port}'],stderr=log,preexec_fn=cap)
    try:
        base=f'http://127.0.0.1:{port}'
        for _ in range(1200):
            try:stats=json.load(urllib.request.urlopen(base+'/stats',timeout=1));break
            except OSError:time.sleep(.1)
        else:raise AssertionError('server failed to start')
        assert stats['k']==9901 and stats['mode']=='dna'
        for endpoint in ['query','ms','batch']:
            value={'reads':rows,'min_len':20} if endpoint=='batch' else rows[0]
            p=subprocess.run(['curl','--fail','--silent','-H','Content-Type: application/json','--data-binary',json.dumps(value),base+'/'+endpoint],capture_output=True)
            assert p.returncode==0 and p.stdout==(LOG/('yeast-http-'+endpoint+'.jsonl')).read_bytes(),endpoint
            print('PASS final HTTP '+endpoint,flush=True)
    finally:server.terminate();server.wait()
peak=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
assert peak*1024<20_000_000_000
print(json.dumps(dict(status='PASS',server_peak_rss_kib=peak,address_space_limit_bytes=20_000_000_000)))
