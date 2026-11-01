#!/usr/bin/env python3
"""Compile the actual SweepImpl and exercise the rank-concat obstruction.

No copied implementation: the standalone fixture extracts the production class.
--require-concat is the intentionally failing proposed-contract gate.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
source = (ROOT / 'bit6/xsa_finish.cpp').read_text()
sweep = source[source.index('template<class Sink = std::function<void(uint64_t)>>\nstruct SweepImpl'):source.index('// A cold terminal state')]
preamble = r'''
#include <bits/stdc++.h>
#include <immintrin.h>
using namespace std;
constexpr uint64_t INF = ~uint64_t(0);
void pf_check(bool ok, const string& message) { if (!ok) throw runtime_error(message); }
struct NRun { uint8_t c; uint64_t len,h,t,rk=0,ht=~uint64_t(0),tt=~uint64_t(0); };
'''
fixture = r'''
static bool ascending(const vector<uint64_t>& out) {
    return adjacent_find(out.begin(), out.end(), greater_equal<uint64_t>()) == out.end();
}
static unsigned held(const Sweep& s) {
    return count(s.rr_act + 1, s.rr_act + 256, true);
}
int test(int argc) {
    constexpr uint64_t n = 1000000;
    auto run = [](uint8_t c, uint64_t rank) {
        const uint64_t pos = rank ? n - rank : 0;
        return NRun{c,1,pos,pos};
    };
    vector<uint64_t> original;
    Sweep s(n, [&](uint64_t x) { original.push_back(x); });
    s.advance(run(1,0),0,INF);
    s.advance(run(2,1),1,INF);
    pf_check(original.empty() && held(s) == 2, "band 0 fixture changed");
    s.advance(run(3,2),2,INF);
    s.advance(run(4,3),1,INF);
    pf_check(original == vector<uint64_t>({1,2}), "band 1 fixture changed");
    pf_check(s.rr_act[1] && s.rr_pos[1] == 0, "rank zero must remain held");
    s.finish();
    pf_check(original == vector<uint64_t>({1,2,0,3}), "original reproduction changed");
    cout << "CHI_DEFERRAL_REPRO ranks=1,2,0,3 monotone=0\n";
    if (argc > 1) {
        pf_check(ascending(original), "concat contract: emitted ranks are not strictly ascending");
    }

    // Same prefix, two legal state-machine continuations. In one, rank zero
    // survives. In the other, raising context 1 replaces it WITHOUT emission.
    // Therefore flushing it early does not preserve the witness set.
    for (unsigned cycles : {1u,128u,4096u}) {
        vector<uint64_t> determined;
        Sweep prefix(n, [&](uint64_t x) { determined.push_back(x); });
        prefix.advance(run(1,0),0,INF);
        prefix.advance(run(2,1),1,INF);
        unsigned peak = held(prefix);
        for (unsigned j = 0; j < cycles; ++j) {
            const size_t band_begin = determined.size();
            prefix.advance(run(3,2+2*j),2,INF);
            prefix.advance(run(4,3+2*j),1,INF);
            // Give the proposal its ascending per-band emission for free.
            sort(determined.begin()+band_begin, determined.end());
            peak = max(peak, held(prefix));
            pf_check(held(prefix) <= 255, "context bound at band close");
            pf_check(prefix.rr_act[1] && prefix.rr_pos[1] == 0, "low rank lost");
        }
        pf_check(determined.size() == 2*cycles, "determined count");
        pf_check(ascending(determined), "determined ranks must already be sorted");
        vector<uint64_t> keep = determined, drop = determined;
        Sweep a = prefix, b = prefix;
        a.emitWitness = [&](uint64_t x) { keep.push_back(x); };
        b.emitWitness = [&](uint64_t x) { drop.push_back(x); };
        a.finish();
        b.advance(run(1,4+2*cycles),2,INF);
        b.finish();
        pf_check(count(keep.begin(),keep.end(),0) == 1, "EOF must preserve zero");
        pf_check(count(drop.begin(),drop.end(),0) == 0, "higher context must replace zero");
        cout << "CHI_DEFERRAL_LOWER_BOUND bands=" << cycles
             << " unresolved_boundary_peak=" << peak
             << " context_bound=255 determined_blocked=" << determined.size()
             << " keep_zero=1 drop_zero=0\n";
    }

    // A real suffix-array fixture, not just arbitrary run/LCP inputs.
    const string text("banana\0",7);
    vector<uint64_t> sa(text.size()), isa(text.size());
    iota(sa.begin(),sa.end(),0);
    sort(sa.begin(),sa.end(),[&](auto a,auto b){return text.substr(a)<text.substr(b);});
    for (size_t i=0;i<sa.size();++i) isa[sa[i]]=i;
    vector<NRun> runs;
    for (auto p : sa) {
        uint8_t c = uint8_t(text[(p+text.size()-1)%text.size()]);
        if (!runs.empty() && runs.back().c == c) { ++runs.back().len; runs.back().t=p; }
        else runs.push_back({c,1,p,p});
    }
    auto lcp = [&](uint64_t a,uint64_t b) {
        uint64_t k=0;
        while(a+k<text.size() && b+k<text.size() && text[a+k]==text[b+k]) ++k;
        return k;
    };
    vector<uint64_t> real;
    Sweep banana(text.size(),[&](uint64_t x){real.push_back(isa[x ? text.size()-x : 0]);});
    for(size_t i=0;i<runs.size();++i)
        banana.advance(runs[i],i ? lcp(runs[i-1].t,runs[i].h) : 0,
                       runs[i].len==1 ? INF : lcp(runs[i].h,runs[i].t));
    banana.finish();
    pf_check(real == vector<uint64_t>({3,2,0}), "banana rank fixture changed");
    cout << "CHI_DEFERRAL_REAL_SUFFIX_FIXTURE text=banana+NUL ranks=3,2,0 monotone=0\n";
    cout << "CHI_DEFERRAL_OBSTRUCTION_PASS\n";
    return 0;
}
int main(int argc, char**) {
    try { return test(argc); }
    catch (const exception& e) { cerr << e.what() << "\n"; return 2; }
}
'''
with tempfile.TemporaryDirectory(prefix='chi-deferral-') as tmp:
    cpp = pathlib.Path(tmp) / 'test.cpp'
    binary = pathlib.Path(tmp) / 'test'
    cpp.write_text(preamble + sweep + fixture)
    subprocess.run(['c++', '-O2', '-std=c++17', str(cpp), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)] + sys.argv[1:])
    sys.exit(0 if result.returncode == 0 else 1)
