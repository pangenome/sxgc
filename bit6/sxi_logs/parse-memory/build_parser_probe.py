import pathlib,subprocess,shlex
here=pathlib.Path(__file__).resolve().parent; work=here/'work'
base=pathlib.Path('/home/erikg/pfp')
s=(base/'src/pfp_algo.cpp').read_text()
pos=s.index('vcfbwt::pfp::ParserText::operator()()')
s=s[:pos]+s[pos:].replace('    char c;','    unsigned char c;',1)
(work/'pfp_algo.cpp').write_text(s)
flags={k:v.strip() for k,v in (line.split('=',1) for line in (base/'build/CMakeFiles/pfp.dir/flags.make').read_text().splitlines() if ' = ' in line)}
cmd=['g++',*shlex.split(flags['CXX_INCLUDES ']),*shlex.split(flags['CXX_FLAGS ']),'-c',str(work/'pfp_algo.cpp'),'-o',str(work/'pfp_algo.cpp.o')]
subprocess.run(cmd,check=True)
subprocess.run(['cp',str(base/'build/libpfp.a'),str(work/'libpfp.a')],check=True)
subprocess.run(['ar','r',str(work/'libpfp.a'),str(work/'pfp_algo.cpp.o')],check=True)
cmd=shlex.split((base/'build/CMakeFiles/pfp++.dir/link.txt').read_text())
cmd=[str(work/'pfp-probe') if x=='pfp++' else str(work/'libpfp.a') if x=='libpfp.a' else x for x in cmd]
subprocess.run(cmd,cwd=base/'build',check=True)
