// pfp_index_build.cpp — persist the PFP query-index structures ONCE at
// build time, so that chi_rspace_dump can LOAD them instead of rebuilding
// them in-process.
//
// Why: chi_rspace_dump already computes the per-run .agg from PFP
// artifacts + .ri4 only (no lcp_index, no TeraLCP).  But at dump time it
// still *constructed*, in process:
//   - pfpds::dictionary   : gsacak SA of D, isaD/daD/lcpD, RMQ, colex
//   - pfpds::parse        : sacak SA of P, isaP/lcpP, RMQ
//   - pf_parsing          : O(n)-bit b_p, O(n)-bit b_bwt, |M| ~ 0.1n, W-WT
// The first two are suffix-sort-class work; the third is O(n)-scaled
// allocation + fill.  None of it depends on the .ri4 — it is a function of
// the parse alone — so it can be computed once and loaded thereafter.
//
// This tool builds exactly what chi_rspace_dump would build (same vendored
// pfp_ds, same flags) and writes the subset of members the dumper's
// queries actually touch:
//
//   pf_parsing : n, w, W_flag, b_bwt (+rank/select), b_p (+rank/select),
//                M, w_wt
//   parse      : saP, isaP, lcpP (+ rmq_lcp_P rebuilt on load); p re-read
//                from the .parse artifact (identical bytes)
//   dictionary : isaD, lcpD (+ rmq_lcp_D rebuilt on load); b_d rebuilt by
//                the cheap all-flags-false constructor (O(|D|) scan, no SA)
//
// IMPORTANT: rank/select/RMQ support structures are NOT serialized — they
// are rebuilt on load from the loaded bit/int-vectors (sdsl's own
// serialize/load of the data is what must be lossless; the supports are
// deterministic functions of the data).  The gate is byte-identity of the
// resulting .agg, which is sensitive to every value the queries read.
//
// File layout (little-endian host, sdsl self-delimiting blocks):
//   magic "XPF1" u32, version u32
//   W u64, pf_n u64, m_count u64, d_size u64, parse_p_size u64, wt_size u64
//   sdsl block: b_bwt (bit_vector)
//   sdsl block: b_p   (bit_vector)
//   sdsl block: w_wt  (pfp_wt_custom)
//   sdsl block: saP, isaP, lcpP (int_vector<0>)
//   sdsl block: isaD, lcpD (int_vector<0>)
//   raw: M: m_count x (u32 len, u32 left, u32 right)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <chrono>

#include <sdsl/int_vector.hpp>
#include "pfp/utils.hpp"
typedef pfpds::long_type long_type;
#include "pfp/pfp.hpp"
#include "pfp/sa_support.hpp"
#include "pfp/lce_support.hpp"
extern "C" {
#include "gsacak.h"
}

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static const uint32_t IDX_MAGIC = 0x31465058; // "XPF1"
static const uint32_t IDX_VERSION = 2;

int main(int argc, char** argv) {
    std::string parsePrefix, outPath;
    long_type W = 10;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--parse") && i + 1 < argc) parsePrefix = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outPath = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) W = strtoll(argv[++i], nullptr, 10);
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 1; }
    }
    if (parsePrefix.empty() || outPath.empty()) {
        fprintf(stderr, "usage: pfp_index_build --parse PFP_PREFIX -o INDEX [-w W]\n");
        return 1;
    }

    double t0 = now_s();
    std::less<uint8_t> u8comp;
    fprintf(stderr, "[%7.2fs] building dictionary (flags: b_d only)...\n", now_s() - t0);
    pfpds::dictionary<uint8_t> D(parsePrefix, W, u8comp, false, false, false, false, false, false, false);
    fprintf(stderr, "[%7.2fs] dict: phrases=%llu size=%llu\n", now_s() - t0,
            (unsigned long long)D.n_phrases(), (unsigned long long)D.d.size());

    fprintf(stderr, "[%7.2fs] reading parse p...\n", now_s() - t0);
    pfpds::parse PP(parsePrefix, D.n_phrases() + 1, false, false, false, false);
    fprintf(stderr, "[%7.2fs] parse: p=%llu\n", now_s() - t0,
            (unsigned long long)PP.p.size());

    // The full builds (what the dumper currently does in-process).
    fprintf(stderr, "[%7.2fs] building parse SA/ISA/LCP/RMQ...\n", now_s() - t0);
    {
        double ta = now_s();
        PP.build(true, true, true, true);
        fprintf(stderr, "[%7.2fs]   parse build: %.2fs\n", now_s() - t0, now_s() - ta);
    }
    fprintf(stderr, "[%7.2fs] building dictionary SA/ISA/DA/LCP/RMQ/colex...\n", now_s() - t0);
    {
        double ta = now_s();
        D.build(true, true, true, true, true, true, true);
        fprintf(stderr, "[%7.2fs]   dict build: %.2fs\n", now_s() - t0, now_s() - ta);
    }
    fprintf(stderr, "[%7.2fs] building pf_parsing (b_p, b_bwt, M, W-WT)...\n", now_s() - t0);
    {
        double ta = now_s();
        pfpds::pf_parsing<uint8_t> PF(D, PP, true, false);
        fprintf(stderr, "[%7.2fs]   pf_parsing build: %.2fs\n", now_s() - t0, now_s() - ta);

        std::ofstream out(outPath, std::ios::binary);
        if (!out) { fprintf(stderr, "cannot open %s\n", outPath.c_str()); return 1; }

        uint32_t magic = IDX_MAGIC, ver = IDX_VERSION;
        uint64_t pf_n = (uint64_t)PF.n, m_count = (uint64_t)PF.M.size();
        uint64_t d_size = (uint64_t)D.d.size(), p_size = (uint64_t)PP.p.size();
        uint64_t wt_size = (uint64_t)PF.w_wt.size();
        uint64_t w64 = (uint64_t)W;
        out.write((const char*)&magic, 4);
        out.write((const char*)&ver, 4);
        out.write((const char*)&w64, 8);
        out.write((const char*)&pf_n, 8);
        out.write((const char*)&m_count, 8);
        out.write((const char*)&d_size, 8);
        out.write((const char*)&p_size, 8);
        out.write((const char*)&wt_size, 8);

        struct Sz { const char* name; long_type bytes; };
        std::vector<Sz> sizes;
        auto block_start = [&]() -> long_type { return (long_type)out.tellp(); };
        auto block_end = [&](const char* name, long_type s) {
            sizes.push_back({name, (long_type)out.tellp() - s});
        };

        long_type s;
        s = block_start(); sdsl::serialize(PF.b_bwt, out); block_end("b_bwt", s);
        s = block_start(); sdsl::serialize(PF.b_p, out); block_end("b_p", s);
        s = block_start(); PF.w_wt.serialize(out); block_end("w_wt", s);
        s = block_start(); sdsl::serialize(PP.saP, out); block_end("saP", s);
        s = block_start(); sdsl::serialize(PP.isaP, out); block_end("isaP", s);
        s = block_start(); sdsl::serialize(PP.lcpP, out); block_end("lcpP", s);
        s = block_start(); sdsl::serialize(D.b_d, out); block_end("b_d", s);
        s = block_start(); sdsl::serialize(D.isaD, out); block_end("isaD", s);
        s = block_start(); sdsl::serialize(D.lcpD, out); block_end("lcpD", s);

        // M: compact raw (u32 len, u32 left, u32 right).  Fail loudly if a
        // value does not fit — a silent truncation would corrupt SA_sup.
        s = block_start();
        {
            const uint64_t U32MAX = 0xFFFFFFFFULL;
            std::vector<uint32_t> buf(3 * (size_t)m_count);
            for (uint64_t i = 0; i < m_count; ++i) {
                const auto& m = PF.M[i];
                if ((uint64_t)m.len > U32MAX || (uint64_t)m.left > U32MAX || (uint64_t)m.right > U32MAX) {
                    fprintf(stderr, "FATAL: M[%llu] = {len=%lld,left=%lld,right=%lld} exceeds u32\n",
                            (unsigned long long)i, (long long)m.len, (long long)m.left, (long long)m.right);
                    return 1;
                }
                buf[3 * i + 0] = (uint32_t)m.len;
                buf[3 * i + 1] = (uint32_t)m.left;
                buf[3 * i + 2] = (uint32_t)m.right;
            }
            out.write((const char*)buf.data(), (std::streamsize)(4 * buf.size()));
        }
        block_end("M", s);
        out.flush();
        long_type total = (long_type)out.tellp();
        out.close();
        fprintf(stderr, "[%7.2fs] wrote %s (%.3f GB)\n", now_s() - t0, outPath.c_str(),
                (double)total / (1024.0 * 1024.0 * 1024.0));
        for (const auto& z : sizes) {
            fprintf(stderr, "    %-6s %14.3f MB  (%6.2f%%)\n", z.name,
                    (double)z.bytes / (1024.0 * 1024.0), 100.0 * (double)z.bytes / (double)total);
        }
        fprintf(stderr, "[%7.2fs] INDEX BUILD DONE: n=%llu |M|=%llu |D|=%llu |P|=%llu |wt|=%llu W=%llu\n",
                now_s() - t0, (unsigned long long)pf_n, (unsigned long long)m_count,
                (unsigned long long)d_size, (unsigned long long)p_size,
                (unsigned long long)wt_size, (unsigned long long)W);
    }
    return 0;
}
