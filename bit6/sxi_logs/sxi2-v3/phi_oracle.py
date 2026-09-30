#!/usr/bin/env python3
"""Independent cyclic-SA oracle for a conservative hybrid phi escape rule.

C(i) is true if the gcd of positive character frequencies is one, or if the
SA-value domain of tail run i contains one value.  Both tests use retained
publish-path inputs (the C table and tail samples) only.
For a failed run the oracle records the *actual successor for every value* in
the interval, not just the run endpoint.  This distinction matters for cyclic
ties: one explicit head sample does not repair the entire affine interval.
"""
import bisect
import collections
import itertools
import math
import random
from escape_codec import Escape, encode


def primitive(t):
    n = len(t)
    return not any(n % p == 0 and t == t[:p] * (n // p)
                   for p in range(1, n))


def frequency_gcd(t):
    return math.gcd(*collections.Counter(t).values())


def check(t):
    n = len(t)
    sa = sorted(range(n), key=lambda i: t[i:] + t[:i])
    bwt = bytes(t[(i - 1) % n] for i in sa)
    ends = [i for i in range(1, n + 1) if i == n or bwt[i] != bwt[i - 1]]
    inverse = [0] * n
    for row, value in enumerate(sa):
        inverse[value] = row
    actual = [sa[(inverse[x] + 1) % n] for x in range(n)]
    # Preserve BWT run IDs as well as the SA-value sorting key.
    pairs = sorted((sa[e - 1], sa[e % n], run_id)
                   for run_id, e in enumerate(ends))
    domain_start = {run_id: u for u, _, run_id in pairs}
    domains = [[] for _ in ends]
    bad = []
    unsafe = set()
    for x in range(n):
        i = bisect.bisect_right(pairs, (x, n, n)) - 1
        u, v, run_id = pairs[i]
        if i < 0:
            u -= n
        predicted = (v + x - u) % n
        domains[run_id].append((x, predicted, actual[x]))
        if predicted != actual[x]:
            bad.append((x, predicted, actual[x], run_id))
    escape = {}
    safe = []
    for run_id, entries in enumerate(domains):
        entries.sort(key=lambda item: (item[0] - domain_start[run_id]) % n)
        c = frequency_gcd(t) == 1 or len(entries) == 1
        if c:
            assert all(predicted == actual_value
                       for _, predicted, actual_value in entries), (t, run_id, entries)
            safe.append(run_id)
        else:
            unsafe.add(run_id)
            escape[run_id] = [(x, actual_value) for x, _, actual_value in entries]
    assert {item[3] for item in bad} <= unsafe
    assert sum(map(len, domains)) == n
    assert all(escape[r] == [(x, a) for x, _, a in domains[r]] for r in escape)
    decoded = Escape(encode(n, len(ends), escape))
    for run_id, entries in enumerate(domains):
        for x, predicted, actual_value in entries:
            if run_id in escape:
                assert decoded.successor(run_id, x) == actual_value
            else:
                assert decoded.successor(run_id, x) is None
                assert predicted == actual_value
    return len(ends), len(safe), len(escape), sum(map(len, escape.values())), bad


def main():
    fixtures = [(b"banana\x1e", False), (b"ACG\x1e" * 4, True),
                (b"\x01\x02\x02" * 2, True)]
    for t, expected_bad in fixtures:
        r, safe, escaped, values, bad = check(t)
        assert bool(bad) == expected_bad
        print("fixture", repr(t), "runs", r, "safe", safe,
              "escaped_runs", escaped, "escaped_values", values,
              "first_mismatch", bad[:1])
    rng = random.Random(2)
    aperiodic = periodic = counterexamples = escaped_runs = escaped_values = 0
    gcd_one = 0
    for n in range(2, 31):
        for _ in range(1000):
            t = bytes(rng.randrange(3) for _ in range(n))
            r, safe, escaped, values, bad = check(t)
            if primitive(t):
                assert not bad
                aperiodic += 1
            else:
                periodic += 1
                counterexamples += bool(bad)
                escaped_runs += escaped
                escaped_values += values
            gcd_one += frequency_gcd(t) == 1
    print("random PASS", "aperiodic", aperiodic, "periodic", periodic,
          "periodic_counterexamples", counterexamples,
          "gcd_one", gcd_one,
          "periodic_escaped_runs", escaped_runs,
          "periodic_escaped_values", escaped_values)
    exhaustive = 0
    for n in range(2, 11):
        for symbols in itertools.product(range(3), repeat=n):
            check(bytes(symbols))
            exhaustive += 1
    print("exhaustive ternary PASS", exhaustive)
    stress = 0
    for p in range(1, 21):
        for copies in (2, 3, 4, 8, 16):
            for _ in range(20):
                base = bytes(rng.randrange(5) for _ in range(p))
                t = base * copies
                _, _, _, _, bad = check(t)
                assert not primitive(t)
                stress += 1
    print("periodic stress PASS", stress)


if __name__ == "__main__":
    main()
