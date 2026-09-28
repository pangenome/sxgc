#include "agc-api.h"
#include <iostream>
int main(int argc,char**argv){ CAGCFile f; if(!f.Open(argv[1],false)) return 1; std::vector<std::string> names; std::vector<std::string> samples; f.ListSample(samples); for(auto &s:samples) { f.ListCtg(s,names); for(auto &c:names) f.GetCtgLen(s,c); } std::string seq; if(f.GetCtgSeq(argv[2],argv[3],-1,-1,seq)<0)return 2;std::cerr<<seq.size()<<'\n';std::cout<<seq; }
