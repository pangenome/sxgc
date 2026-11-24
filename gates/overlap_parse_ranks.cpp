// External integer suffix ranks for a text-order u32 lexical parse. Rank 0 is
// virtual END. At depth h, sort (R[i],R[i+h]); scatter dense new ranks through
// bounded ordinal ranges. Verify final ranks by the independent one-step law.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
using namespace std;namespace fs=std::filesystem;using U=uint32_t;using Q=uint64_t;
struct Row{U a,b,i;};struct Assign{U i,r;};static_assert(sizeof(Row)==12&&sizeof(Assign)==8);
Q reads(){ifstream f("/proc/self/io");string k;Q v;while(f>>k>>v)if(k=="read_bytes:")return v;abort();}
int main(int ac,char**av){assert(ac==5);fs::path parse=av[1],dir=av[2];Q n=stoull(av[3]),k=stoull(av[4]);assert(n<=1010000000 && n>0 && k<n);Q bytes=fs::file_size(parse);assert(bytes%4==0 && bytes/4<=UINT32_MAX-1);U m=bytes/4;assert(m && k<=m);assert(fs::create_directory(dir));Q live=bytes+8*k,peak=live;auto add=[&](Q v){live+=v;assert(live<=2*n);peak=max(peak,live);};auto del=[&](fs::path p){live-=fs::file_size(p);assert(fs::remove(p));};auto guard=[&](){assert(reads()<=12*n);};auto path=[&](char c,U i){return dir/(string(1,c)+to_string(i));};
 // Range size counts rank values, not bytes. Explicit skew refusal per bucket.
 const U stride=1<<18, shards=(m+stride-1)/stride;U classes=k;fs::path input=parse;bool initial=true,verify=false; Q depth=1;unsigned passes=0;
 for(;;){U ranges=(classes+stride-1)/stride;vector<ofstream> o(ranges);vector<U> cnt(ranges);for(U b=0;b<ranges;b++)o[b].open(path('b',b),ios::binary);ifstream left(input,ios::binary),right(input,ios::binary),symbols;if(verify)symbols.open(parse,ios::binary);Q offset=verify?1:depth;right.seekg(min<Q>(offset,m)*4);for(U i=0;i<m;i++){if(!(i%262144))guard();U a=0,b=0;assert(left.read((char*)&a,4));if(initial)++a;assert(a>0&&a<=classes);if(i+offset<m){assert(right.read((char*)&b,4));if(initial)++b;assert(b>0&&b<=classes);}U bucket=(a-1)/stride;Row row{a,b,i};if(verify){assert(symbols.read((char*)&row.b,4));row.i=b;}assert(++cnt[bucket]<=2000000);add(12);o[bucket].write((char*)&row,12);assert(o[bucket]);}for(auto&f:o){f.close();assert(f);}left.close();right.close();if(!initial&&!verify)del(input);
 vector<ofstream> ord(verify?0:shards);for(U s=0;s<ord.size();s++)ord[s].open(path('a',s),ios::binary);U newclasses=0,seen=0;pair<U,U> prev{};for(U b=0;b<ranges;b++){guard();vector<Row> v(cnt[b]);ifstream f(path('b',b),ios::binary);if(cnt[b])assert(f.read((char*)v.data(),12ull*v.size()));f.close();del(path('b',b));sort(v.begin(),v.end(),[](Row x,Row y){return tie(x.a,x.b,x.i)<tie(y.a,y.b,y.i);});for(auto x:v){auto key=verify?make_pair(x.b,x.i):make_pair(x.a,x.b);if(verify){assert(x.a==seen+1 && (!seen||prev<key));}else{if(!seen||key!=prev)++newclasses;Assign p{x.i,newclasses};add(8);ord[x.i/stride].write((char*)&p,8);assert(ord[x.i/stride]);}prev=key;++seen;}}assert(seen==m);if(verify)break;
 for(auto&f:ord){f.close();assert(f);}input=dir/"inverse.u32";ofstream next(input,ios::binary);for(U s=0;s<shards;s++){guard();U len=min(stride,m-s*stride);vector<U> block(len);ifstream f(path('a',s),ios::binary);for(U j=0;j<len;j++){Assign p;assert(f.read((char*)&p,8)&&p.i/stride==s&&p.i%stride<len&&!block[p.i%stride]);block[p.i%stride]=p.r;}assert(f.peek()==EOF);f.close();del(path('a',s));add(4ull*len);next.write((char*)block.data(),4ull*len);assert(next);}next.close();assert(next);depth*=2;++passes;classes=newclasses;initial=false;printf("PASS depth=%llu classes=%u reads=%llu\n",(unsigned long long)depth,classes,(unsigned long long)reads());fflush(stdout);if(classes==m)verify=true;else assert(depth<m);}
 guard();printf("RANKS rows=%u passes=%u permutation_and_suffix_recurrence=1 peak_counted=%llu final_columns=%llu reads=%llu\n",m,passes,(unsigned long long)peak,(unsigned long long)live,(unsigned long long)reads());}
