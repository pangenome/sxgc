"""Check actual remote publication immediately before admitting a larger rung.
No push, merge or source modification. This check is not a performance gate.
"""
import subprocess as sp
from pathlib import Path

ORIGINS = {'git@github.com:pangenome/sxgc.git','https://github.com/pangenome/sxgc.git',
           'ssh://git@github.com/pangenome/sxgc.git','https://github.com/pangenome/sxgc'}

def published_main(root, run=sp.check_output):
    def git(*a):return run(['git','-C',str(root),*a],text=True,timeout=30).strip()
    if Path(git('rev-parse','--show-toplevel')).resolve()!=Path(root).resolve():raise ValueError('not repository root')
    if git('symbolic-ref','--short','HEAD')!='main' or git('status','--porcelain'):
        raise ValueError('scale requires clean main, including untracked files')
    if git('remote','get-url','origin') not in ORIGINS:raise ValueError('not the project origin')
    head=git('rev-parse','HEAD');remote=git('ls-remote','--exit-code','origin','refs/heads/main').split()
    if git('rev-parse','refs/remotes/origin/main')!=head or remote!=[head,'refs/heads/main']:
        raise ValueError('HEAD is not the published origin/main')
    return head

if __name__=='__main__':print('PUBLISHED_MAIN',published_main(Path(__file__).resolve().parents[1]))
