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
