#!/usr/bin/env python3
"""Summarize PFP dictionary and parse without loading either file as a blob."""
import argparse
from array import array
from collections import Counter
import json
from pathlib import Path
import struct


def quantile(hist, fraction):
    target = int((sum(hist.values()) * fraction) + 0.999999999)
    seen = 0
    for length, count in sorted(hist.items()):
        seen += count
        if seen >= target:
            return length
    return 0


def summarize(prefix, input_bytes):
    prefix = Path(prefix)
    lengths = array('I', [0])  # phrase IDs are one based
    length = 0
    with Path(str(prefix) + '.dict').open('rb') as source:
        while block := source.read(8 << 20):
            for char in block:
                if char == 1:
                    lengths.append(length)
                    length = 0
                elif char == 0:
                    assert length == 0
                else:
                    length += 1
    assert length == 0
    dict_hist = Counter(lengths[1:])
    total_dict_bytes = sum(lengths)
    parse_hist = Counter()
    interior_hist = Counter()
    previous_length = None
    parse_bytes = Path(str(prefix) + '.parse').stat().st_size
    assert parse_bytes % 4 == 0
    with Path(str(prefix) + '.parse').open('rb') as source:
        while block := source.read(4 << 20):
            for (phrase_id,) in struct.iter_unpack('<I', block):
                assert 0 < phrase_id < len(lengths)
                current_length = lengths[phrase_id]
                parse_hist[current_length] += 1
                if previous_length is not None:
                    interior_hist[previous_length] += 1
                previous_length = current_length
    def distribution(hist):
        count = sum(hist.values())
        return dict(count=count, mean=sum(k*v for k,v in hist.items())/count,
                    p50=quantile(hist, .5), p90=quantile(hist, .9), max=max(hist))
    return dict(input_bytes=input_bytes, dictionary_bytes=total_dict_bytes,
                D_over_n=total_dict_bytes/input_bytes,
                distinct_phrases=len(lengths)-1, parse_phrases=parse_bytes//4,
                parse_bytes=parse_bytes, trigger_density=(parse_bytes//4-1)/input_bytes,
                phrase_lengths_occurrences=distribution(parse_hist),
                phrase_lengths_nonterminal=distribution(interior_hist),
                terminal_phrase_length=previous_length,
                phrase_lengths_distinct=distribution(dict_hist))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('prefix')
    parser.add_argument('input')
    args = parser.parse_args()
    print(json.dumps(summarize(args.prefix, Path(args.input).stat().st_size), sort_keys=True))
