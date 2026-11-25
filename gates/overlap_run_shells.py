"""Lexical cuts inside x^9, with no further raw histogram. Every bound is on
weighted rows, hence also on distinct (word, offset) descriptors. Empty cuts
may be dropped only after an actual census. The decoder never sees a corpus.
"""
from dataclasses import dataclass

from overlap_m20_prefix_cuts import trigger8
from overlap_prefix_vocab_bounds import need


@dataclass(frozen=True)
class Shell:
    # side=-1 for END or a byte below x, +1 for a byte above x.
    side: int
    first: int
    past: int
    upper: int


def shells(x, rows9, exits8, max_phrase, slots):
    """Use a half-open interval of initial run lengths, not a raw rank cut.

    A maximal run of length L>=9 contributes L-8 positions to x^9 and
    at most one position of any specified initial run length j<=L.
    Per side and j>=a>=9 this gives min(exits8[side], rows9//(a-8)).
    """
    need(type(x) is int and 0 <= x < 256 and not trigger8(bytes([x])*8),
         'ordinary finite word may end inside the run')
    need(len(exits8) == 2 and all(type(v) is int and v >= 0 for v in exits8)
         and all(type(v) is int and v > 0 for v in (rows9,max_phrase,slots)),
         'invalid run-shell counts')
    end = min(max_phrase, rows9+8)  # one such position at every length 9..L
    need(end >= 9 and rows9 <= (end-8)*sum(exits8), 'inconsistent run envelope')
    cuts = []
    for side, count in zip((-1,1), exits8):
        if not count:
            continue
        a = 9
        part = []
        while a <= end:
            per = min(count, rows9//(a-8))
            need(0 < per <= slots, 'single run-length shell not proved to fit')
            width = min(end+1-a, slots//per)
            part.append(Shell(side, a, a+width, per*width))
            need(len(part)+len(cuts) <= 16384, 'shell count limit')
            a += width
        # x^j SMALL < x^(j+1) ... < x^k LARGE. Once the next byte
        # is above x (rather than below/END), lengths sort in reverse.
        cuts.extend(part if side < 0 else reversed(part))
    need(rows9 <= sum(c.upper for c in cuts), 'uncovered homogeneous row mass')
    return cuts


def shape(word, x):
    """Literal reference for tests. Exhaustion sorts below every byte (even 0)."""
    j = 0
    while j < len(word) and word[j] == x:
        j += 1
    return (-1 if j == len(word) or word[j] < x else 1), j


def locate(intervals, side, j):
    """Small generated oracle only; the prospective plan does not time lookups."""
    ids = [i for i,c in enumerate(intervals) if c.side == side and c.first <= j < c.past]
    need(len(ids) == 1, 'absent or overlapping run length')
    return ids[0]
