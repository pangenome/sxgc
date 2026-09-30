"""Finite evidence for saturation on actual suffix streams.

The scan below follows Sxgc.scan with MAXINT replaced by a small cap.
For cap M, investigate T = 1^(M+2) ++ [2] ++ 1^(M+1).  The suffix
array and LCP values are computed from T, not supplied as a stream.

The checked M=0..10 instances suggest a family extending to the fixed
MAXINT.  This script is not a Lean proof of that extrapolation or a
kernel refutation of a theorem about the fixed MAXINT.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class Triple:
    c: int
    lcp: int
    sa: int


def triples_of(text: list[int]) -> list[Triple]:
    reverse = text[::-1] + [0]
    suffix_array = sorted(range(len(reverse)), key=lambda i: reverse[i:])
    triples = []
    for row, sa in enumerate(suffix_array):
        lcp = 0
        if row:
            previous = suffix_array[row - 1]
            while (
                previous + lcp < len(reverse)
                and sa + lcp < len(reverse)
                and reverse[previous + lcp] == reverse[sa + lcp]
            ):
                lcp += 1
        triples.append(Triple(reverse[sa - 1] if sa else 0, lcp, sa))
    return triples


def capped_scan(size: int, triples: list[Triple], cap: int) -> list[int]:
    sigma = 128
    candidates = [(-1, 0, False) for _ in range(sigma)]
    emitted = []

    def evaluate(level: int) -> None:
        for char in range(1, sigma):
            length, position, active = candidates[char]
            if level < length:
                if active:
                    emitted.append(position)
                candidates[char] = (level, 0, False)

    def update(char: int, length: int, position: int) -> None:
        if length > candidates[char][0]:
            candidates[char] = (length, position, True)

    if not triples:
        return []
    previous = triples[0]
    minimum = cap
    for current in triples[1:]:
        minimum = min(minimum, current.lcp)
        if current.c != previous.c:
            evaluate(minimum)
            update(previous.c, current.lcp, max(0, size - previous.sa))
            update(current.c, current.lcp, max(0, size - current.sa))
            minimum = cap
        previous = current
    evaluate(-1)
    return emitted


def predicted_stream(cap: int) -> list[Triple]:
    """The proposed closed form, independently checked against suffix sorting."""
    initial = [Triple(1, max(0, row - 1), 2 * cap + 4 - row)
               for row in range(cap + 2)]
    boundaries = [Triple(2, cap + 1, cap + 2), Triple(0, cap + 1, 0)]
    final = [Triple(1, cap + 1 - sa, sa) for sa in range(1, cap + 2)]
    return initial + boundaries + final


if __name__ == "__main__":
    for cap in range(11):
        text = [1] * (cap + 2) + [2] + [1] * (cap + 1)
        actual = triples_of(text)
        assert actual == predicted_stream(cap), (cap, actual)
        output = capped_scan(len(text) + 1, actual, cap)
        assert output == [cap + 2, cap + 3, cap + 3], (cap, output)
        assert len(output) > len(set(output))
        # With a cap above all actual LCPs the duplicate disappears.
        assert capped_scan(len(text) + 1, actual, len(text)) == [cap + 2, cap + 3]
        print(f"cap={cap:2d} text_length={len(text):2d} actual_stream=True output={output}")
    print("11 finite capped instances verified; fixed-MAXINT extrapolation is unproved.")
