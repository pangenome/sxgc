#!/usr/bin/env python3
"""Small exhaustive checker for the proposed incremental-chi merge invariant.

This implements the finite-text definitions in lean/Sxgc.lean (requirements,
coversAt, ScopeLe, IsMax), not the LCP scan. It is deliberately limited to
tiny strings and does not read corpus files.
"""
from itertools import product

SEP = 30


def requirements(t):
    substrings = {t[i:j] for i in range(len(t)) for j in range(i + 1, len(t) + 1)}
    substrings.add(())
    out = set()
    for w in substrings:
        ext = {t[i + len(w)] for i in range(len(t) - len(w)) if t[i:i + len(w)] == w}
        if not w or t[-len(w):] == w or len(ext) >= 2:
            out.update((w, c) for c in ext)
    return out


def scopes(t):
    req = requirements(t)
    return [frozenset((w, c) for w, c in req
                      if t[:x][-len(w) - 1:] == w + (c,))
            for x in range(1, len(t) + 1)]


def maximal(sc):
    return {i for i, a in enumerate(sc) if a and not any(a < b for b in sc)}


def chi(sc):
    # Lean's chi_eq_maxClasses characterization; count distinct maximal scopes.
    return len({sc[i] for i in maximal(sc)})


def cyclic_bwt(t):
    return tuple(t[(i - 1) % len(t)] for i in sorted(
        range(len(t)), key=lambda i: (t[i:] + t[:i], i)))


def search():
    alphabet = (1, 2)
    for alen in range(1, 7):
        for blen in range(1, 7):
            for a0 in product(alphabet, repeat=alen):
                a = a0 + (SEP,)
                old = scopes(a)
                for b0 in product(alphabet, repeat=blen):
                    b = b0 + (SEP,)
                    joined = a + b
                    new = scopes(joined)
                    old_max = maximal(old)
                    new_max = maximal(new)
                    revived = [i + 1 for i in range(len(a))
                               if i not in old_max and i in new_max]
                    # Same within-chunk suffix order need not survive joining.
                    local_order = sorted(range(len(a)), key=lambda i: (a[i:] + a[:i], i))
                    global_order = sorted(range(len(a)), key=lambda i: (joined[i:] + joined[:i], i))
                    if revived:
                        return {
                            "a": a, "b": b, "old_chi": chi(old),
                            "new_chi": chi(new), "revived": revived,
                            "old_scopes": old, "new_scopes": new,
                            "local_order": local_order,
                            "global_order": global_order,
                            "chunk_bwts": (cyclic_bwt(a), cyclic_bwt(b)),
                            "joined_bwt": cyclic_bwt(joined),
                        }
    return None


if __name__ == "__main__":
    found = search()
    if not found:
        raise SystemExit("no counterexample in search range")
    for key, value in found.items():
        print(f"{key}: {value}")
