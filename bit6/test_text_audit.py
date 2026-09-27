#!/usr/bin/env python3
"""Audit must reject bad materialized text and failed gates must not publish."""
import json,pathlib,subprocess,tempfile,os
ROOT=pathlib.Path(__file__).resolve().parents[1];LOG=ROOT/'bit6/sxi_logs/query-product'
record=json.loads((LOG/'fresh-build.log').read_text().splitlines()[-1]);work=pathlib.Path(record['work'])
base=['/tmp/sxi-query-audit',str(work/'collection.txt'),str(work/'fresh.ri4'),str(work/'fresh.agg'),str(work/'fresh.sA'),'32']
p=subprocess.run(base,capture_output=True,text=True);assert p.returncode==0 and 'verified=32' in p.stdout,p.stderr
with tempfile.TemporaryDirectory(prefix='sxi-audit-negative-') as tmp:
    tmp=pathlib.Path(tmp);bad=tmp/'bad.txt';bad.write_bytes(b'A'*(work/'collection.txt').stat().st_size)
    args=base.copy();args[1]=str(bad);p=subprocess.run(args,capture_output=True,text=True);assert p.returncode!=0 and 'TEXT_SAMPLE_FAIL' in p.stderr
    tools=tmp/'tools';tools.mkdir()
    for name in ['rpfbwt_endpoints','slim_dump','sxi_write']:(tools/name).symlink_to('/tmp/sxi-query-tools/'+name)
    (tools/'sxi_text_audit').symlink_to('/bin/false')
    output=tmp/'must-not-exist.sxi'
    p=subprocess.run([ROOT/'xsa/target/release/xsa','build','--fasta','/tmp/sxi-query-fresh/refs.fa','-o',output,'--scratch',tmp,'--verify-text-sample','32','--log-dir',LOG],env=dict(os.environ,XSA_TOOLS=str(tools)),capture_output=True,text=True)
    assert p.returncode!=0 and 'verify-text-sample failed' in p.stderr and not output.exists(),p.stderr
print('PASS direct-text witnesses, corrupted text rejected, failed sample gate prevents publication')
