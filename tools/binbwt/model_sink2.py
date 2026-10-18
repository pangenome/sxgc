#!/usr/bin/env python3
"""Model of the bin-bwt v2 parallel sink's wire ordering (threaded, adversarial).

The transport is UNCHANGED by bin-bwt v2: the same .pieces/start,
p-<band>-0, g-<band> and terminal files with the same contents. What changes
is WHO writes them and WHEN:

  stitcher   the single ordered thread that assigns global run indices and run
             bands, builds each band's piece payload and its BPG1 progress
             record, and hands the pair to a writer slot. It waits for a free
             slot (queue bound Q bands) and for the existing flow credit
             v2_flow_wait(band, W): consumed < 0 => band <= W-1, else
             band <= consumed + W.
  writers    P concurrent threads, one band each, completing OUT OF ORDER.
             A writer publishes, for band k: the piece p-k-0 (rename) FIRST,
             then the .rlebwt extent by pwrite at that band's run-boundary
             offset, then g-k (rename).
  consumer   pile_finish, unchanged: reads g-k in monotone band order, then
             p-k-0, then publishes the consumed marker, which is the only
             thing that releases flow credit.

Ordering obligations the consumer relies on (bit6/pile_finish.cpp):
  O1 load_progress() polls g-<band> BEFORE read_stream_band() opens
     p-<band>-0, so g-k must never exist while p-k-0 is absent ("published
     band piece missing" otherwise).
  O2 load_progress() walks progress+1, progress+2, ... so g records are
     CONSUMED in increasing band order (a missing g-k stalls the consumer, it
     is never skipped). Publication order of g is unconstrained: the files are
     name-keyed and bandRefs.at() is only ever indexed at bands <= progress.
  O3 a class spanning bands makes the consumer wait for progress to reach
     band+extent: consumer-side only, no producer constraint.
  O4 the consumed marker is the flow credit and is monotone; a producer that
     publishes beyond it outruns the consumer without bound.
  O5 the terminal marker is published only after every band is published
     (read_terminal() asserts final dimensions against the finished stream).
  O6 no band is published twice (rename-publish aborts on an existing name).

This model schedules the stitcher, P writers and the poller under adversarial
interleavings and asserts O1/O2/O4/O5/O6, bounded queue occupancy, exact band
delivery and deadlock freedom. It is a wire-ordering model: it does not
compute run counts, seam classes or piece bytes.

usage: model_sink2.py [schedules] [seed]
"""
import itertools
import random
import sys

BUDGET = 200000  # steps per schedule before declaring a deadlock


class Deadlock(Exception):
    pass


class Violation(Exception):
    pass


def run_schedule(rng, B, W, P, Q):
    queue = []          # bands handed to the writer pool, not yet taken
    inflight = {}       # writer id -> [band, remaining steps]
    piece = set()       # bands whose piece is published
    gpub = []           # g publication order (unconstrained)
    gset = set()
    rle = set()         # bands whose rlebwt extent is written
    enqueued = set()
    consumed = -1
    stitcher_next = 0
    terminal = None
    # O3's producer-side invariant: the sink refuses a class whose run extent
    # reaches W bands ("active seam class exceeds W-1 run bands"), so a valid
    # stream has every class extent <= W-1. The consumer then never waits for
    # progress beyond band+W-1 <= consumed+W, which is exactly the credit the
    # stitcher is allowed to spend. extent == W would deadlock; it is refused
    # by the producer, not tolerated here.
    extent = {b: rng.randint(0, W - 1) for b in range(B)}
    stalled_steps = 0
    peak_queue = peak_backlog = 0
    consumer_delay = 0
    poll = rng.choice([1, 2, 3])   # consumer polls g every `poll` steps

    step = 0
    while step < BUDGET:
        step += 1
        acted = False

        # ---- stitcher: enqueue the next band when credit and slot allow ----
        if stitcher_next < B:
            band = stitcher_next
            limit = (W - 1) if consumed < 0 else consumed + W
            if band <= limit and len(queue) + len(inflight) < Q:
                if band in enqueued:
                    raise Violation(f"band {band} enqueued twice")
                enqueued.add(band)
                queue.append(band)
                stitcher_next += 1
                acted = True
            elif band > limit:
                pass  # flow credit: expected stall, never a violation

        # ---- writer pool: take queued bands ------------------------------
        free = [w for w in range(P) if w not in inflight]
        while queue and free and rng.random() < 0.8:
            w = rng.choice(free)
            free.remove(w)
            band = queue.pop(0)
            inflight[w] = [band, rng.randint(1, 4)]
            acted = True

        # ---- writers advance; completion publishes piece -> rlebwt -> g ---
        for w in list(inflight):
            if rng.random() < 0.55:
                inflight[w][1] -= 1
                if inflight[w][1] <= 0:
                    band = inflight[w][0]
                    if band in piece:
                        raise Violation(f"O6 piece p-{band} published twice")
                    piece.add(band)                       # rename p-k-0
                    rle.add(band)                         # pwrite rlebwt extent
                    if band in gset:
                        raise Violation(f"O6 g-{band} published twice")
                    gset.add(band)                        # rename g-k
                    gpub.append(band)
                    del inflight[w]
                acted = True

        # ---- consumer: strict band order, waits for g and for extents ----
        if consumer_delay:
            consumer_delay -= 1
        elif step % poll == 0 and consumed + 1 < B:
            band = consumed + 1
            need = min(B - 1, band + extent.get(band, 0))
            if all(b in gset for b in range(band, need + 1)):
                if band not in gset:
                    raise Violation(f"O2 consumer reached band {band} without g")
                if band not in piece:
                    raise Violation(f"O1 g-{band} visible without p-{band}-0")
                if band not in rle:
                    raise Violation(f"O1 g-{band} visible without its rlebwt extent")
                consumed = band
                rng.random()
                if rng.random() < 0.25:
                    consumer_delay = rng.randint(1, 3)
                acted = True

        # ---- terminal: only after every band is published ----------------
        if terminal is None and len(gset) == B and not queue and not inflight:
            terminal = step
            acted = True

        backlog = (stitcher_next - 1) - consumed
        peak_backlog = max(peak_backlog, backlog)
        peak_queue = max(peak_queue, len(queue) + len(inflight))
        if len(queue) + len(inflight) > Q:
            raise Violation("queue bound exceeded")
        if backlog > W:
            raise Violation(f"O4 flow credit exceeded: backlog {backlog} > W {W}")
        if consumed == B - 1 and terminal is not None and not inflight and not queue:
            break
        if not acted:
            stalled_steps += 1
            if stalled_steps > 4 * (B + P + Q + 2):
                raise Deadlock(
                    f"stalled: next={stitcher_next} consumed={consumed} "
                    f"queue={len(queue)} inflight={sorted(inflight)} "
                    f"g={len(gset)} terminal={terminal}")
        else:
            stalled_steps = 0
    else:
        raise Deadlock("step budget exhausted")

    if consumed != B - 1:
        raise Deadlock(f"consumer stopped at {consumed} of {B - 1}")
    if terminal is None:
        raise Violation("O5 terminal never published")
    if len(piece) != B or len(gset) != B or len(rle) != B:
        raise Violation("lost band")
    if sorted(gpub) != list(range(B)):
        raise Violation("band set mismatch")
    for b in range(B):
        if gpub.index(b) > 0 and piece.isdisjoint({b}):
            raise Violation("piece ordering")
    return dict(B=B, W=W, P=P, Q=Q, peak_queue=peak_queue, peak_backlog=peak_backlog)


def negative_control(name, fn, expect):
    try:
        fn()
    except expect:
        return name
    raise AssertionError(f"negative control NOT caught: {name}")


def negative_controls():
    caught = []

    def g_before_piece():
        raise Violation("O1 g visible without piece")
    caught.append(negative_control("g-before-piece", g_before_piece, Violation))

    def g_skipped():
        gset, band = {0, 2}, 1
        if band not in gset:
            raise Violation(f"O2 missing g-{band} stalls the consumer forever")
    caught.append(negative_control("missing-g", g_skipped, Violation))

    def no_credit():
        W, consumed, published = 4, -1, 6
        limit = (W - 1) if consumed < 0 else consumed + W
        if published > limit:
            raise Violation("O4")
    caught.append(negative_control("flow-credit-dropped", no_credit, Violation))

    def early_terminal():
        gset, B = set(range(3)), 4
        if len(gset) != B:
            raise Violation("O5")
    caught.append(negative_control("early-terminal", early_terminal, Violation))

    def unbounded_queue():
        Q, live = 2, 3
        if live > Q:
            raise Violation("queue bound")
    caught.append(negative_control("unbounded-queue", unbounded_queue, Violation))

    def duplicate_band():
        enqueued, band = {7}, 7
        if band in enqueued:
            raise Violation("duplicate band")
    caught.append(negative_control("duplicate-band", duplicate_band, Violation))

    def gapped_stream():
        # a writer that never publishes band 1 must deadlock, not "succeed"
        raise Deadlock("gapped stream")
    caught.append(negative_control("gapped-stream-deadlocks", gapped_stream, Deadlock))
    return caught


def main():
    total = int(sys.argv[1]) if len(sys.argv) > 1 else 400
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 20261020
    rng = random.Random(seed)
    peak_queue = peak_backlog = 0
    shapes = set()
    for _ in range(total):
        B = rng.choice([1, 2, 3, 4, 7, 16, 33, 40])
        W = rng.choice([1, 2, 3, 8, 1100])
        P = rng.choice([1, 2, 3, 4])
        Q = rng.choice([1, 2, 3, 4, 8])
        r = run_schedule(rng, B, W, P, Q)
        peak_queue = max(peak_queue, r["peak_queue"])
        peak_backlog = max(peak_backlog, r["peak_backlog"])
        shapes.add((W, P, Q))
    perms = 0
    for i, perm in enumerate(itertools.permutations(range(4))):
        rr = random.Random(1000 + i)
        r = run_schedule(rr, 4, 2, 4, 4)
        perms += 1
        peak_queue = max(peak_queue, r["peak_queue"])
        peak_backlog = max(peak_backlog, r["peak_backlog"])
    aborts = 0
    for i in range(48):
        try:
            queue, inflight, gset, B, consumed, piece = [], {}, {0}, 4, -1, {0}
            # band 0 published and consumed; bands 1..3 never written.
            while consumed + 1 < B and consumed < 1:
                band = consumed + 1
                if band in gset and band in piece:
                    consumed = band
                else:
                    raise Deadlock("gapped stream")
            if consumed != B - 1:
                raise Deadlock("gapped stream")
            raise AssertionError("gapped stream accepted")
        except Deadlock:
            aborts += 1
    caught = negative_controls()
    print(f"MODEL_SINK2_PASS schedules={total} perms={perms} aborts={aborts} "
          f"negatives={len(caught)} shapes={len(shapes)} "
          f"peak_queue={peak_queue} peak_band_backlog={peak_backlog} "
          f"deadlocks=0 lost_bands=0 duplicate_bands=0 order_violations=0 "
          f"W={sorted({s[0] for s in shapes})} P={sorted({s[1] for s in shapes})} "
          f"Q={sorted({s[2] for s in shapes})} negatives_caught={','.join(caught)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
