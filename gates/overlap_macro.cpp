// Select maximal consecutive sets of complete windows whose spill files fit
// the remaining scratch. Exercise ONE set: physical phrase pass -> window files.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
using namespace std;namespace fs=std::filesystem;using U=uint32_t;using Q=uint64_t;using Key=pair<string_view,bool>;
struct Mass{Q desc,bytes,rows;};struct Rec{U pos,span,id,freq;};struct Row{U id,off,span,pred;};constexpr U A=258;
Q reads(){ifstream f("/proc/self/io");string k;Q v;while(f>>k>>v)if(k=="read_bytes:")return v;abort();}
U cell(Key k){return (2+(unsigned char)k.first[0])*A+(k.first.size()>1?2+(unsigned char)k.first[1]:U(k.second));}
int main(int ac,char**av){assert(ac==8);fs::path source=av[1],refs=av[2],plan=av[3],loc=av[4],dir=av[5];U m=stoul(av[6]),which=stoul(av[7]);assert(fs::create_directory(dir));Q n=fs::file_size(source),k=fs::file_size(refs)/16;assert(n<=1010000000&&fs::file_size(loc)==4*(k+1));Q live=16*k+fs::file_size(plan)+fs::file_size(loc)+13ull*m+16*A*A,peak=live;assert(live<2*n);Q avail=2*n-live;ifstream f(plan,ios::binary);U count;assert(f.read((char*)&count,4)&&count&&count<65536);vector<Mass>v(count);vector<pair<string,bool>>piv;vector<U> ends;Q sum=0,volume=0;for(U i=0;i<count;i++){U tag;assert(f.read((char*)&v[i],sizeof(Mass))&&f.read((char*)&tag,4));if(i+1<count){string s(tag&0x7fffffff,'\0');assert(s.size()&&f.read(s.data(),s.size()));piv.emplace_back(move(s),tag>>31);}Q size=16*v[i].desc+v[i].bytes;assert(size<=avail && v[i].desc<=1100000 && v[i].rows<=2000000 && v[i].bytes<=64u<<20);if(sum+size>avail){ends.push_back(i);sum=0;}sum+=size;volume+=size;}ends.push_back(count);assert(which<ends.size());U lo=which?ends[which-1]:0,hi=ends[which];auto key=[&](U i)->Key{return {piv[i].first,piv[i].second};};vector<U>bound(A*A+1);U p=0;for(U c=0;c<=A*A;c++){while(p<piv.size()&&cell(key(p))<c)p++;bound[c]=p;}
 int fd=open(source.c_str(),O_RDONLY);assert(fd>=0);auto t=(const char*)mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);assert(t!=MAP_FAILED);close(fd);madvise((void*)t,n,MADV_SEQUENTIAL);vector<ofstream>out(hi-lo);vector<Mass>got(hi-lo);for(U j=lo;j<hi;j++)out[j-lo].open(dir/to_string(j),ios::binary);ifstream rf(refs,ios::binary);Rec w;Q spill=0;while(rf.read((char*)&w,16)){U len=w.span&0x7fffffff;bool eof=w.span>>31;assert(w.pos+Q(len)<=n);for(U o=0;o<len-(eof?0:8);o++){Key a={{t+w.pos+o,len-o},eof};U c=cell(a);if(bound[c+1]<lo||bound[c]>=hi)continue;U l=bound[c],r=bound[c+1];while(l<r){U mid=l+(r-l)/2;if(key(mid)<a)l=mid+1;else r=mid;}if(l<lo||l>=hi)continue;auto&g=got[l-lo];g.desc++;g.bytes+=a.first.size();g.rows+=w.freq;Row row{w.id,o,(len-o)|(U(eof)<<31),o?U((unsigned char)t[w.pos+o-1]):0};spill+=16+a.first.size();assert(live+spill<=2*n);out[l-lo].write((char*)&row,16).write(a.first.data(),a.first.size());assert(out[l-lo]);}if(!(w.id%32768))assert(reads()<=3*n);}for(U j=lo;j<hi;j++){auto&g=got[j-lo];assert(g.desc==v[j].desc&&g.bytes==v[j].bytes&&g.rows==v[j].rows);out[j-lo].close();assert(out[j-lo]&&fs::file_size(dir/to_string(j))==g.bytes+16*g.desc);}peak+=spill;assert(reads()<=3*n);cout<<"MACRO sets="<<ends.size()<<" total_windows="<<count<<" first="<<lo<<" limit="<<hi<<" spill="<<spill<<" peak_counted="<<peak<<" reads="<<reads()<<" projected_all_spill="<<volume<<" projected_source_plus_refs_scan="<<ends.size()*(n+16*k)<<'\n';}
