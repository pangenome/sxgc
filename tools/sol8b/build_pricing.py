#!/usr/bin/env python3
"""Generate an instrumented measurement build without editing production code."""
from pathlib import Path
import subprocess, sys
root=Path(__file__).resolve().parents[2]
scratch=Path(sys.argv[1]);scratch.mkdir(parents=True,exist_ok=True)
source=(root/'bit6/cross_lcp_merge.cpp').read_text()
replacements={
    '        ++bud->comparisons;\n#ifdef SOL7D_TRACE': '        Sol8bGuard<Keys> sol8bGuard(this,p1,p2);\n        ++bud->comparisons;\n#ifdef SOL7D_TRACE',
    '    workLine("plan", 0, difference(bud, preparedWork));':'    workLine("plan", 0, difference(bud, preparedWork));\n    sol8bEnabled.store(true);',
    '        if (sparse) sparse->finishWalk();':'        sol8bEnabled.store(false);\n        if (sparse) sparse->finishWalk();',
}
for before,after in replacements.items():
    assert source.count(before)==1,before
    source=source.replace(before,after)
copy=scratch/'instrumented.cpp';copy.write_text(source)
command=['c++','-O3','-std=c++17','-pthread','-DSOL8B_LCE',
         '-include',str(root/'tools/sol8b/lce_measure.hpp'),
         '-DSOL8B_MERGE_SOURCE="'+str(copy)+'"',str(root/'tools/sol8a/measure.cpp'),
         '-latomic','-o',str(scratch/'measure-lce')]
subprocess.run(command,check=True)
