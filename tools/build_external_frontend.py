#!/usr/bin/env python3
"""Build isolated parsed-space frontends using already built pinned dependencies.

build_external_frontend.py pfp SOURCE BUILD NEW_OUTPUT
build_external_frontend.py rpfbwt DEPS BUILD NEW_OUTPUT
DEPS is build_all.sh's dependency tree; BUILD is its rpfbwt-build directory.
No source/dependency/retained artifact outside NEW_OUTPUT is modified.
"""
import pathlib
import shlex
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
if len(sys.argv) != 5 or sys.argv[1] not in ('pfp', 'rpfbwt'):
    raise SystemExit(__doc__)
mode = sys.argv[1]
source, built, out = (pathlib.Path(x).resolve() for x in sys.argv[2:])
out.mkdir(parents=True, exist_ok=False)

def run(cmd):
    print(shlex.join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), check=True)

if mode == 'pfp':
    import shutil
    shutil.copytree(source / 'include', out / 'include')
    shutil.copytree(source / 'src', out / 'src')
    shutil.copyfile(source/'pfp++.cpp',out/'pfp++.cpp')
    run(['patch', '-d', out, '-p1', '-i', root/'bit6/patches/pfp_external.patch'])
    flags_file = (built / 'CMakeFiles/pfp.dir/flags.make').read_text()
    flags = []
    for line in flags_file.splitlines():
        if line.startswith(('CXX_DEFINES = ', 'CXX_INCLUDES = ', 'CXX_FLAGS = ')):
            flags += shlex.split(line.split(' = ', 1)[1])
    flags = ['-I'+str(out/'include'), '-I'+str(root/'bit6/external')] + flags
    # Recompile every translation unit whose layout may depend on Dictionary.
    files = list((out/'src').glob('*.cpp')) + [built/'generated/version.cpp', out/'pfp++.cpp']
    files += list((built/'_deps/smhasher-src/src').glob('MurmurHash3.cpp'))
    objects=[]
    for i, path in enumerate(files):
        obj=out/f'unit-{i}.o'; objects.append(obj)
        run(['g++', *flags, '-c', path, '-o', obj])
    link = shlex.split((built/'CMakeFiles/pfp++.dir/link.txt').read_text())
    # Preserve upstream library order and AGC dependencies, replacing its objects.
    start=link.index('libpfp.a')+1
    run(['g++', '-fopenmp', *objects, *link[start:], '-o', out/'pfp-external'])
else:
    # Reuse the isolated 32/64-bit ABI build, then replace only the L1 backend.
    memory = out/'memory'
    run([sys.executable, root/'tools/build_memory_rpfbwt.py', source, built, memory])
    run(['patch','-d',memory/'rpfbwt','-p1','-i',root/'bit6/patches/rpfbwt_external.patch'])
    cpp=memory/'rpfbwt/rpfbwt.cpp'
    includes=[root/'bit6/pfp_ds_vendor',memory/'rpfbwt/include',source/'rpfbwt/include',built/'generated',
        *[source/p for p in ['pfp_ds/include','spdlog/include','cli11/include','mio/single_include','rlestring/include','kseq','gsacak','sdsl/include']],built/'_deps/divsufsort-build/include']
    libs=[built/'_deps'/p for p in ['sdsl-build/lib/libsdsl.a','divsufsort-build/lib/libdivsufsort.a','divsufsort-build/lib/libdivsufsort64.a','rlestring-build/librlestring.a','sdsl-build/lib/libsdsl.a','divsufsort-build/lib/libdivsufsort.a','divsufsort-build/lib/libdivsufsort64.a']]
    run(['g++','-O3','-DNDEBUG','-std=c++17','-fopenmp','-DM64=1','-DSXI_MEMORY_DICTIONARY','-DSXI_EXTERNAL_DICTIONARY',
        *['-I'+str(p) for p in includes],cpp,built/'generated/version.cpp',memory/'gsacak32.o',memory/'gsacak64.o',*libs,'-pthread','-ldl','-o',out/'rpfbwt-external'])
