#!/usr/bin/env python3
"""Exhaust all schedules of the abstract reserved-window/two-queue protocol.

This is a design check, not a proof of the legacy LCP or seam wire protocols.
Each model band produces one independently readable piece.
"""
from collections import deque


def check(n, k, p):
    # admitted prefix, sorting, completed queue, private window, emitted, pieces, finished
    initial = (0, 0, 0, 0, 0, 0, 0)
    seen = {initial}
    todo = deque([initial])
    terminal = 0
    while todo:
        state = todo.popleft()
        admitted, sorting, ready, private, emitted, pieces, finished = state
        assert (sorting | ready).bit_count() <= k
        assert private.bit_count() <= k and 0 <= pieces <= p
        if finished == n:
            terminal += 1
            continue
        successors = []
        if admitted < n and (sorting | ready).bit_count() < k:
            successors.append((admitted+1, sorting | 1 << admitted, ready, private, emitted, pieces, finished))
        first = emitted // k * k
        end = min(first+k, n)
        mask = ((1 << end)-1) ^ ((1 << first)-1)
        for band in range(n):
            bit = 1 << band
            if sorting & bit:
                successors.append((admitted, sorting ^ bit, ready | bit, private, emitted, pieces, finished))
            if ready & bit & mask:
                successors.append((admitted, sorting, ready ^ bit, private | bit, emitted, pieces, finished))
        if emitted < n and private & mask == mask and pieces < p:
            successors.append((admitted, sorting, ready, 0 if emitted+1 == end else private,
                               emitted+1, pieces+1, finished))
        if pieces:
            successors.append((admitted, sorting, ready, private, emitted, pieces-1, finished+1))
        assert successors, f'deadlock n={n} K={k} P={p}: {state}'
        for next_state in successors:
            if next_state not in seen:
                seen.add(next_state)
                todo.append(next_state)
    assert terminal
    return len(seen)


for k in range(1, 5):
    for p in range(1, 4):
        print(f'PASS abstract progress K={k} P={p} states={check(7,k,p)}')
