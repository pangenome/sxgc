"""Standalone tiny materialisations. No archive, original, suffix spool or build."""
from collections import Counter, defaultdict
from itertools import product
import random

from overlap_run_shells import locate, shape, shells


def run_bags():
    # Enumerate three maximal runs, with 7/8 byte cases that can increase the
    # exit count without contributing any x^9 row. Include one-sided sets.
    choices = list(product((7,8,9,10,13,20), (-1,1)))
    fit = refused = exact = 0
    for runs in product(choices,repeat=3):
        true = Counter((s,j) for length,s in runs for j in range(9,length+1))
        if not true:
            continue
        n = sum(true.values()); e = [sum(s == side and length >= 8 for length,s in runs) for side in (-1,1)]
        for cap in (1,2,3,5,12,23):
            try:
                groups = shells(32,n,e,max(k[1] for k in true),cap)
            except ValueError as err:
                assert str(err) == 'single run-length shell not proved to fit' and min(max(e),n)>cap
                refused += 1; continue
            fit += 1; actual = Counter(); pairs = []
            for (s,j),mass in true.items():
                index = locate(groups,s,j); actual[index] += mass
                pairs.append((b' '*j + (b'\x00' if s < 0 else b'A'),index))
            order = [k for _,k in sorted(pairs)]
            assert order == sorted(order) and sum(actual.values()) == n
            for i,g in enumerate(groups):
                assert actual[i] <= g.upper <= cap
                exact += int(actual[i] == g.upper)
    assert fit and refused and exact
    return dict(passing_schedules=fit,conservative_refusals=refused,tight_group_upper_checks=exact)


def hit(s):
    v = 1469598103934665603
    for b in s:
        v = ((v ^ b)*1099511628211) % 2**64
    return v%31 == 0


def occurrences(t):
    marks = [0] + [i for i in range(1,len(t)-7) if hit(t[i:i+8])]
    for i,a in enumerate(marks):
        terminal = i+1 == len(marks)
        past = len(t) if terminal else marks[i+1]+8
        w = t[a:past]
        for j in range(len(w) if terminal else len(w)-8):
            yield a+j,terminal,w,j


def explicit(t,x):
    prefix = bytes([x])*9
    rec = list(occurrences(t)); assert len(rec) == len(t)
    ids = [p for p in range(len(t)) if t.startswith(prefix,p)]
    n = len(ids); assert n > 0
    # Raw clipped-by-text-EOF ONLY. A new run end never comes from a literal
    # doc delimiter masquerading as a boundary here.
    adj = [t[i+8] if i+8<len(t) else -1 for i in range(len(t)) if t.startswith(prefix[:8],i)]
    e = [sum(y<x for y in adj),sum(y>x for y in adj)]
    cap = max(max(e), 7); groups = shells(x,n,e,max(len(w) for _,_,w,_ in rec),cap)
    route = {}; descriptors = defaultdict(set); keys = defaultdict(set); finite = []
    for p,eof,w,j in rec:
        s = w[j:]; assert s.startswith(prefix) == t.startswith(prefix,p)
        if s.startswith(prefix):
            a,b = shape(s,x); assert (a,b) == shape(t[p:],x)
            k = locate(groups,a,b); route[p] = k; descriptors[k].add((w,eof,j)); keys[s,eof].add(k); finite.append((s,eof,k))
    assert all(len(z)==1 for z in keys.values())
    raw_order = [route[i] for i in sorted(ids,key=lambda i:t[i:])]
    finite_order = [k for _,_,k in sorted(finite)]
    assert raw_order == sorted(raw_order) == finite_order
    count = Counter(raw_order)
    for i,g in enumerate(groups):assert len(descriptors[i]) <= count[i] <= g.upper <= cap
    return n, sum(map(len,descriptors.values())),len(keys),len(groups)


def fixtures():
    bags = run_bags(); totals = [0]*4; trials = 0
    for x in (0,32,45,255):
        rnd = random.Random(81345+x)
        unit = b'PX'+rnd.randbytes(90)+bytes([x])*40+b'?'+rnd.randbytes(40)+bytes([x])*13+b'\x00\xff'
        # Different bytes immediately before the run force distinct ordinary
        # words. x^7+y is an actual common ending trigger; all tails of length
        # >=9 ending there (from inside the run) tie across those records.
        y = next(b for b in range(256) if b!=x and hit(bytes([x])*7+bytes([b])))
        tied = b''.join(bytes([b])+bytes([x])*35+bytes([y])+b'end' for b in (77,91,77,91))
        cases = (bytes([x])*90, unit*6+bytes([x])*70, tied,
                 b''.join(bytes([y])+bytes([x])*n for y,n in [(1,7),(0,8),(222,9),(71,10),(2,17),(127,37),(34,12)]))
        for t in cases:
            z = explicit(t,x); trials += 1
            if t==tied:assert z[1]>z[2]
            for i,v in enumerate(z):totals[i] += v
    assert totals[0]>totals[1]>totals[2] and trials==16
    # A genuine counterexample: the comma eight-mer triggers. Distinct raw
    # run lengths would scatter occurrences of even the SAME descriptor.
    t = b','*44; mapping = defaultdict(set)
    for p,e,w,j in occurrences(t):
        if w[j:] == b','*9:mapping[w,e,j].add(shape(t[p:],44))
    bad = max(map(len,mapping.values())); assert hit(b','*8) and bad > 1
    for args,reason in [((44,30,[0,1],50,8),'ordinary finite word'),((32,27,[0,2],10,8),'inconsistent run envelope'),
                        ((32,30,[1,12],20,8),'single run-length'),((32,1,[-1,2],12,2),'invalid run-shell counts')]:
        try:shells(*args)
        except ValueError as err:assert str(err).startswith(reason)
        else:raise AssertionError('bad shell premises accepted')
    return dict(**bags,independent_w8_texts=trials,homogeneous_rows=totals[0],distinct_descriptors=totals[1],finite_keys=totals[2],nominal_ranges=totals[3],
                distinct_raw_run_lengths_for_one_descriptor_in_refused_trigger_case=bad,explicit_policy_refusals=4)

if __name__=='__main__':
    import json
    assert __debug__; print(json.dumps(fixtures(),sort_keys=True))
