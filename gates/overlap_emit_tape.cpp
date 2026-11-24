// Stage only: physically ordered dictionary tape + exact descriptor/row mass per
// two-byte cell. Inputs are private copies; only the private refs copy is retired.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
using namespace std;namespace fs=std::filesystem;using U=uint32_t;using Q=uint64_t;
struct Rec{U p,h,id,freq;};struct Word{U id,h;};static_assert(sizeof(Rec)==16&&sizeof(Word)==8);
constexpr U A=258;struct Cell{Q desc=0,rows=0;};U get(istream&f){U x;assert(f.read((char*)&x,4));return x;}Q reads(){ifstream f("/proc/self/io");string k;Q v;while(f>>k>>v)if(k=="read_bytes:")return v;abort();}
int main(int ac,char**av){bool compact=ac==6&&string(av[5])=="refs";assert(ac==5||compact);fs::path corpus=av[1],refs=av[2],loc=av[3],d=av[4];assert(fs::create_directory(d));Q n=fs::file_size(corpus),rb=fs::file_size(refs);assert(n<=1010000000&&n>0&&rb%8==0);U k=rb/8;assert(fs::file_size(loc)==4ull*(k+1));ifstream lf(loc,ios::binary);lf.seekg(4ull*k);U m=get(lf);lf.seekg(0);assert(get(lf)==0); // upstream parse/inverse/dict-adj retired; retain aligned 4m, occurrence 9m, loc.
 Q live=13ull*m+fs::file_size(loc)+rb,peak=live,tape_size=0;auto add=[&](Q v){live+=v;assert(live<=2*n);peak=max(live,peak);};auto del=[&](fs::path p){live-=fs::file_size(p);assert(fs::remove(p));};auto guard=[&]{assert(reads()<=5*n);};
 const U stride=1<<22,nb=(n+stride-1)/stride;auto path=[&](U b){return d/("range"+to_string(b));};vector<U>count(nb);{ifstream rf(refs,ios::binary);vector<ofstream>out(nb);for(U b=0;b<nb;b++)out[b].open(path(b),ios::binary);U off=0;for(U id=0;id<k;id++){U end=get(lf);assert(off<end&&end<=m);Rec r{get(rf),get(rf),id,end-off};off=end;assert(r.p<n);U b=r.p/stride;assert(++count[b]<=2000000);add(16);out[b].write((char*)&r,16);assert(out[b]);}assert(off==m&&rf.peek()==EOF);for(auto&f:out){f.close();assert(f);}}del(refs);
 int fd=open(corpus.c_str(),O_RDONLY);assert(fd>=0);auto t=(const unsigned char*)mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);assert(t!=MAP_FAILED);madvise((void*)t,n,MADV_SEQUENTIAL);vector<Cell>cells(A*A);auto tp=d/(compact?"physical.refs":"words.tape");ofstream tape(tp,ios::binary);Q desc=0,rows=0;U prev=0,seen=0;for(U b=0;b<nb;b++){guard();vector<Rec>v(count[b]);ifstream in(path(b),ios::binary);if(count[b])assert(in.read((char*)v.data(),16ull*v.size()));in.close();del(path(b));sort(v.begin(),v.end(),[](Rec x,Rec y){return x.p<y.p;});for(auto r:v){U len=r.h&0x7fffffffu;bool eof=r.h>>31;assert(len>0&&r.p+Q(len)<=n&&(eof?r.p+len==n:len>8)&&(!seen||r.p>prev));prev=r.p;++seen;Word w{r.id,r.h};Q stored=compact?sizeof(Rec):8ull+len;add(stored);tape_size+=stored;if(compact)tape.write((char*)&r,sizeof r);else tape.write((char*)&w,8).write((char*)t+r.p,len);assert(tape);U width=len-(eof?0:8);desc+=width;rows+=Q(width)*r.freq;for(U o=0;o<width;o++){U i=r.p+o,c=(2+t[i])*A+(o+1<len?2+t[i+1]:1);cells[c].desc++;cells[c].rows+=r.freq;}}}tape.close();assert(tape&&seen==k&&rows==n&&fs::file_size(tp)==tape_size);
 // Independent check for every cell: all non-END suffix rows are owned once,
 // so weighted cell counts must equal the corpus adjacent-byte histogram.
 vector<Q>truth(A*A);for(U i=0;i<n;i++){U c=(2+t[i])*A+(i+1<n?2+t[i+1]:1);truth[c]++;}Q largest=0;for(U c=0;c<A*A;c++){assert(truth[c]==cells[c].rows);largest=max(largest,cells[c].desc);}ofstream out(d/"cells.bin",ios::binary);add(cells.size()*sizeof(Cell));out.write((char*)cells.data(),cells.size()*sizeof(Cell));out.close();assert(out);munmap((void*)t,n);close(fd);guard();printf("TAPE n=%llu distinct=%u descriptors=%llu weighted_rows=%llu max_cell=%llu tape_bytes=%llu peak_counted=%llu final_columns=%llu reads=%llu all_cell_masses_verified=1\n",(unsigned long long)n,k,(unsigned long long)desc,(unsigned long long)rows,(unsigned long long)largest,(unsigned long long)tape_size,(unsigned long long)peak,(unsigned long long)live,(unsigned long long)reads());}
