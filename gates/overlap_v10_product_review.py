"""Read-only recovery after proc_7aaf's *reporting* failure. No row replay or constructor.
Checks product bytes, raw/map/remap, witnesses, all required worker exits and postchecks.
Writes JSON to stdout; neither failure exit nor either private STOP is cleared.
"""
import json
from pathlib import Path
import re

from overlap_v10_product_compare import digest, inventory, INPUT, N, ROWS
from overlap_v10_product_input import model

A=Path('/tmp/rolling-v10-product-ljelES')
B=Path('/tmp/rolling-v10-product-4kNx5u')
R=Path(__file__).resolve().parent/'receipts/rolling-v10-product'

def main():
    assert __debug__
    for p,sha in ((A/'reference/status.json','196a3e3d43ea7083d0df034f5c0bf72f103c10703e9125bbf14ac92abd0ab09d'),(B/'candidate/status.json','736d0e840b75faf1c0670607f0c874ee0a6dffbd66792395d86e6696033015d5'),(B/'reference/verified.json','c37655019bd9ad464d0eacafa1e314e1430f7d364fb48bbbaca8932b543544a6'),(B/'arm-candidate.log','dc72ae662f39afd05203832e4dee8b8833ab95b4e8c56ba6eae5ca2205952bc7')): assert digest(p)==sha,p
    assert (A/'exits.log').read_text()=='compile=0\npins=0\narm-reference=1\nlauncher=1\n' and (B/'exits.log').read_text()=='compile=0\npins=0\narm-check-joined-reference=0\narm-candidate=1\nlauncher=1\n'
    assert all((p/'stop.requested').is_file() for p in (A,B)) and not (A/'candidate').exists()
    assert 'PermissionError:' in (A/'arm-reference.log').read_text() and (B/'arm-candidate.log').read_text().rstrip().endswith("/io.stat'")
    ref=A/'reference'; check=B/'reference'; cand=B/'candidate'; st=lambda p:json.loads((p/'status.json').read_text()); rs,cs=st(ref),st(cand)
    assert rs['stop'] and cs['stop'] and not st(check)['stop'] and not cs['errors']
    def zeros(s,names):assert s['exits']==dict.fromkeys(names.split(),0)
    zeros(rs,'rows tap relay sink finish witness join');zeros(st(check),'docs ctx search0 search1');zeros(cs,'front rank emit tap witness sink finish join docs ctx search0 search1')
    for p in (ref,check,cand):
        assert [digest(p/f) for f in ('raw','checker','map/seps.bin','remap')]==[INPUT[k] for k in ('raw','raw','map','remap')];assert not (p/'raw').samefile(p/'checker')
    expect='921f4e473e19fefcf8a7916d431c28694941323b6262ff109ae3c7c7820f19ab'; files=[ref/'work.partial.sxi2',check/'index.sxi2',cand/'index.sxi2']
    assert all(p.stat().st_size==378392 and digest(p)==expect for p in files) and not files[0].samefile(files[-1]);assert files[0].read_bytes()==files[1].read_bytes()==files[-1].read_bytes()
    members=inventory(files[-1]);assert members==inventory(files[0])==json.loads((check/'verified.json').read_text())['members'] and members[3]['sha256']==INPUT['remap']
    for p in (ref,cand):
        assert f'ORACLE rows={ROWS} independent_final_rank={ROWS} all_adjacent_sa_lcp_bwt_verified=1' in (p/'witness.log').read_text() and 'PRODUCT_FENCE runs=101800 escapes=0' in (p/'witness.log').read_text()
        assert 'SXI2_PASS bytes=323360 lite=1 members=8' in (p/'finish.log').read_text() and f'SS_DONE_R overlap_stream rows={ROWS}' in (p/'sink.log').read_text()
    for p in (check,cand):
        assert 'DOCSTARTS_PARITY result=PASS' in (p/'docs.log').read_text() and 'exact=2 mismatches=0 below_min_support=0' in (p/'ctx.log').read_text()
        for cross in (0,1):
            hits=[json.loads(l) for l in (p/f'search{cross}.log').read_text().splitlines() if l.startswith('{')]
            assert [(q['pos'],q['cross']) for q in hits]==([(4628641,1)] if cross else [])
    assert not (cand/'tap').exists() and not list(cand.rglob('*.sa')) and f'CONTEXT_TRIE_ROW_PIPE rows={ROWS} position_file_bytes=0 m20_bytes=54880' in (cand/'tap.log').read_text()
    epochs=[int(x) for x in re.findall(r'^EPOCH_PART .*? bytes=(\d+) ',(cand/'emit.log').read_text(),re.M)];assert epochs==[64896,63720,64832,32048]
    assert cs['sampled_logical_scratch']<=model()['candidate']<=2*N and not re.search(r'^(?:oom|oom_kill) [1-9]',cs['memory_events'],re.M)
    journal=[json.loads(l) for l in (R/'candidate/service-clock.jsonl').read_text().splitlines()];assert 'candidate '+str(B) in journal[0]['MESSAGE'] and 'status=1/FAILURE' in journal[1]['MESSAGE'] and journal[0]['_BOOT_ID']==journal[1]['_BOOT_ID']
    service_us=int(journal[1]['__MONOTONIC_TIMESTAMP'])-int(journal[0]['__MONOTONIC_TIMESTAMP']);assert service_us>0
    print(json.dumps(dict(scope='Two full generated v10 indexes compared after preserved supervisor failures. Not a clean timing arm or launch authority.',inputs=INPUT,product_bytes=378392,sha256=expect,all_bytes_identical=True,members=members,verified_rows=ROWS,all_query_checks_pass=True,epochs=epochs,position_file_bytes=0,candidate_exits=cs['exits'],reference_exits=rs['exits'],reference_postcheck_exits=st(check)['exits'],original_launcher_exits=[1,1],postpublication_stop=True,service_elapsed_us=service_us,service_clock_scope='Generated input setup, cold preparation, full build, row and query checks, receipt failure and teardown to main exit',raw_to_join_s=None,prepared_to_join_s=None,io_stat=None,sampled_logical_scratch=cs['sampled_logical_scratch'],scratch_plan=model(),shared_peak=cs['shared_peak'],shared_limit=384<<20,swap_max=0,cpus=2,memory_events=cs['memory_events']),sort_keys=True,indent=2))

if __name__=='__main__':main()
