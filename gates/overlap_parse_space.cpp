// Diagnostic, NOT a production or BWT merger. One verified text-order lexical
// parse + check-only inverse ranks; compute exact parse BWT runs and rerun the
// overlap census on *integer token boundaries*. No new sort column on disk.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>
using U=uint32_t; using Q=uint64_t;
struct Span { U pos,raw;bool eof; };
std::vector<U> readcol(const char* p) {
    auto bytes=std::filesystem::file_size(p);
    assert(bytes && bytes%4==0 && bytes<=32'000'000*4);
    std::vector<U> a(bytes/4); std::ifstream f(p,std::ios::binary);
    assert(f.read((char*)a.data(),bytes)); return a;
}
int main(int ac,char** av) {
    assert(ac==3); auto t=readcol(av[1]);U m=t.size();assert(m>=8);
    U k=1+*std::max_element(t.begin(),t.end());assert(k<=m);
    std::vector<U> count(k); for(U id:t)++count[id]; assert(std::count(count.begin(),count.end(),0)==0);
    auto singleton=std::count(count.begin(),count.end(),1);
    U r=1;
    {auto inv=readcol(av[2]);assert(inv.size()==m);std::vector<U>bwt(m+1,UINT32_MAX);bwt[0]=t.back();
     for(U i=0;i<m;i++){U j=inv[i];assert(j && j<=m && bwt[j]==UINT32_MAX);bwt[j]=i?t[i-1]:k;}
     for(U i=1;i<=m;i++){r+=(bwt[i]!=bwt[i-1]);} assert(r>=k+1);}
    printf("P1 tokens=%u distinct=%u singleton_symbols=%lld bwt_rows=%u bwt_runs=%u min_runs=%u\n",m,k,(long long)singleton,m+1,r,k+1);
    std::vector<Span>a;U start=0;
    for(U i=1;i+8<=m;i++) {Q h=1469598103934665603ull;
        for(U j=i;j<i+8;j++)h=(h^t[j])*1099511628211ull;
        if(h%31==0){a.push_back({start,i+8-start,false});start=i;} }
    a.push_back({start,m-start,true});
    std::vector<U> order(a.size()),p2(a.size());std::iota(order.begin(),order.end(),0);
    auto less=[&](U i,U j){auto x=a[i],y=a[j];auto z=std::min(x.raw,y.raw);
       auto [p,q]=std::mismatch(t.begin()+x.pos,t.begin()+x.pos+z,t.begin()+y.pos);
       if(p!=t.begin()+x.pos+z)return *p<*q;
       return std::pair(x.raw,x.eof)<std::pair(y.raw,y.eof);};
    std::sort(order.begin(),order.end(),less);U k2=0,maxspan=0;Q d=1,owned=0;
    for(U i=0;i<order.size();i++){U j=order[i];auto x=a[j];if(!i||less(order[i-1],j)){
            ++k2;d+=x.raw+1;owned+=x.raw-(x.eof?0:8);maxspan=std::max(maxspan,x.raw);}
        p2[j]=k2-1;}
    Q expanded=0;for(auto x:a)expanded+=x.raw-(x.eof?0:8);assert(expanded==m);
    // p2 is a physical dense ID string in RAM, not text byte offsets. Distinct
    // counts bound the BWT runs even without sorting P2 suffixes.
    printf("P2 tokens=%zu distinct=%u bytes_u32=%zu min_bwt_runs=%u dict_symbols_with_separators=%llu owned_lower_token_descriptors=%llu max_span_tokens=%u decoded_token_count=%llu\n",p2.size(),k2,p2.size()*sizeof(U),k2+1,(unsigned long long)d,(unsigned long long)owned,maxspan,(unsigned long long)expanded);
}
