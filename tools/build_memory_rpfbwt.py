#!/usr/bin/env python3
"""Build an isolated memory experiment from existing pinned, built dependencies.
Usage: build_memory_rpfbwt.py DEPS RPFBWT_BUILD OUTPUT_DIRECTORY
No dependency or retained artifact is modified.
"""
import pathlib, subprocess, sys
if len(sys.argv) != 4:
    raise SystemExit(__doc__)
root=pathlib.Path(__file__).resolve().parents[1]
deps, built, out=map(lambda x:pathlib.Path(x).resolve(),sys.argv[1:])
if 'out_tail_tmp_fstream' not in (deps/'rpfbwt/include/rpfbwt_algorithm.hpp').read_text():
    raise SystemExit('DEPS must be the tail-emitting fork produced by tools/build_all.sh')
out.mkdir(parents=True,exist_ok=True)
def run(cmd):
    print(' '.join(map(str,cmd)),flush=True)
    subprocess.run(list(map(str,cmd)),check=True)
# Isolate ALL defined globals, including gSACA-K's non-static helper functions.
run(['gcc','-O3','-DM64=0','-c',deps/'gsacak/gsacak.c','-o',out/'gsacak32.o'])
symbols=subprocess.check_output(['nm','-g','--defined-only',str(out/'gsacak32.o')],text=True)
(out/'symbols.map').write_text(''.join(f'{line.split()[-1]} sxi32_{line.split()[-1]}\n' for line in symbols.splitlines() if line.strip()))
run(['objcopy','--redefine-syms='+str(out/'symbols.map'),out/'gsacak32.o'])
run(['gcc','-O3','-DM64=1','-c',deps/'gsacak/gsacak.c','-o',out/'gsacak64.o'])
source=out/'rpfbwt';source.mkdir(exist_ok=True)
for name in ['rpfbwt.cpp','include/rpfbwt_algorithm.hpp']:
    dest=source/name;dest.parent.mkdir(exist_ok=True)
    dest.write_bytes((deps/'rpfbwt'/name).read_bytes())
run(['patch','-d',source,'-p1','-i',root/'bit6/patches/rpfbwt_memory.patch'])
includes=[root/'bit6/pfp_ds_vendor',source/'include',deps/'rpfbwt/include',built/'generated',
          *[deps/p for p in ['pfp_ds/include','spdlog/include','cli11/include','mio/single_include','rlestring/include','kseq','gsacak','sdsl/include']],built/'_deps/divsufsort-build/include']
libs=[built/'_deps'/p for p in ['sdsl-build/lib/libsdsl.a','divsufsort-build/lib/libdivsufsort.a','divsufsort-build/lib/libdivsufsort64.a','rlestring-build/librlestring.a','sdsl-build/lib/libsdsl.a','divsufsort-build/lib/libdivsufsort.a','divsufsort-build/lib/libdivsufsort64.a']]
run(['g++','-O3','-DNDEBUG','-std=c++17','-fopenmp','-DM64=1','-DSXI_MEMORY_DICTIONARY',*['-I'+str(p) for p in includes],source/'rpfbwt.cpp',built/'generated/version.cpp',out/'gsacak32.o',out/'gsacak64.o',*libs,'-pthread','-ldl','-o',out/'rpfbwt-memory'])
