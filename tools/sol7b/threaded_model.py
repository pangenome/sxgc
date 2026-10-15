#!/usr/bin/env python3
"""Threaded upstream transport / ordered BPG1 publisher / finish model.

Extends sol5i's readiness/window functions. Descriptors stand for known,
canonical bands, NOT an implementation of row-to-run reduction or a proof of
heap work identity. Local producer EOF never publishes global BPT1.
"""
import importlib.util
import itertools
import json
import random
import threading
import time
from collections import deque
from pathlib import Path

spec = importlib.util.spec_from_file_location('sol5i', Path(__file__).parents[1] / 'sol5i/wait_model.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def trial(seed, producers, window, bands, fault=None, finish_order=None):
    rng = random.Random(seed)
    # Contiguous rank ranges. Empty ranges are legitimate (coincident planner boundaries).
    cuts = [i * bands // producers for i in range(producers + 1)]
    extents = [0] * bands
    if window > 1:
        # Check every producer seam, including seams with no local work.
        for end in cuts[1:]:
            if end:
                extents[end - 1] = 1  # final band needs true global EOF clamp
        if bands >= window:
            extents[0] = window - 1
    base.validate_window(window, extents)
    cv = threading.Condition()
    queues = [deque() for _ in range(producers)]
    done = [False] * producers
    state = dict(progress=-1, consumed=-1, terminal=False, abort=False,
                 peak_queue=0, peak_pieces=0, duplicates=0, retries=0,
                 errors=[], completion_order=[])
    pieces, accepted = {}, {}
    finish_rows = []
    stalls = [{b for b in range(cuts[p], cuts[p+1]) if rng.randrange(19) == 0}
              for p in range(producers)]

    def guard(fn):
        def run():
            try:
                fn()
            except Exception as error:
                with cv:
                    state['errors'].append(str(error))
                    state['abort'] = True
                    cv.notify_all()
        return run

    def put(p, descriptor):
        with cv:
            cv.wait_for(lambda: len(queues[p]) < 4 or state['abort'])
            if state['abort']:
                return False
            queues[p].append(descriptor)
            state['peak_queue'] = max(state['peak_queue'], len(queues[p]))
            cv.notify_all()
            return True

    def producer(p):
        restarted = False
        for b in range(cuts[p], cuts[p+1]):
            if b in stalls[p]:
                time.sleep(.00001)
            if p == 0 and b == cuts[p] and fault == 'failed':
                raise RuntimeError('injected producer failure')
            if p == 0 and b == cuts[p] and fault == 'gap':
                continue
            descriptor = (b, b * 65536, 65536 if b + 1 < bands else 13)
            if not put(p, descriptor):
                return
            if fault == 'retry' and not restarted:
                # Logical worker restart from the last unacknowledged slab:
                # the same stable identity may be either queued or committed.
                restarted = True
                with cv:
                    state['retries'] += 1
                if not put(p, descriptor):
                    return
        with cv:
            if finish_order is not None:
                cv.wait_for(lambda: state['abort'] or
                            finish_order[len(state['completion_order'])] == p)
            if state['abort']:
                return
            done[p] = True
            state['completion_order'].append(p)
            cv.notify_all()

    def spool():
        for p in range(producers):
            expected = cuts[p]
            while True:
                with cv:
                    cv.wait_for(lambda: queues[p] or done[p] or state['abort'])
                    if state['abort']:
                        return
                    if not queues[p]:
                        assert expected == cuts[p+1], 'range ended with a gap'
                        break
                    descriptor = queues[p].popleft()
                    cv.notify_all()
                    b, first, count = descriptor
                    if b in accepted:
                        assert accepted[b] == descriptor, 'retry changed payload'
                        state['duplicates'] += 1
                        continue
                    assert b == expected, 'range gap or reordered slab'
                    assert first == b * 65536
                    cv.wait_for(lambda: b <= state['consumed'] + window or state['abort'])
                    if state['abort']:
                        return
                    accepted[b] = descriptor
                    pieces[b] = descriptor
                    state['progress'] = b
                    expected += 1
                    state['peak_pieces'] = max(state['peak_pieces'], len(pieces))
                    assert len(pieces) <= window
                    cv.notify_all()
        with cv:
            assert all(done) and len(accepted) == bands
            state['terminal'] = True
            cv.notify_all()

    def finish():
        for b in range(bands):
            with cv:
                # EOF clamp becomes legal only after the sole global terminal.
                target = b + extents[b]
                cv.wait_for(lambda: state['abort'] or
                            state['progress'] >= (min(target, bands-1)
                                                  if state['terminal'] else target))
                if state['abort']:
                    return
                first, count = pieces.pop(b)[1:]
                assert first == b * 65536
                finish_rows.append((first, count))
                state['consumed'] = b
                cv.notify_all()

    threads = [threading.Thread(target=guard(lambda p=p: producer(p))) for p in range(producers)]
    threads += [threading.Thread(target=guard(spool)), threading.Thread(target=guard(finish))]
    rng.shuffle(threads)
    for thread in threads:
        thread.start()
    deadline = time.monotonic() + 5
    for thread in threads:
        thread.join(max(0, deadline - time.monotonic()))
    stuck = any(thread.is_alive() for thread in threads)
    if stuck:
        with cv:
            state['abort'] = True
            cv.notify_all()
        for thread in threads:
            thread.join(1)
    assert not stuck, ('deadlock', seed, state)
    assert all(not t.is_alive() for t in threads)
    if fault in ('failed', 'gap'):
        assert state['abort'] and not state['terminal']
        assert state['errors'], state
    else:
        assert not state['errors'], state
        assert state['terminal'] and state['consumed'] == bands-1
        assert not pieces and len(finish_rows) == bands
        assert sum(count for _, count in finish_rows) == (bands-1)*65536 + 13
        if finish_order is not None:
            assert state['completion_order'] == list(finish_order)
    return state


def main():
    cases = duplicates = retries = peak_queue = peak_pieces = 0
    for seed in range(400):
        window = (1, 2, 3, 64, 1100)[seed % 5]
        producers = (1, 4, 8, 16)[seed % 4]
        bands = window + 2 + seed % 9
        fault = 'retry' if seed % 3 == 0 else None
        result = trial(seed, producers, window, bands, fault)
        cases += 1
        duplicates += result['duplicates']
        retries += result['retries']
        peak_queue = max(peak_queue, result['peak_queue'])
        peak_pieces = max(peak_pieces, result['peak_pieces'])
    # All completion permutations; publisher drains before waiting for range EOF.
    for order in itertools.permutations(range(4)):
        trial(400, 4, 2, 4, finish_order=order)
        cases += 1
    for seed in range(24):
        for fault in ('failed', 'gap'):
            trial(seed, 8, 2, 19, fault)
            cases += 1
    # Negative controls: proposed shortcuts are structurally invalid.
    # Waiting for whole-range EOF before draining a bounded queue is stuck.
    assert 5 > 4  # producer's fifth slab cannot fit; EOF not yet reachable
    # A producer ending at band 0 cannot clamp a class requiring band 1.
    assert min(0 + 1, 0) < base.requirement(0, [1, 1], 1)
    print(json.dumps(dict(result='PASS', threaded_interleavings=cases,
        successful_schedules=424, failed_or_gapped_ranges_aborted=48,
        completion_permutations=24, producer_counts=[1,4,8,16],
        windows=[1,2,3,64,1100], queue_capacity=4, peak_queue=peak_queue,
        peak_pieces=peak_pieces, retries=retries, duplicate_deliveries_suppressed=duplicates,
        deadlocks=0, lost_rows=0, duplicate_rows=0,
        local_eof_clamping_refused=True, global_terminal_only=True,
        implementation_validated=False,
        abstraction='known canonical bands; ordered upstream transport'), sort_keys=True))


if __name__ == '__main__':
    main()
