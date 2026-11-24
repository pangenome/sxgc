// Build CSR ties from text-order parse and 1-based inverse parse SA.
// One source scan supplies occurrence position + preceding byte. Never pack a
// predecessor into the high bits of a rank or phrase ID (1 GB exceeds 24 bits).
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
using namespace std; namespace fs=std::filesystem; using U=uint32_t;using Q=uint64_t;
struct Row{U id,next,pos; uint8_t pred;};static_assert(sizeof(Row)==16);
Q reads(){ifstream f("/proc/self/io");string k;Q v;while(f>>k>>v)if(k=="read_bytes:")return v;abort();}
U get(istream&f){U v;assert(f.read((char*)&v,4));return v;}
int main(int ac,char**av){assert(ac==6);fs::path corpus=av[1],parse=av[2],inv=av[3],d=av[4];const Q n=fs::file_size(corpus),pb=fs::file_size(parse);const U k=stoul(av[5]);assert(n>0&&n<=1010000000&&pb%4==0&&pb==fs::file_size(inv)&&pb/4<n);const U m=pb/4;assert(k>0&&k<=m);assert(fs::create_directory(d));Q live=2*pb+8ull*k,peak=live;auto add=[&](Q v){live+=v;assert(live<=2*n);peak=max(peak,live);};auto guard=[&](){assert(reads()<=5*n);};int fd=open(corpus.c_str(),O_RDONLY);assert(fd>=0);auto t=(const unsigned char*)mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);assert(t!=MAP_FAILED);madvise((void*)t,n,MADV_SEQUENTIAL);
 const U stride=1<<17,nb=(k+stride-1)/stride;auto path=[&](U b){return d/("bucket"+to_string(b));};vector<ofstream> out(nb);vector<U>counts(nb);for(U b=0;b<nb;b++)out[b].open(path(b),ios::binary);ifstream f(parse,ios::binary),r(inv,ios::binary);assert(get(r)>0);U j=0;auto emit=[&](U p){assert(j<m);U id=get(f),rank=j+1==m?0:get(r);assert(id<k&&rank<=m&&(rank||j+1==m));Row row{id,rank,p,(uint8_t)(p?t[p-1]:0)};U b=id/stride;assert(++counts[b]<=2000000);add(sizeof row);out[b].write((char*)&row,sizeof row);assert(out[b]);++j;};emit(0);for(U i=1;i+8<=n;i++){if(i%1048576==0)guard();Q h=1469598103934665603ull;for(U x=i;x<i+8;x++)h=(h^t[x])*1099511628211ull;if(h%31==0)emit(i);}assert(j==m&&f.peek()==EOF&&r.peek()==EOF);for(auto &s:out){s.close();assert(s);}if(n>101000000){munmap((void*)t,n);t=nullptr;}
 ofstream loc(d/"loc.u32",ios::binary),occ(d/"pos-next.u32x2",ios::binary),pred(d/"pred.u8",ios::binary);U z=0,words=0;for(U b=0;b<nb;b++){guard();vector<Row>a(counts[b]);ifstream in(path(b),ios::binary);if(!a.empty())assert(in.read((char*)a.data(),a.size()*sizeof(Row)));in.close();live-=fs::file_size(path(b));assert(fs::remove(path(b)));sort(a.begin(),a.end(),[](Row x,Row y){return tie(x.id,x.next)<tie(y.id,y.next);});U last=0;for(auto x:a){if(words==x.id){loc.write((char*)&z,4);++words;add(4);}else{assert(words==x.id+1&&last<x.next);}last=x.next;occ.write((char*)&x.pos,4).write((char*)&x.next,4);pred.put((char)x.pred);++z;add(9);}assert(loc&&occ&&pred);}assert(z==m&&words==k);loc.write((char*)&z,4);add(4);loc.close();occ.close();pred.close();assert(loc&&occ&&pred);
 // Independent sandbox verifier: recover j from textual start, check parse ID,
 // expected rank and predecessor for every entry, plus ordinal coverage.
 if(t){vector<U>starts{0},ids(m),ranks(m);for(U i=1;i+8<=n;++i){Q h=1469598103934665603ull;for(auto x=i;x<i+8;++x)h=(h^t[x])*1099511628211ull;if(h%31==0)starts.push_back(i);}assert(starts.size()==m);ifstream fi(parse,ios::binary),fr(inv,ios::binary),fl(d/"loc.u32",ios::binary),fo(d/"pos-next.u32x2",ios::binary),fp(d/"pred.u8",ios::binary);assert(fi.read((char*)ids.data(),pb)&&fr.read((char*)ranks.data(),pb));vector<bool>seen(m);U off=get(fl);assert(off==0);for(U v=0;v<k;v++){U end=get(fl);assert(off<end&&end<=m);while(off<end){U p=get(fo),rank=get(fo);U j=lower_bound(starts.begin(),starts.end(),p)-starts.begin();assert(j<m&&starts[j]==p&&!seen[j]&&ids[j]==v&&rank==(j+1<m?ranks[j+1]:0)&&fp.get()==(p?t[p-1]:0));seen[j]=true;++off;}}assert(off==m&&fo.peek()==EOF&&fp.peek()==EOF&&fl.peek()==EOF);munmap((void*)t,n);}guard();printf("OCC m=%u k=%u sorted_rows=%u sandbox_decode=%d max_bucket_records=%u peak_counted=%llu final_columns=%llu reads=%llu\n",m,k,z,int(n<=101000000),*max_element(counts.begin(),counts.end()),(unsigned long long)peak,(unsigned long long)live,(unsigned long long)reads());}
