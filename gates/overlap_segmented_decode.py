"""Untimed (separately metered) cold private-column reconstruction."""
from pathlib import Path
import os
import subprocess as sp
import sys
import time

from overlap_segmented_gate import CG, NAMES, RUNGS, cg_info, cold, finished, load, need, memory_available, private_dir, proc_info, save, stat_io


def decode(rung,d,deadline,receipt):
    n=RUNGS[rung][0]; grant={'1g':3<<30,'10g':16<<30}[rung]
    front=load(d,'segmented.json');need(front['status']=='FRONT_COMPARED' and front['n']==n,'front comparison required')
    cg=cg_info();need((cg['cpu.max'],cg['memory.max'],cg['memory.swap.max'])==('100000 100000',str(grant),'0') and
                      (CG/'cgroup.procs').read_text().split()==[str(os.getpid())],'wrong independent verifier cgroup')
    stop=d/'stop.requested'; cols=d/'columns';k=front['census']['k'];metadata=8*k+(k+7)//8
    available=memory_available();tape_bytes=(cols/NAMES[0]).stat().st_size
    need(available>=grant and metadata+tape_bytes+(128<<20)<=grant,'insufficient verifier RAM/headroom')
    receipt.update(host_available_before=available,grant_minus_metadata_and_tape=grant-metadata-tape_bytes,cgroup=str(CG),cgroup_before=cg,metadata_limit=metadata,raw_sha256=front['raw_sha256'])
    for name in NAMES:
        need(time.time()<deadline and not stop.exists(),'stopped before decoder cold check')
        col=cols/name;need(not col.is_symlink() and col.stat().st_nlink==1,'not a private column');cold(col)
    need(not stop.exists() and time.time()<deadline,'stopped at cold barrier')
    receipt['cold_private_columns_zero_resident']=list(NAMES)
    start=time.monotonic();before=stat_io((CG/'io.stat').read_text());refusal=None
    with (d/'decode.log').open('xb') as log, sp.Popen([d/'verify',cols,str(n),front['raw_sha256'],str(metadata)],stdout=log,stderr=log) as p:
        try:
            with open(f'/proc/{p.pid}/io') as io:
                while True:
                    done=finished(p);info=proc_info(p.pid,io); now=stat_io((CG/'io.stat').read_text())
                    delta={key:now[key]-before[key] for key in now}
                    if stop.exists() or time.time()>=deadline or max(info['read_bytes'],delta['rbytes'])>8*n or max(info['write_bytes'],delta['wbytes'])>(16<<20):
                        refusal='decode stop/time/IO guard';stop.touch(exist_ok=True)
                        if not done:p.terminate()
                    if done:break
                    time.sleep(.25)
            rc=p.wait()
        except BaseException:
            p.kill();raise
    after=cg_info();receipt.update(rc=rc,process=info,wall_s=time.monotonic()-start,cgroup_after=after,cgroup_io_delta=delta)
    line=(d/'decode.log').read_text().strip(); v=front['census']
    expected=(f"VERIFIED n={n} k={k} m={v['m']} descriptors={v['descriptors']} D={v['D']} "
              f"max_phrase={v['max_phrase']} metadata_bytes={metadata} sha256={front['raw_sha256']}")
    need(not refusal and rc==0 and {'oom 0','oom_kill 0'}.issubset(after['memory.events'].splitlines()) and
         line==expected,'independent decoder refused')
    receipt['decode_verified']=True
    need(front['front_under_3x'], 'exact columns and decode, missed front-only 3x ceiling')
    need(time.time()<deadline and not stop.exists(),'stopped at verdict')


if __name__=='__main__':
    rung,d,deadline=sys.argv[1:];d=Path(d);private_dir(d,rung);need(not (d/'decode.json').exists(),'refuse receipt reuse')
    receipt=dict(status='REFUSED',promotion=False)
    try:
        decode(rung,d,int(deadline),receipt);receipt['status']='INTERMEDIATE_VERIFIED'
    except Exception as e:
        receipt['error']=str(e);raise
    finally:
        save(d/'decode.json',receipt)
