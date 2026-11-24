"""Replay only the small sync/async receipts. Never open products, private inputs or native tools."""
import json
import math
from pathlib import Path
import re
import sys
from overlap_async_pair import BANK, CUTOFF, N, NATIVE, REF, S, STEPS, binaries, decision, load, need, score, sha

ARM_FILES=set('source.json binaries.sha prep.json certify.log cold.log exits.log build.json identity.json oracle.json oracle.log pack.log resident.log scratch-plan.json front-end.json sink.log finish.log'.split())

def single(log,tag):
    matches=[l for l in log.splitlines() if l.startswith(tag+' ')];need(len(matches)==1,'missing/duplicate '+tag);return dict(re.findall(r'(\w+)=(\S+)',matches[0]))

def review(d):
    s=load(d,'source.json');adm=load(d,'admission.json');orig=Path(s['original_directory']);names=set('manifest.sha tools-and-fixtures.log admission.json comparison.json'.split())|{a+'/'+f for a in STEPS for f in ARM_FILES}
    need(__debug__ and s['artifacts'].keys()==names and all((d/f).is_file() and not (d/f).is_symlink() and (d/f).stat().st_size<=131072 and sha(d/f)==h for f,h in s['artifacts'].items()),'archive hashes')
    need(s['exit_code']==0 and s['revision']==adm['rev']=='75869a3b4010f6b086f9fb5e76d3a9ce68d5bc8e' and orig.parent==S and re.fullmatch('async-pair-1g-[A-Za-z0-9]{6}',orig.name),'launch pin')
    one,ten=decision('1g'),decision('10g');need(adm['decisions']=={'1g':one['profile'],'10g':ten['profile']} and adm['expected_profile']==one['profile'] and adm['baseline_raw_s']==one['raw_s'] and adm['required_s']==math.ceil(2*(1.7*one['raw_s']+120)+90) and adm['deadline']<=CUTOFF,'admission witness')
    tools={n:h for h,n in (l.split() for l in (d/'manifest.sha').read_text().splitlines())};need(tools.keys()==set(NATIVE),'native tool set')
    v=score(d);need(v==load(d,'comparison.json'),'pair comparison mismatch'); clocks={};both=[]
    for a,side in [('sync',v['sync']),('async',v['async_audit'])]:
        arm=d/a; src=load(arm,'source.json');need(src['source']==str(orig/a) and src['complete_arm'] is True and src['identity'] is True and src['selected_logs'].keys()=={'sink.log','finish.log'},'arm bank chain')
        for f,x in src['selected_logs'].items():need(x['selected_lines']==len((arm/f).read_bytes().splitlines())<=x['original_lines'] and 0<(arm/f).stat().st_size<=x['original_bytes'] and re.fullmatch('[a-f0-9]{64}',x['sha256']),'invalid selected log')
        need(all(binaries(arm)[k]==h for k,h in tools.items()) and (arm/'binaries.sha').read_text().splitlines()[0]==s['revision'],'arm tools')
        log=(arm/'resident.log').read_text();plan=single(log,'RESIDENT_RADIX_PLAN');join=single(log,'RESIDENT_RADIX_JOIN');stream=single(log,'RESIDENT_STREAM')
        core=tuple(int(x) for x in [plan['lanes'],plan['ranges'],join['blocks'],join['max_range_rows'],stream['rows'],stream['descriptors'],stream['groups'],stream['max_group']]);both.append(core)
        need(core[0]==4 and core[1]==int(join['ranges'])<=core[2] and core[4]==N+1 and join['serial_row_comparisons']=='0' and int(plan['row_queue_reserved'])==side['queue_reserved'],'ordered row geometry')
        clock=dict(plan_s=float(plan['plan_s']),ordered_receive_s=float(join['order_wait_s']),sink_write_s=float(stream['sink_write_s']),publisher_audit_write_s=float(stream['audit_write_s']))
        if a=='async':
            tee=side['tee']; clock.update(enqueue_s=tee['enqueue_s'],drain_s=tee['drain_s'])
            need(0<=tee['worker_write_s']<=tee['worker_wall_s']<=side['wall']['emit'],'async worker clocks')
        clock['other_publisher_elapsed_s']=side['wall']['emit']-sum(clock.values());need(min(clock.values())>=0,'publisher clock partition');clocks[a]=clock
    need(both[0]==both[1],'different range/stream geometry');lanes,ranges,blocks,maxrows,rows,descriptors,groups,maxgroup=both[0]
    return dict(scope='verified 1 GB matched audit pair; publisher partition excludes overlapping audit worker and verifier. No new default or larger-rung qualification.',
        score_sha256=sha(d/'comparison.json'),verified_full_products=v['verified_full_products'],matched_hypothesis=v['matched_product_speed_io_cpu_pass'],container_sha256=one['reference_sha256'],
        rows=rows,lanes=lanes,ranges=ranges,blocks=blocks,max_range_rows=maxrows,descriptors=descriptors,finite_groups=groups,max_group=maxgroup,mean_data_packet_bytes=12*N/blocks,
        publisher=clocks,async_worker_s={k:v['async_audit']['tee'][k] for k in ('worker_write_s','worker_wall_s')},time_ratio=v['time_ratio'],build_cpu_ratio=v['build_cpu_ratio'],
        post_front_io_ratio=v['post_front_io_ratio'],async_over_original_default_wall_for_context_not_pair={k:v['async_audit']['wall'][k]/one[k+'_s'] for k in ('raw','prepared')})

if __name__=='__main__':print(json.dumps(review(Path(sys.argv[1]) if len(sys.argv)>1 else BANK/'async-audit-1g'),indent=2,sort_keys=True))
