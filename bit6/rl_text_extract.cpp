// rl_text_extract: LF-walk text accessor over a v4 .ri4 — GATE TOOL.
//
// Validates, against the flat text (oracle), that the .ri4 alone (rlbwt + C +
// run-end SA samples, one self-contained artifact) can serve every text access
// the xsa query layer needs. This is the "index is sovereign" claim turned
// into a gate: no AGC, no sidecar, no external text at query time.
//
// Usage:
//   rl_text_extract <file.ri4> <flat.txt> <k> [--steps W]
//     <flat.txt> = the '\n'-separated multi-string text that was indexed
//     (for revlines-built chains this is the revlines text itself).
//     k = number of strings.
//   Small n (<= 40M): full brute mode (suffix array of the equivalent flat
//     text in-memory) — validates in addition:
//       (3) the LF law sa[LF^j(e)] == sa[e] - j  (LF decrements SA by 1)
//       (4) the v4 sample formula at every run end:
//             S(r) == fsFwd[i] + (fend_i - sa[end_row(r)])
//           with fsFwd == fstart (plain-chain sidecar; revlines chains use
//           the names.tsv fstart, not checked here)
//       (5) W-step window emission from each run-end anchor:
//             emitted[j] == flat[(sa[e]-1-j) mod n]  (global wrap)
//   All modes always run:
//       (1) structural: sum(run_len) == n; C == prefix starts; per-char
//           totals == flat text counts (sentinels as 0x0A)
//       (2) inversion: walking LF from bare-sentinel row i emits string i
//           backward then its endmarker — the classic BWT inversion, and a
//           convention-blind end-to-end check of loader+C+rank+LF.
//
// Exit 0 iff all checks pass. See BIT_LADDER for gate records.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>
#include <atomic>

#include <sdsl/int_vector.hpp>

struct Ri4 {
    uint32_t version = 0;
    uint64_t n = 0, k = 0, R = 0;
    std::vector<uint64_t> C;                 // 256 F-column starts
    std::vector<unsigned char> run_char;
    std::vector<uint32_t> run_len;
    std::vector<uint64_t> sa_sample;          // v4: S = fsFwd + (fend - pos), run END rows

    std::vector<uint64_t> run_start_blk;
    static const uint64_t BLK = 64;
    std::vector<std::vector<uint32_t>> cruns;
    std::vector<std::vector<uint64_t>> csum;
    std::vector<uint64_t> total;

    void load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { std::cerr << "cannot open " << path << "\n"; exit(1); }
        uint32_t magic, sigma;
        f.read((char*)&magic, 4);
        if (!f || magic != 0x5258'5349u) { std::cerr << "bad magic\n"; exit(1); }
        f.read((char*)&version, 4);
        f.read((char*)&n, 8);
        if (version == 4) { f.read((char*)&k, 8); f.read((char*)&R, 8); }
        else { f.read((char*)&sigma, 4); f.read((char*)&R, 8); }
        C.resize(256); f.read((char*)C.data(), 256*8);
        run_char.resize(R); f.read((char*)run_char.data(), R);
        run_len.resize(R); f.read((char*)run_len.data(), 4*R);
        sa_sample.resize(R);
        if (version >= 3) {
            sdsl::int_vector<> sa_iv;
            sa_iv.load(f);
            if (sa_iv.size() < R) { std::cerr << "packed SA too small\n"; exit(1); }
            for (uint64_t r = 0; r < R; ++r) sa_sample[r] = sa_iv[r];
        } else if (version == 2) {
            f.read((char*)sa_sample.data(), 8*R);
        } else {
            std::vector<uint32_t> tmp(R); f.read((char*)tmp.data(), 4*R);
            for (uint64_t r = 0; r < R; ++r) sa_sample[r] = tmp[r];
        }
        f.close();

        run_start_blk.reserve(R/BLK + 2);
        uint64_t acc = 0;
        for (uint64_t r = 0; r < R; ++r) {
            if (r % BLK == 0) run_start_blk.push_back(acc);
            acc += run_len[r];
        }
        run_start_blk.push_back(acc);

        cruns.resize(256); csum.resize(256); total.assign(256, 0);
        std::vector<uint64_t> acc256(256, 0);
        for (uint64_t r = 0; r < R; ++r) {
            unsigned char c = run_char[r];
            cruns[c].push_back((uint32_t)r);
            csum[c].push_back(acc256[c]);
            acc256[c] += run_len[r];
        }
        for (int c = 0; c < 256; ++c) total[c] = acc256[c];
        for (int c = 0; c < 256; ++c) csum[c].push_back(total[c]);
    }

    uint64_t run_start(uint64_t r) const {
        uint64_t s = run_start_blk[r / BLK];
        for (uint64_t q = (r / BLK) * BLK; q < r; ++q) s += run_len[q];
        return s;
    }
    uint64_t run_of(uint64_t i) const {
        uint64_t lo = 0, hi = run_start_blk.size() - 1;
        while (lo + 1 < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (run_start_blk[mid] <= i) lo = mid; else hi = mid;
        }
        uint64_t r = lo * BLK;
        uint64_t s = run_start_blk[lo];
        while (r + 1 < R && s + run_len[r] <= i) { s += run_len[r]; ++r; }
        return r;
    }
    uint64_t rank(unsigned char c, uint64_t i) const {
        if (i == 0) return 0;
        if (i >= n) return total[c];
        uint64_t r = run_of(i);
        uint64_t s = run_start(r);
        const auto& v = cruns[c];
        uint64_t j = std::lower_bound(v.begin(), v.end(), (uint32_t)r) - v.begin();
        if (run_char[r] == c) return csum[c][j] + (i - s);
        return csum[c][j];
    }
    uint64_t lf(uint64_t i) const {
        uint64_t r = run_of(i);
        unsigned char c = run_char[r];
        return C[c] + rank(c, i);
    }
    unsigned char bwt_char(uint64_t i) const { return run_char[run_of(i)]; }
};

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: rl_text_extract <file.ri4> <flat.txt> <k> [--steps W]\n";
        return 1;
    }
    std::string ri4Path = argv[1], flatPath = argv[2];
    uint64_t K = strtoull(argv[3], nullptr, 10);
    uint64_t W = 32;
    for (int i = 4; i < argc; ++i)
        if (!strcmp(argv[i], "--steps") && i + 1 < argc) W = strtoull(argv[++i], nullptr, 10);

    Ri4 idx;
    idx.load(ri4Path);
    std::cerr << "ri4: version=" << idx.version << " n=" << idx.n
              << " k=" << idx.k << " R=" << idx.R << "\n";

    std::ifstream ff(flatPath, std::ios::binary);
    if (!ff) { std::cerr << "cannot open flat text\n"; return 1; }
    std::string flat((std::istreambuf_iterator<char>(ff)), std::istreambuf_iterator<char>());
    uint64_t n = flat.size();
    if (n != idx.n) { std::cerr << "FATAL: flat n=" << n << " != ri4 n=" << idx.n << "\n"; return 1; }

    std::vector<uint64_t> len(K), fstart(K), fend(K);
    {
        uint64_t f = 0;
        for (uint64_t i = 0; i < K; ++i) {
            uint64_t j = flat.find('\n', f);
            if (j == std::string::npos) { std::cerr << "FATAL: fewer than k strings\n"; return 1; }
            len[i] = j - f; fstart[i] = f; fend[i] = j; f = j + 1;
        }
        if (f != n) { std::cerr << "FATAL: trailing text after k-th newline\n"; return 1; }
    }

    int fails = 0;

    // ---- (1) structural ----
    {
        uint64_t acc = 0; bool ok = true;
        for (uint64_t r = 0; r < idx.R; ++r) acc += idx.run_len[r];
        if (acc != n) { std::cerr << "CHECK1 sum(run_len)=" << acc << " != n\n"; ok = false; }
        std::vector<uint64_t> cnt(256, 0);
        for (uint64_t i = 0; i < n; ++i) {
            unsigned char c = flat[i]; if (c == '\n') c = 0x0A;
            cnt[c]++;
        }
        for (int c = 0; c < 256; ++c)
            if (idx.total[c] != cnt[c]) {
                std::cerr << "CHECK1 char " << c << " total " << idx.total[c] << " != flat " << cnt[c] << "\n";
                ok = false;
            }
        uint64_t pre = 0;
        for (int c = 0; c < 256; ++c) {
            if (idx.C[c] != pre) {
                std::cerr << "CHECK1 C[" << c << "]=" << idx.C[c] << " != prefix " << pre << "\n";
                ok = false;
            }
            pre += idx.total[c];
        }
        if (ok) std::cerr << "CHECK1 structural: GREEN\n"; else ++fails;
    }

    // ---- (2) inversion: sentinel row i emits string i backward + endmarker ----
    {
        std::atomic<int> bad{0};
        std::atomic<uint64_t> done{0};
        uint64_t nthreads = std::min<uint64_t>(std::thread::hardware_concurrency(), K ? K : 1);
        std::vector<std::thread> th;
        auto worker = [&](uint64_t tid) {
            for (uint64_t i = tid; i < K; i += nthreads) {
                uint64_t row = i;
                for (uint64_t j = 0; j <= len[i]; ++j) {
                    unsigned char c = idx.bwt_char(row);
                    unsigned char want = (j < len[i]) ? (unsigned char)flat[fend[i]-1-j] : 0x0A;
                    if (c != want) {
                        fprintf(stderr, "CHECK2 string %llu step %llu: emitted %d want %d\n",
                                (unsigned long long)i, (unsigned long long)j, (int)c, (int)want);
                        bad.fetch_add(1); break;
                    }
                    if (j < len[i]) row = idx.lf(row);
                }
                done.fetch_add(1);
            }
        };
        for (uint64_t t = 0; t < nthreads; ++t) th.emplace_back(worker, t);
        for (auto& t : th) t.join();
        if (bad.load() == 0)
            std::cerr << "CHECK2 inversion: GREEN (" << K << "/" << K << " strings)\n";
        else ++fails;
    }

    if (n > 40ull*1000*1000) {
        std::cerr << "n > 40M: skipping brute-SA checks (3)-(5)\n";
        if (fails) { std::cerr << "GATE RED\n"; return 1; }
        std::cerr << "GATE GREEN (structural + inversion)\n";
        return 0;
    }

    // ---- brute SA of the equivalent flat text (sentinels 1..k) ----
    std::vector<uint64_t> mp(n);
    {
        uint64_t seq = 0;
        for (uint64_t i = 0; i < n; ++i)
            mp[i] = (flat[i] == '\n') ? (++seq) : (unsigned char)flat[i];
    }
    std::vector<uint64_t> sa(n);
    {
        for (uint64_t i = 0; i < n; ++i) sa[i] = i;
        std::sort(sa.begin(), sa.end(), [&](uint64_t a, uint64_t b) {
            while (a < n && b < n) { if (mp[a] != mp[b]) return mp[a] < mp[b]; ++a; ++b; }
            return a == n;   // shorter (hit end) first
        });
    }
    std::vector<uint64_t> isa(n);
    for (uint64_t i = 0; i < n; ++i) isa[sa[i]] = i;

    // ---- (3) LF law + (5) window emission, bounded to the owning string ----
    // LAW (recorded 2026-09-23, ft30): the rlbwt remaps all k sentinels to
    // 0x0A, so an LF step *from* a string-start row (BWT byte = sentinel)
    // permutes sentinel rows and does NOT satisfy sa[lf] = sa-1. Within a
    // string the LF law is exact. Patterns never contain 0x0A, so backward
    // search never steps from a sentinel row; text extraction must bound
    // windows to the owning string (the chi-verify contract guarantees
    // this: a seed match lies inside one contig).
    {
        bool ok3 = true, ok5 = true;
        uint64_t nchk = 0, nwin = 0;
        for (uint64_t r = 0; r < idx.R; ++r) {
            uint64_t e = idx.run_start(r) + idx.run_len[r] - 1;   // run END row
            uint64_t s = sa[e];
            uint64_t i = (uint64_t)(std::upper_bound(fstart.begin(), fstart.end(), s) - fstart.begin()) - 1;
            if (s > fend[i]) { std::cerr << "CHECK3 run " << r << ": pos " << s << " not in string " << i << "\n"; ok3 = false; break; }
            uint64_t w = std::min(W, s - fstart[i]);   // valid lf steps within string i
            {
                uint64_t row = e;
                for (uint64_t j = 0; j < w; ++j) {   // never lf FROM the sentinel-byte row
                    row = idx.lf(row);
                    ++nchk;
                    if (row >= n) { std::cerr << "CHECK3 run " << r << " step " << j << ": lf out of range\n"; ok3 = false; break; }
                    if (sa[row] != s - j - 1) {
                        std::cerr << "CHECK3 run " << r << " step " << (j+1)
                                  << ": sa[lf]=" << sa[row] << " want " << (s - j - 1) << "\n";
                        ok3 = false; break;
                    }
                }
            }
            {
                uint64_t row2 = e;
                for (uint64_t j = 0; j <= w; ++j) {   // chars of string i + the endmarker byte
                    unsigned char c = idx.bwt_char(row2);
                    uint64_t p = s - 1 - j;         // >= fstart[i]-1 by bound
                    unsigned char want = (unsigned char)mp[p];
                    if (flat[p] == '\n') want = 0x0A;
                    if (c != want) {
                        std::cerr << "CHECK5 run " << r << " window j=" << j
                                  << ": emitted " << (int)c << " want " << (int)want << "\n";
                        ok5 = false; break;
                    }
                    if (j < w) row2 = idx.lf(row2);
                    ++nwin;
                }
            }
            if (!ok3 || !ok5) break;
        }
        if (ok3) std::cerr << "CHECK3 LF-decrement law (within string): GREEN (" << nchk << " steps over " << idx.R << " run ends)\n";
        else ++fails;
        if (ok5) std::cerr << "CHECK5 window emission (within string): GREEN (" << nwin << " chars over " << idx.R << " anchors)\n";
        else ++fails;
    }

    // ---- (4) v4 sample formula at every run end ----
    {
        bool ok = true;
        for (uint64_t r = 0; r < idx.R; ++r) {
            uint64_t e = idx.run_start(r) + idx.run_len[r] - 1;
            uint64_t s = sa[e];
            // owning string: the one containing position s (fstart <= s <= fend)
            uint64_t i = (uint64_t)(std::upper_bound(fstart.begin(), fstart.end(), s) - fstart.begin()) - 1;
            if (s > fend[i]) { std::cerr << "CHECK4 run " << r << ": pos " << s << " not in string " << i << "\n"; ok = false; break; }
            uint64_t want = fstart[i] + (fend[i] - s);   // fsFwd == fstart (plain chain)
            if (idx.sa_sample[r] != want) {
                std::cerr << "CHECK4 run " << r << ": S=" << idx.sa_sample[r]
                          << " want " << want << " (string " << i << ", sa_e=" << s << ")\n";
                ok = false; break;
            }
        }
        if (ok) std::cerr << "CHECK4 v4 sample formula: GREEN (" << idx.R << "/" << idx.R << " runs)\n";
        else ++fails;
    }

    if (fails) { std::cerr << "GATE RED\n"; return 1; }
    std::cerr << "GATE GREEN: all checks passed\n";
    return 0;
}
