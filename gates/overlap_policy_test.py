"""Small negative tests: local tracking refs and projected speed aren't promotion."""
from pathlib import Path
import subprocess as sp
import sys, select as poll
from tempfile import TemporaryDirectory
from overlap_resident_gate import release_unowned_pipe
from overlap_timing_idle import worker_name
import unittest
from overlap_published import published_main
from overlap_scaling_policy import select
from overlap_ten_dict_grid import replay, score as grid_score, GRID, REPO
from overlap_ten_score import get, measured_goals
from overlap_resident_gate import dict_extra,save

class PolicyTest(unittest.TestCase):
    def test_actual_worker_not_shell_code(self):
        for args in [['bash','-c','git show gates/a; diff bin/overlap_resident'],['rg','something','gates/overlap_radix_product.sh'],['python3','-c','print(1)','# /overlap_resident']]:self.assertIsNone(worker_name(args))
        for args in [['bash','-e','--','gates/overlap_ten_product.sh'],['/tmp/tool/overlap_resident','columns'],['/usr/bin/python3','-B','/gates/overlap_resident_gate.py','build']]:self.assertIsNotNone(worker_name(args))
    def test_non_deadline_stop_unblocks_unowned_full_pipe(self):
        with TemporaryDirectory() as tmp:
            flag=Path(tmp)/'stop.requested'
            with sp.Popen([sys.executable,'-c',"import sys;sys.stdout.buffer.write(b'x'*(8<<20));sys.stdout.buffer.flush()"],stdout=sp.PIPE,stderr=sp.DEVNULL) as p:
                self.assertTrue(poll.select([p.stdout],[],[],3)[0]);workers={'resident':p}
                release_unowned_pipe(workers,flag);self.assertFalse(p.stdout.closed)
                flag.touch();release_unowned_pipe({**workers,'sink':object()},flag);self.assertFalse(p.stdout.closed)
                release_unowned_pipe(workers,flag);self.assertTrue(p.stdout.closed);self.assertNotEqual(p.wait(timeout=3),0)
    def test_native_grid_replay(self):
        base=REPO/'gates/receipts';self.assertEqual(measured_goals(base/'packed-serial-10g'),get(base/'packed-serial-10g','goals.json'))
        f=get(base/'packed-front-10g','front-end.json');D=f['census']['D'];m=f['census']['m']
        with TemporaryDirectory() as tmp:
            d=Path(tmp);cg={'memory.max':str(200<<30),'cpu.max':'400000 100000','memory.swap.max':'0','memory.peak':'10','memory.events':'oom 0\noom_kill 0'}
            save(d/'inputs.json',dict(env=GRID,columns=f['columns'],array_grant=140<<30,cgroup=cg));save(d/'exit.json',dict(rc=0,cgroup=cg))
            (d/'cold.log').write_text('private column copies: mincore zero resident pages\n');(d/'fixtures.log').write_text('PASS naive SHA-256 of all LE i64 entries x3 arms; no rows; fixture')
            (d/'binaries.log').write_text('a'*40+'\n'+'b'*64+'  /test/overlap_resident\n')
            lines=[f'RESIDENT_CHECK recurrence_ranks={m} virtual_end=0','RESIDENT_PHASE phase=aligned_and_lists seconds=1']
            for t,s in [(1,9),(4,7),(2,7.3)]:lines += [f'RESIDENT_DICT_SORT kernel=native_wide wide=true selected_threads={t} fs=0 thread_workspace_bytes={dict_extra(D,t)}',f'DICT_GRID threads={t} entries={D} wall_s={s} cpu_s=1 peak_rss=9 read_bytes=0 write_bytes=0 sha256={"f"*64} hash_s=1']
            log='\n'.join(lines);(d/'resident.log').write_text(log);result=grid_score(log,D);save(d/'result.json',result)
            self.assertEqual(replay(d)['selected_threads'],2);save(d/'result.json',{**result,'selected_threads':4})
            with self.assertRaises(AssertionError):replay(d)
            save(d/'result.json',result);(d/'stop.requested').touch()
            with self.assertRaises(AssertionError):replay(d)
    def test_publication(self):
        root=Path('/no-dataset'); h='a'*40
        base={'rev-parse --show-toplevel':str(root),'symbolic-ref --short HEAD':'main','status --porcelain':'',
              'remote get-url origin':'git@github.com:pangenome/sxgc.git','rev-parse HEAD':h,'rev-parse refs/remotes/origin/main':h,
              'ls-remote --exit-code origin refs/heads/main':f'{h}\trefs/heads/main'}
        check=lambda m:published_main(root,lambda argv,**_:m[' '.join(argv[3:])])
        self.assertEqual(check(base),h)
        for key,bad in [('ls-remote --exit-code origin refs/heads/main',f'{"b"*40}\trefs/heads/main'),
                        ('rev-parse refs/remotes/origin/main','b'*40),('status --porcelain','?? unbanked.plan'),
                        ('symbolic-ref --short HEAD','study'),('remote get-url origin','/tmp/local-snapshot')]:
            with self.subTest(key=key),self.assertRaises(ValueError):check({**base,key:bad})
    def test_wall_cpu_queue_lexicographic_order(self):
        row=lambda l,t,c,ok=True:dict(lanes=l,raw_s=t,build_cpu_s=c,eligible=ok)
        a,b,c=row(4,100,250),row(2,102,200),row(1,108,180)
        self.assertEqual(select([a,b,c,row(1,75,50,False)]),b)
        self.assertEqual(select([a,row(2,100.5,255)] )['lanes'],2)
        self.assertIsNone(select([row(1,1,1,False)]))

if __name__=='__main__':unittest.main()
