// Trace exact phrase-tail comparator page references on independent contiguous samples.
// Models SOURCE pages only, not OS readahead, allocator, or pipeline IO. C++17.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <string_view>
#include <vector>
using namespace std;
struct Cache { vector<int> prev,next; int first=-1,last=-1,count=0,cap; uint64_t miss=0;
 Cache(int n,int c):prev(n,-2),next(n,-1),cap(c){}
 void touch(int p){if(cap==0||first==p)return; if(prev[p]==-2){++miss;if(count==cap){int t=last;last=prev[t];if(last>=0)next[last]=-1;else first=-1;prev[t]=-2;}else ++count;}else{int a=prev[p],b=next[p];next[a]=b;if(b<0)last=a;else prev[b]=a;}next[p]=first;if(first>=0)prev[first]=p;else last=p;first=p;prev[p]=-1;}
};
struct Tail{int p,end;bool eof;};
int main(int argc,char**argv){if(argc<4||argc>6)return 2;const bool budgeted=argc==6;if(budgeted&&string_view(argv[5])!="fixed")return 2;const int prefix=argc>=5?stoi(argv[4]):0;if(prefix<0||prefix>64)return 2;ifstream f(argv[1],ios::binary);int n=stoi(argv[2]);if(n<4096||n>4000000)return 2;f.seekg(stoll(argv[3]));vector<unsigned char> t(n);if(!f.read((char*)t.data(),n))return 2;
 vector<int>s{0};for(int i=1;i+8<=n;++i){uint64_t h=1469598103934665603ULL;for(int j=i;j<i+8;++j)h=(h^t[j])*1099511628211ULL;if(h%31==0)s.push_back(i);}s.push_back(n);
 map<pair<string_view,bool>,int> words;for(size_t i=0;i+1<s.size();++i){bool eof=i+2==s.size();words.emplace(make_pair(string_view((char*)&t[s[i]],s[i+1]-s[i]+(eof?0:8)),eof),s[i]);}
 vector<Tail> v;for(auto [key,p]:words){int end=p+key.first.size();for(int j=p;j<end-(key.second?0:8);++j)v.push_back({j,end,key.second});}sort(v.begin(),v.end(),[](auto a,auto b){return tie(a.p,a.end)<tie(b.p,b.end);}); // physical regeneration
 auto cell=[&](Tail a){return t[a.p]*258+(a.p+1<a.end?t[a.p+1]+2:int(a.eof));};vector<vector<Tail>> cells(256*258);for(auto a:v)cells[cell(a)].push_back(a);
 int pages=(n+4095)/4096;vector<Cache> caches;const int reserved=((n+23)/24)*prefix;for(int pct:{25,50,75,90,100}){int slots=max(1,pages*pct/100)-(budgeted?(reserved+4095)/4096:0);caches.emplace_back(pages,max(1,slots));if(slots<1)caches.back().cap=0;}uint64_t comparisons=0,stage_bytes=0,maxbytes=0;int peak_pairs=0;uint64_t slow=0;int bins=0,cap=(n+23)/24,oversize=0;
 for(size_t lo=0;lo<cells.size();){size_t hi=lo+1,m=cells[lo].size();while(hi<cells.size()&&m+cells[hi].size()<=size_t(cap)){m+=cells[hi++].size();}if(m){++bins;oversize+=m>size_t(cap);vector<Tail>b;for(size_t k=lo;k<hi;++k)b.insert(b.end(),cells[k].begin(),cells[k].end());sort(b.begin(),b.end(),[](auto a,auto z){return tie(a.p,a.end)<tie(z.p,z.end);});uint64_t arena=0;for(auto a:b)arena+=a.end-a.p;stage_bytes+=arena;maxbytes=max(maxbytes,arena);
 // Trace physical prefix loads. During comparison model these bytes as resident;
 // this is an IO model, not a wall-time benchmark. Charge prefix*m metadata.
 peak_pairs=max(peak_pairs,int(m));for(auto a:b)for(int i=a.p;i<min(a.end,a.p+prefix);++i)for(auto &c:caches)c.touch(i/4096);
 sort(b.begin(),b.end(),[&](Tail a,Tail z){++comparisons;int i=a.p,j=z.p;while(i<a.end&&j<z.end){if(i-a.p>=prefix){slow+=i-a.p==prefix;for(auto &c:caches){c.touch(i/4096);c.touch(j/4096);}}if(t[i]!=t[j])return t[i]<t[j];++i;++j;}return make_pair(a.end-i,a.eof)<make_pair(z.end-j,z.eof);});
 auto key=[&](Tail a){return make_pair(string_view((char*)&t[a.p],a.end-a.p),a.eof);};assert(is_sorted(b.begin(),b.end(),[&](auto a,auto z){return key(a)<key(z);}));}lo=hi;}
 cout<<argv[3]<<','<<n<<','<<words.size()<<','<<v.size()<<','<<bins<<','<<oversize<<','<<comparisons<<','<<stage_bytes<<','<<maxbytes;for(auto &c:caches){cout<<',';if(c.cap)cout<<c.miss;else cout<<"NA";}if(argc>=5)cout<<','<<prefix<<','<<slow<<','<<uint64_t(prefix)*peak_pairs;if(budgeted){assert(oversize==0);cout<<','<<reserved;for(auto &c:caches)cout<<','<<c.cap;}cout<<'\n';}
