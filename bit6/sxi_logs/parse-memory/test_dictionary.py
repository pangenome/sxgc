import pathlib, shlex, subprocess
here=pathlib.Path(__file__).resolve().parent
command=next(shlex.split(l) for l in (here/'build.log').read_text().splitlines() if l.startswith('g++ -O3'))
for mode in ('baseline','memory'):
    cmd=[str(here/'dictionary_memory_test.cpp') if x.endswith('/rpfbwt/rpfbwt.cpp') else x for x in command if not x.endswith('/generated/version.cpp')]
    cmd=[x for x in cmd if mode=='memory' or x!='-DSXI_MEMORY_DICTIONARY']
    cmd[cmd.index('-o')+1]=str(here/'work'/('dictionary-test-'+mode))
    subprocess.run(cmd,check=True)
    with (here/'work'/('dictionary-'+mode+'.values')).open('wb') as out:
        subprocess.run([cmd[-1]],stdout=out,check=True)
subprocess.run(['cmp',str(here/'work/dictionary-baseline.values'),str(here/'work/dictionary-memory.values')],check=True)
print('PASS: 24 randomized byte/integer dictionaries, exact SA/ISA/DA/LCP/colex values')
