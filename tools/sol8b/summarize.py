#!/usr/bin/env python3
"""Render exact census and explicitly unqualified sensitivity columns."""
import json, math, sys
rows=[json.loads(line) for line in open(sys.argv[1])]
print('|d|buckets|one-chunk mass|all-eight mass|weighted log2 C|pile log2 E[C]|global-mean proxy symbols|')
print('|--:|--:|--:|--:|--:|--:|--:|')
for r in rows:
    print(f"|{r['d']}|{r['buckets']:,}|{r['one_chunk_mass']:,}|{r['all_chunks_mass']:,}|{r['weighted_log2_contributors']:.6f}|{r['pile_poisson_weighted_log2_expected_contributors']:.6f}|{r['proxy_symbols_k8']:,}|")
print('Global-mean proxies are NOT conditional-LCE estimates or measured hybrid counters.')
