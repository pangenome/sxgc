"""Reproducible finite evidence for the remaining minimality obstruction.

No theorem about fixed MAXINT and arbitrarily long texts is inferred from
these tests. Requirements and maximal coverage classes are computed directly
from text; they do not use the scan/FM oracle.
"""

from collections import Counter
from itertools import product
import json

from o1_saturation_family import Triple, capped_scan, triples_of


def coverage_classes(text):
    text = tuple(text)
    n = len(text)
    words = {text[i:j] for i in range(n + 1) for j in range(i, n + 1)}
    requirements = set()
    for word in words:
        d = len(word)
        extensions = {
            text[i + d]
            for i in range(n - d)
            if text[i:i + d] == word
        }
        is_suffix = text[n - d:] == word
        if not word or is_suffix or len(extensions) >= 2:
            requirements.update(word + (c,) for c in extensions)
    scopes = {
        x: frozenset(w for w in requirements if len(w) <= x and text[x-len(w):x] == w)
        for x in range(1, n + 1)
    }
    maximal = {s for s in scopes.values() if s and not any(s < t for t in scopes.values())}
    return requirements, scopes, maximal


def predicted_two_block_stream(a, b):
    assert a >= 1 and b >= 1
    return (
        [Triple(1, max(0, r - 1), a + b + 1 - r) for r in range(a)]
        + [Triple(2, a - 1, b + 1), Triple(0, min(a, b), 0)]
        + [Triple(1, b - k - 1, k + 1) for k in range(b)]
    )


def audit_family():
    cases = 0
    for a in range(1, 21):
        for b in range(1, 21):
            text = [1] * a + [2] + [1] * b
            ts = triples_of(text)
            assert ts == predicted_two_block_stream(a, b), (a, b, ts)
            requirements, scopes, maximal = coverage_classes(text)
            assert len(maximal) == 2, (a, b, maximal)
            # A two-position cover and two disjoint epsilon requirements
            # independently establish chi=2 for each checked instance.
            unary_position = a if a >= b else len(text)
            assert scopes[unary_position] | scopes[a + 1] == requirements
            assert (1,) in requirements and (2,) in requirements
            for cap in range(22):
                out = capped_scan(len(text) + 1, ts, cap)
                count = Counter(out)
                duplicate_marker = cap < min(a - 1, b)
                extra_unary = cap < min(a - 1, b - 1)
                assert count[a + 1] == 1 + duplicate_marker, (a, b, cap, out)
                assert len(out) == 2 + duplicate_marker + extra_unary, (a, b, cap, out)
                assert set(out) <= {a, a + 1, len(text)}, (a, b, cap, out)
                assert count[a] <= 1 and count[len(text)] <= 1
                assert (a in out and len(text) in out) == extra_unary
                if extra_unary:
                    if a == b:
                        assert scopes[a] == scopes[len(text)]
                        assert scopes[a] in maximal
                    elif a < b:
                        assert scopes[a] < scopes[len(text)]
                    else:
                        assert scopes[len(text)] < scopes[a]
                cases += 1
    return cases


def audit_full_length():
    counts = Counter()
    for n in range(11):
        for text in product((1, 2), repeat=n):
            requirements, scopes, maximal = coverage_classes(text)
            ts = triples_of(list(text))
            # All true LCPs are below this cap. This is scanCap n,
            # not an unbounded theorem about the fixed MAXINT definition.
            out = capped_scan(n + 1, ts, n)
            assert len(out) == len(set(out)), (text, out, "O2")
            assert all(scopes[x] in maximal for x in out), (text, out, "O3")
            assert len({scopes[x] for x in out}) == len(out), (text, out, "O4")
            covered = set().union(*(scopes[x] for x in out))
            assert covered == requirements, (text, out, "covering")
            assert len(out) == len(maximal), (text, out, "cardinality")
            counts["texts"] += 1
    return dict(counts)


if __name__ == "__main__":
    print(json.dumps({
        "two_block_cases": audit_family(),
        "full_length_cap_binary_n_le_10": audit_full_length(),
        "failed_assertions": 0,
        "scope": "Finite evidence only; no fixed-MAXINT text refutation claimed.",
    }, indent=2))
