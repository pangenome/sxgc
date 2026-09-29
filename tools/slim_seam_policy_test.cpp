// Exactness and refusal gates for seam verification policy, no text expansion.
#include "../bit6/sxi_format.hpp"
#include <chrono>
#include <atomic>
#include <memory>
#include <thread>
#include <cassert>
static constexpr uint64_t INF=UINT64_MAX;
static double tnow(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double G_T0=tnow();
#define SLIM_LCE_CORE_ONLY
#include "../bit6/slim_lce.hpp"
int main(int argc,char** argv) {
    std::string mode=argc>1?argv[1]:"pass";
    std::vector<uint32_t> seq(200002,7);seq[100000]=8;seq.back()=9;
    SlimFingerprint<std::vector<uint32_t>> fp(seq,16,mode=="collision");
    SlimSeamWork work(seq.size());fp.seam_policy(work,1000,"test-parse");
    if(mode=="per-query")fp.verificationLimit=1000;
    if(mode=="total")work.limit=100;
    if(mode=="probe")fp.probeLimit=1;
    if(mode=="quick-total")work.limit=0;
    auto value=fp.lce(0,100001);
    if(mode!="pass")return 99;
    assert(value==100000);
    assert(fp.verificationWork.load()==100017);
    // One charged unit per hash probe, not per reconstructed symbol: probe
    // reads are a bounded structure constant (tau), reported separately.
    assert(fp.suffixReads.load()>0 && fp.hashProbes.load()>0
           && fp.suffixReads.load()>fp.hashProbes.load());
    assert(work.used.load()==fp.verificationWork.load()+fp.hashProbes.load());
    fp.report("test-parse");
    // Atomic reservation cannot overspend the shared parse/dictionary budget.
    SlimSeamWork shared(8);shared.limit=10003;
    std::vector<std::thread> threads;
    for(int i=0;i<8;++i)threads.emplace_back([&]{while(shared.reserve(1)) {}});
    for(auto& t:threads)t.join();
    assert(shared.used.load()==10003 && !shared.reserve(1));
    fprintf(stderr,"SLIM_SEAM_POLICY_TEST PASS exact_lce=100000 shared_limit=10003\n");
}
