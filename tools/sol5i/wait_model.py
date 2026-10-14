#!/usr/bin/env python3
"""Executable abstract model of the no-shard sol5i protocol.

Zero-based bands; N is the last band, P=C=-1 initially. Atomic progress
publication implies both piece publication and capture of all rows/flanks
needed by classes ending in that band. Terminal publication validates all
captures. This model does not assume an implementation of those implications.
"""
import json
import random


def requirement(band, extents, last):
    return min(band + extents[band], last)


def validate_window(window, extents):
    if window < 1 or any(e < 0 or e >= window for e in extents):
        raise ValueError('class extent exceeds W-1')


def trial(seed, window, last, terminal_class=False, stall_side=None):
    rng = random.Random(seed)
    extents = [0] * (last + 1)
    classes = []
    # Disjoint classes mirror the maximal seam intervals retained by SCS1.
    lo = 0
    while lo <= last:
        if rng.randrange(5) == 0:
            hi = min(last, lo + rng.randrange(window))
            classes.append((lo, hi))
            extents[lo:hi+1] = [hi-lo] * (hi-lo+1)
            lo = hi + 1
        else:
            lo += 1
    if terminal_class and last:
        classes = [(lo, hi) for lo, hi in classes if hi < last-1]
        classes.append((last-1, last))
        extents = [0] * (last+1)
        for lo, hi in classes:
            extents[lo:hi+1] = [hi-lo] * (hi-lo+1)
    validate_window(window, extents)
    progress = consumed = -1
    terminal = False
    pieces = {}
    capture = set()
    consumed_runs = 0
    band_size = 65536
    published_runs = 0
    peak = 0
    steps = 0
    stalled = 0
    while consumed < last or not terminal:
        # Three independently schedulable actions: producer write+publish,
        # terminal rename, consumer readiness+consume. Stalls are finite.
        producer = progress < last and progress+1 <= consumed+window
        finish = consumed < last and (terminal or progress >= requirement(consumed+1, extents, last))
        terminal_ready = progress == last and not terminal
        assert producer or finish or terminal_ready, (seed, progress, consumed, extents)
        choices = []
        if producer:
            choices.append('producer')
        if finish:
            choices.append('finish')
        if terminal_ready:
            choices.append('terminal')
        if stalled < 2*window and stall_side in choices and len(choices) > 1:
            choices.remove(stall_side)
            stalled += 1
        action = rng.choice(choices)
        if action == 'producer':
            band = progress + 1
            assert band <= consumed + window
            # Fixed run bands: only EOF may contain a partial band.
            count = band_size if band < last else rng.randrange(1, band_size + 1)
            pieces[band] = (band * band_size, count)
            published_runs += count
            for index, (_, hi) in enumerate(classes):
                if hi == band:
                    capture.add(index)
            progress = band
            peak = max(peak, len(pieces))
            assert peak <= window
        elif action == 'terminal':
            assert len(capture) == len(classes)
            terminal = True
        else:
            band = consumed+1
            assert terminal or progress >= requirement(band, extents, last)
            for index, (lo, hi) in enumerate(classes):
                if lo <= band <= hi:
                    assert index in capture
            base, count = pieces.pop(band)
            assert base == band * band_size == consumed_runs
            consumed_runs += count
            consumed = band
        steps += 1
        assert steps <= 2*(last+1)+1
    assert not pieces and consumed_runs == published_runs
    return peak


def main():
    cases = peak = 0
    for seed in range(400):
        window = (2, 3, 8, 64, 1100)[seed % 5]
        last = window + seed % 17
        for stall in (None, 'producer', 'finish'):
            peak = max(peak, trial(seed, window, last, True, stall))
            cases += 1
    # Explicit minimal/terminal cases and maximum admitted class extent.
    for window in (1, 2, 3, 64, 1100):
        for last in (0, 1, window-1, window, window+1):
            for seed in range(10):
                trial(seed, window, last)
                cases += 1
        validate_window(window, [window-1])
        for invalid in ([-1], [window]):
            try:
                validate_window(window, invalid)
            except ValueError:
                pass
            else:
                raise AssertionError('malformed extent accepted')
    inequalities = 0
    for window in (1, 2, 3, 64, 1100):
        for extent in range(window):
            for consumed in (-1, 0, 10):
                assert consumed+window >= consumed+1+extent
                inequalities += 1
    print(json.dumps(dict(result='PASS', randomized_interleavings=cases,
                         deadlocks=0, terminal_cases=1200,
                         active_producer_inequalities=inequalities,
                         maximum_backlog=peak, malformed_extents_refused=10,
                         implementation_validated=False, shards=0, band_size=65536,
                         bases='65536*b'), sort_keys=True))


if __name__ == '__main__':
    main()
