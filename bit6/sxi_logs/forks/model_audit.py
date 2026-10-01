#!/usr/bin/env python3
"""Read-only audit of assumptions needed by the proposed fork matrix.

The optional corpus scan reads a contiguous prefix once. It never edits an
artifact or rereads corpus bytes.
"""

import argparse
import json
import struct


def sxi_header(path):
    with open(path, "rb") as source:
        h = source.read(64)
        if h[:4] not in (b"SXI1", b"SXI2"):
            raise ValueError("not an SXI artifact")
        version = struct.unpack_from("<I", h, 4)[0]
        n, k, runs = struct.unpack_from("<QQQ", h, 8)
        member_count = struct.unpack_from("<I", h, 32)[0]
        directory = source.read(40 * member_count)
        members = {}
        for i in range(member_count):
            member_id, codec, offset, size, count = struct.unpack_from(
                "<IIQQQ", directory, 40 * i
            )
            members[member_id] = {
                "codec": codec,
                "offset": offset,
                "bytes": size,
                "count": count,
            }
    return {
        "magic": h[:4].decode(),
        "version": version,
        "text_bytes": n,
        "record_count": k,
        "run_count": runs,
        "chi_count": members[5]["count"],
        "member_ids": list(members),
        "has_move_map": 10 in members,
    }


def toy_counterexample():
    records = [b"AACGAAAT", b"AGGAGCCT", b"AATCACCT"]
    text = b"\x1e".join(records) + b"\x1e"
    rev = text[::-1]
    suffix_array = sorted(range(len(rev)), key=lambda i: rev[i:] + rev[:i])
    bwt = bytes(rev[(i - 1) % len(rev)] for i in suffix_array)
    rows = [i for i, sa in enumerate(suffix_array) if rev[sa : sa + 1] == b"A"]
    interval = bytes(bwt[i] for i in rows)
    run_count = 1 + sum(a != b for a, b in zip(interval, interval[1:]))
    branches = sorted(set(interval))
    per_record = [
        sorted({record[i + 1] for i in range(len(record) - 1) if record[i] == 65})
        for record in records
    ]
    assert run_count > len(branches)
    assert any(len(x) > 1 for x in per_record)
    return {
        "records": [x.decode() for x in records],
        "context": "A",
        "bwt_interval": interval.decode("ascii"),
        "run_count": run_count,
        "distinct_branch_count": len(branches),
        "record_following_A": [bytes(x).decode("ascii") for x in per_record],
    }


def scan_corpus_prefix(path, names_path, n, record_limit):
    """Count records realizing multiple following characters after A."""
    records = []
    with open(names_path, "r", encoding="utf-8") as names:
        for line in names:
            _, fstart, length = line.rstrip("\n").split("\t")
            length = int(length)
            start = n - 1 - int(fstart) - length
            records.append((start, length))
    records.sort()
    if len(records) < record_limit:
        raise ValueError("fewer names than requested records")
    if any(a + length + 1 != b for (a, length), (b, _) in zip(records, records[1:])):
        raise ValueError("names do not partition the corpus")
    multi = 0
    with open(path, "rb") as source:
        for start, length in records[:record_limit]:
            if source.tell() != start:
                raise ValueError("names start does not match single-pass stream position")
            branches = set()
            previous = None
            left = length
            while left:
                segment = source.read(min(1 << 20, left))
                if not segment:
                    raise ValueError("corpus ended within a named record")
                left -= len(segment)
                if len(branches) < 2:
                    if previous == 65 and segment:
                        branches.add(segment[0])
                    cursor = 0
                    while len(branches) < 2:
                        cursor = segment.find(b"A", cursor)
                        if cursor < 0 or cursor + 1 == len(segment):
                            break
                        branches.add(segment[cursor + 1])
                        cursor += 1
                previous = segment[-1]
            multi += len(branches) >= 2
            if len(source.read(1)) != 1:
                raise ValueError("missing record separator")
    return {"records_checked": record_limit, "records_with_multiple_A_branches": multi}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--yeast-sxi")
    parser.add_argument("--hprc-sxi")
    parser.add_argument("--corpus")
    parser.add_argument("--names")
    parser.add_argument("--records", type=int, default=100)
    args = parser.parse_args()
    result = {"toy": toy_counterexample()}
    if args.yeast_sxi:
        result["yeast_artifact"] = sxi_header(args.yeast_sxi)
    if args.hprc_sxi:
        result["hprc_artifact"] = sxi_header(args.hprc_sxi)
    if args.corpus:
        if not args.names or not args.yeast_sxi:
            parser.error("--corpus requires --names and --yeast-sxi")
        result["corpus_prefix"] = scan_corpus_prefix(
            args.corpus, args.names, result["yeast_artifact"]["text_bytes"], args.records
        )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
