"""Bounded single-pass raw census; no dictionary or row ordering."""
import ctypes as C
import hashlib

TEN = 10_000_000_000
CELLS = 256 * 257
U64 = (1 << 64) - 1


def checked(n):
    if not 0 <= n <= U64:
        raise OverflowError('u64 planning extent')
    return n


def native_chunk(lib):
    chunk = C.CDLL(str(lib)).scanplan_chunk
    chunk.restype = None
    ptr = C.POINTER(C.c_uint64)
    chunk.argtypes = [C.c_char_p, C.c_size_t, C.c_size_t, C.c_uint64, ptr, ptr]
    return chunk


def census(lib, stream, n, guard, block=1 << 20):
    if not 0 < n <= TEN or not 1 <= block <= 1 << 20:
        raise ValueError('wrong scan extent')
    cells = (C.c_uint64 * CELLS)()
    # Initial mandatory word starts at 0.
    state = (C.c_uint64 * 5)(0, 1, 0, 0, 0)
    chunk = native_chunk(lib)
    digest = hashlib.sha256()
    base, history = 0, b''
    while base < n:
        guard()
        data = stream.read(min(n-base, block))
        if not data:
            raise ValueError('short private input')
        digest.update(data)
        buf = history + data
        chunk(buf, len(history), len(buf), base, cells, state)
        base += len(data)
        history = buf[-8:]
    if stream.read(1):
        raise ValueError('long private input')
    start, m, high, longest, stride = state
    cells[history[-1]*257] += 1  # final owned suffix, second symbol END
    hist = [[i, v] for i, v in enumerate(cells) if v]
    if sum(v for _, v in hist) != n:
        raise ValueError('row coverage')
    return dict(n=n, sha256=digest.hexdigest(), m=m, starts_above_u32=high,
                eof_start=start, eof_raw_length=n-start, global_end_rows=1,
                max_phrase=max(longest, n-start), max_owned_stride=max(stride, n-start),
                alphabet=len({i//257 for i, _ in hist}), cells=hist)


def plan(c, arena):
    n, m = c['n'], c['m']
    if not 1 <= m <= n or arena < 8:
        raise ValueError('invalid planning inputs')
    d = checked(n+9*m-7)
    result = dict(k_upper=m, U_upper=n, D_upper=d, tape_upper=checked(d+3*m-1),
                  compact_tail_bytes_upper=checked(8*n), arena_bytes=arena,
                  descriptor_capacity=arena//8)
    peak_cell = max(v for _, v in c['cells'])
    # One cursor per descriptor in an equal-key group; groups cannot span cells.
    result['separate_heap_bytes_upper'] = checked(20*min(m, peak_cell))
    result['combined_tail_arena_and_heap_reservation'] = checked(arena+result['separate_heap_bytes_upper'])
    if m > 0xffffffff or max(c['max_phrase']+1, c['eof_raw_length']+2) > 0xffffffff:
        return dict(result, ranges=None, reason='compact word ID / offset not certified')
    ranges = []
    first = last = None
    used = 0
    for key, mass in c['cells']:
        if mass > arena//8:
            return dict(result, ranges=None, reason='indivisible prefix cell exceeds capacity', cell=key)
        if used and used+mass > arena//8:
            ranges.append([first, last, used]); used = 0
        if not used:
            first = key
        last = key
        used += mass
    ranges.append([first, last, used])
    return dict(result, ranges=ranges)
