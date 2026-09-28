#include "agc-api.h"
#include <iostream>
int main(int argc,char**argv){ CAGCFile f; if(!f.Open(argv[1],false)) return 1; std::vector<std::string>s,c; f.ListSample(s); uint64_t off=0; for(auto&a:s){f.ListCtg(a,c);for(auto&b:c){auto n=f.GetCtgLen(a,b);std::cout<<a<<'\t'<<b<<'\t'<<off<<'\t'<<n<<'\n';off+=n+1;}}std::cerr<<off<<'\n';}
