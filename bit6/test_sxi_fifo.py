#!/usr/bin/env python3
"""FIFO single-writer and cancellation tests (only this test's child groups)."""
import json, os, pathlib, signal, subprocess, sys, tempfile, time
from sxi_pipeline import stream_parse
with tempfile.TemporaryDirectory(prefix='sxi-fifo-') as tmp:
    root=pathlib.Path(tmp);fifo=root/'input.fifo';rows=[]
    producer=root/'producer.py';consumer=root/'consumer.py';out=root/'received'
    producer.write_text('import os,sys\nfrom pathlib import Path\nwith Path(sys.argv[1]).open("a") as f:f.write(str(os.getpid())+"\\n")\nsys.stdout.buffer.write(b"AGCT"*250000)\n')
    consumer.write_text('from pathlib import Path\nimport sys\nwith open(sys.argv[1],"rb") as f:Path(sys.argv[2]).write_bytes(f.read())\n')
    count=root/'writers'
    stream_parse(fifo,[sys.executable,producer,count],[sys.executable,consumer,fifo,out],os.environ,root,'good',rows.append)
    assert out.read_bytes()==b'AGCT'*250000 and len(count.read_text().splitlines())==1
    assert not fifo.exists() and all(row.get('returncode',0)==0 for row in rows)
    # An early successful reader exit still breaks a large producer loudly.
    try:
        stream_parse(fifo,[sys.executable,producer,count],
                     [sys.executable,'-c',"import sys;open(sys.argv[1],'rb').read(1)",fifo],
                     os.environ,root,'broken',rows.append)
        raise AssertionError('broken stream succeeded')
    except RuntimeError as error:
        assert 'stream failed' in str(error)
    assert not fifo.exists()
    # Journal I/O failure during startup cannot strand either FIFO endpoint.
    calls=[]
    def broken_journal(row):
        calls.append(row)
        if len(calls)>1:raise OSError('injected journal failure')
    try:
        stream_parse(fifo,[sys.executable,producer,count],
                     [sys.executable,consumer,fifo,out],os.environ,root,'journal',broken_journal)
        raise AssertionError('journal failure ignored')
    except OSError as error:
        assert 'injected journal failure' in str(error)
    assert not fifo.exists()
    # Start a slow stream in a separate driver, then cancel that driver. Both
    # its producer and consumer must be gone, and its FIFO must be unlinked.
    driver=root/'driver.py';pidfile=root/'pids'
    driver.write_text('''import os,sys
sys.path.insert(0,'''+repr(str(pathlib.Path(__file__).resolve().parent))+''')
from sxi_pipeline import stream_parse
from pathlib import Path
p=Path(sys.argv[1]);stream_parse(p/'cancel.fifo',[sys.executable,'-c',"import os,sys,time;open(sys.argv[1],'a').write(str(os.getpid())+'\\\\n');sys.stdout.buffer.write(b'A'*1000000);sys.stdout.flush();time.sleep(300)",str(p/'pids')],[sys.executable,'-c',"import os,sys;open(sys.argv[2],'a').write(str(os.getpid())+'\\\\n');open(sys.argv[1],'rb').read()",str(p/'cancel.fifo'),str(p/'pids')],os.environ,p,'cancel',lambda x:None)
''')
    with (root/'cancel.log').open('wb') as log:
        child=subprocess.Popen([sys.executable,driver,root],stdout=log,stderr=log)
        deadline=time.monotonic()+10
        while not pidfile.exists() or len(pidfile.read_text().splitlines())!=2:
            assert child.poll() is None,(root/'cancel.log').read_text()
            assert time.monotonic()<deadline
            time.sleep(.05)
        children=list(map(int,pidfile.read_text().splitlines()))
        child.send_signal(signal.SIGTERM);assert child.wait(timeout=15)!=0
    assert not (root/'cancel.fifo').exists()
    for pid in children:
        try:os.kill(pid,0)
        except ProcessLookupError:continue
        # A reaped wrapper can briefly leave an orphan zombie until init reaps it.
        status=pathlib.Path(f'/proc/{pid}/status').read_text()
        assert 'State:\tZ' in status,status
print('PASS exactly one writer, byte-exact FIFO, broken pipe, journal failure, SIGTERM cleanup of FIFO and children')
