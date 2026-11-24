#include "../xsa/pfp/tools/finite_emit_rows.hpp"
#include <cassert>
#include <numeric>
#include <random>
#include <sys/wait.h>
using namespace finiteemit;
int main(){U m=318164488,c=546043;Marks marks(m);std::vector<U> v;std::mt19937_64 rng(927454113);for(U i=0;i<c;++i)v.push_back(1+i*503);v.push_back(m);auto original=v;std::shuffle(v.begin(),v.end(),rng);
 for(unsigned pass=0;pass<2;++pass){for(U x:v)marks.add(x);U i=0;marks.drain([&](U x){assert(i<original.size() && x==original[i++]);});assert(i==original.size());marks.drain([](U){assert(false);});}
 auto frame=row(0x112345678ULL,0x298765432ULL,256);assert(frame.p==0x12345678 && frame.h==0x98765432 && frame.tag==0x80200300);
 pid_t pid=fork();assert(pid>=0);if(!pid){marks.add(64);marks.add(64);_exit(0);}int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==75);
 std::printf("PASS two shuffled 546044-successor drains in a 318164488-rank universe, exact clear/reuse and duplicate refusal, 43-bit LE framing; mark_bytes=%llu\n",(unsigned long long)Marks::bytes(m));}
