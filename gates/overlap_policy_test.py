"""Small negative tests: local tracking refs and projected speed aren't promotion."""
from pathlib import Path
import unittest
from overlap_published import published_main
from overlap_scaling_policy import select

class PolicyTest(unittest.TestCase):
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
