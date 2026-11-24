// Exact partition-local census. Scratch files are private, native-endian and ephemeral.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace std;
namespace fs=filesystem;
using U=uint64_t; using Byte=unsigned char;
constexpr U Mi=1<<20, TEN=10000000000ULL, basis=1469598103934665603ULL, prime=1099511628211ULL;
constexpr unsigned buckets=64, fan=8;
void need(bool b,const char* msg){if(!b) throw runtime_error(msg);}
using File=unique_ptr<FILE,int (*)(FILE*)>;
File file(const string& p,const char* mode){File f(fopen(p.c_str(),mode),fclose); need(bool(f),"scratch open");return f;}
struct Segment{unsigned id;unsigned bytes;};
using Run=deque<Segment>;
struct Census{
 U n=0,deadline=0,live=0,peak=0,rd=0,wr=0,ioLimit=0,seg=8*Mi,base=0;
 unsigned files=0,levels=0;bool collide=false;
 U m=0,k=0,u=0,sumF=0,sumN=0,eofs=0,high=0,longest=0,stride=0,last=0;
 array<U,256*257> desc{},occ{};
 void guard(){auto now=chrono::system_clock::to_time_t(chrono::system_clock::now());
  need(now>=0 && U(now)<deadline && !fs::exists("stop.requested"),"stop/deadline");}
 string path(Segment s){return "vocab.tmp/"+to_string(s.id);}
 File openSegment(Segment s){auto f=file(path(s),"rb");struct stat st{};
  need(!fstat(fileno(f.get()),&st) && st.st_size==s.bytes,"segment extent");return f;}
 void retire(Segment s){need(fs::remove(path(s)),"retire");live-=s.bytes;}
 void charge(U len,bool write){need(rd+wr+len<=ioLimit,"logical IO limit");if(write){need(live+len<=2*n,"scratch limit");live+=len;peak=max(peak,live);wr+=len;}else rd+=len;}
 struct Writer{
  Census& c;Run& r;array<char,32*1024> buf;File f{nullptr,fclose};explicit Writer(Census& c,Run& r):c(c),r(r){}
  void close(){if(f)need(!fclose(f.release()),"scratch flush");}
  void put(uint32_t h,const Byte* p,U freq){unsigned len=h>>1,total=len+4+(freq?8:0);
   need(total<=c.seg,"record exceeds segment");c.charge(total,true);
   if(!f || r.back().bytes+total>c.seg){close();c.guard();need(c.files<16384,"file limit");need(r.size()<2048,"run segment limit");r.push_back({++c.files,0});f=file(c.path(r.back()),"wbx");need(!setvbuf(f.get(),buf.data(),_IOFBF,buf.size()),"bucket buffer");}
   need(fwrite(&h,4,1,f.get())==1 && (!freq || fwrite(&freq,8,1,f.get())==1) && fwrite(p,1,len,f.get())==len,"scratch write");r.back().bytes+=total;
  }
 };
 void group(uint32_t h,const Byte* p,U freq){unsigned len=h>>1;bool end=h&1;need(freq && len>(end?0:8),"bad word");unsigned owned=len-(end?0:8);
  ++k;u+=owned;sumF+=freq;sumN+=freq*owned;eofs+=end?freq:0;
  for(unsigned i=0;i<owned;++i){unsigned key=p[i]*257+(i+1<len?p[i+1]+1:0);++desc[key];occ[key]+=freq;}
 }
 // A segment is an independently bounded sorting arena. Offsets refer only to this arena.
 void sortOne(Segment s,Writer* out){guard();charge(s.bytes,false);vector<Byte>a(s.bytes);{auto f=openSegment(s);need(fread(a.data(),1,s.bytes,f.get())==s.bytes,"short segment");}retire(s);
  vector<unsigned> off;off.reserve(s.bytes/5+1);auto header=[&](unsigned i){uint32_t h;memcpy(&h,&a[i],4);return h;};
  for(unsigned i=0;i<s.bytes;){need(s.bytes-i>=4,"short header");unsigned len=header(i)>>1;need(len && len<=s.bytes-i-4,"bad length");off.push_back(i);i+=4+len;}
  unsigned steps=0;auto cmp=[&](unsigned i,unsigned j){if(!(++steps%4096))guard();auto h=header(i),g=header(j);if(h!=g)return h<g?-1:1;return memcmp(&a[i+4],&a[j+4],h>>1);};
  sort(off.begin(),off.end(),[&](unsigned i,unsigned j){return cmp(i,j)<0;});
  for(size_t i=0,j;i<off.size();i=j){j=i+1;while(j<off.size()&&!cmp(off[i],off[j]))++j;auto h=header(off[i]);auto p=&a[off[i]+4];if(out)out->put(h,p,j-i);else group(h,p,j-i);}
 }
 struct Cursor{Census& c;Run r;File f{nullptr,fclose};unsigned left=0;uint32_t h=0;U freq=0;vector<Byte> word;
  Cursor(Census& c,Run r):c(c),r(move(r)){next();}
  void next(){if(!left){if(f){f.reset();c.retire(r.front());r.pop_front();}if(r.empty()){h=0;return;}c.guard();left=r.front().bytes;f=c.openSegment(r.front());c.charge(left,false);}
   need(left>=12 && fread(&h,4,1,f.get())==1 && fread(&freq,8,1,f.get())==1,"run header");unsigned len=h>>1;need(len && len<=Mi && len<=left-12 && freq && freq<=c.m,"run length/count");word.resize(len);need(fread(word.data(),1,len,f.get())==len,"short run");left-=12+len;
  }
 };
 void mergeRuns(vector<Run>& runs,size_t i,size_t end,Writer* out){vector<Cursor> cs;cs.reserve(end-i);for(;i<end;++i)cs.emplace_back(*this,move(runs[i]));
  unsigned steps=0;for(;;){if(!(steps++%4096))guard();Cursor* best=nullptr;for(auto& x:cs)if(x.h && (!best || x.h<best->h || (x.h==best->h && x.word<best->word)))best=&x;if(!best)break;
   U freq=best->freq;for(auto& x:cs)if(&x!=best && x.h==best->h && x.word==best->word){freq+=x.freq;x.next();}
   if(out)out->put(best->h,best->word.data(),freq);else group(best->h,best->word.data(),freq);best->next();}
 }
 void process(Run& parts){if(parts.empty())return;if(parts.size()==1){sortOne(parts.front(),nullptr);return;}
  vector<Run> runs;for(auto s:parts){runs.emplace_back();Writer w(*this,runs.back());sortOne(s,&w);w.close();}
  unsigned level=0;while(runs.size()>1){need(++level<=6,"merge level limit");levels=max(levels,level);vector<Run> next;
   for(size_t i=0;i<runs.size();i+=fan){next.emplace_back();Writer w(*this,next.back());mergeRuns(runs,i,min(i+fan,runs.size()),runs.size()>fan?&w:nullptr);w.close();}runs=move(next);}
 }
 void scan(FILE* raw){array<Run,buckets> parts;vector<Writer> ws;ws.reserve(buckets);for(auto& r:parts)ws.emplace_back(*this,r);
  vector<Byte> word;word.reserve(Mi);array<Byte,65536> buf;U start=0;
  auto emit=[&](bool end){U len=word.size(),owned=len-(end?0:8);need(owned>0,"empty stride");++m;high+=base+start>UINT32_MAX;longest=max(longest,len);stride=max(stride,owned);last=start;
   uint32_t h=2*len+end;if(end && m==1){group(h,word.data(),1);return;}U hash=basis;for(auto b:word)hash=(hash^b)*prime;hash=(hash^end)*prime;ws[collide?0:(hash>>58)].put(h,word.data(),0);};
  for(U pos=0;pos<n;){guard();size_t want=min<U>(buf.size(),n-pos);need(fread(buf.data(),1,want,raw)==want,"short raw");for(size_t i=0;i<want;++i){need(word.size()<Mi,"word limit");word.push_back(buf[i]);++pos;if(pos<=8)continue;
    U h=basis;for(auto it=word.end()-8;it!=word.end();++it)h=(h^*it)*prime;if(h%31==0){emit(false);memmove(word.data(),word.data()+word.size()-8,8);word.resize(8);start=pos-8;}}}need(fgetc(raw)==EOF && !ferror(raw),"raw extent changed");emit(true);
  for(auto& w:ws)w.close();
  for(auto& r:parts)process(r);
 }
 void report(){guard();U a=0,b=0,alphabet=0;for(unsigned i=0;i<256;++i){bool seen=false;for(unsigned j=i*257;j<(i+1)*257;++j){a+=desc[j];b+=occ[j];seen|=occ[j]!=0;}alphabet+=seen;}
  need(!live && rd==wr && a==u && b==n && sumF==m && sumN==n && eofs==1,"census identities");U D=u+9*k-7;
  cout<<"n="<<n<<" m="<<m<<" k="<<k<<" U="<<u<<" D="<<D<<" tape="<<D+3*k-1<<" max_phrase="<<longest<<" max_owned_stride="<<stride<<" starts_above_u32="<<high<<" alphabet="<<alphabet<<" eof_start="<<base+last<<" eof_raw_length="<<n-last<<" global_end_rows=1 files="<<files<<" merge_levels="<<levels<<" peak="<<peak<<" scratch_read="<<rd<<" scratch_write="<<wr<<'\n';
  for(unsigned i=0;i<desc.size();++i)if(desc[i])cout<<i<<' '<<desc[i]<<' '<<occ[i]<<'\n';
 }
};
U number(const char* s){string t(s);need(!t.empty()&&t.find_first_not_of("0123456789")==string::npos,"unsigned argument");return stoull(t);}
string setting(const fs::path& p){ifstream f(p);string line;need(bool(getline(f,line)),"missing grant");return line;}
void checkGrant(){auto line=setting("/proc/self/cgroup");need(line.rfind("0::/",0)==0,"cgroup v2");auto cg=fs::path("/sys/fs/cgroup") / line.substr(4);
 need(setting(cg/"memory.max")=="201326592" && setting(cg/"cpu.max")=="100000 100000" && setting(cg/"memory.swap.max")=="0","calibration grant");}
int main(int argc,char** argv){bool created=false;try{need(argc>=3,"mode deadline required");Census c;bool fixture=string(argv[1])=="--fixture";need(fixture || string(argv[1])=="--internal-calibrate-10g","mode");c.deadline=number(argv[2]);c.guard();need(argc==3 || (fixture && argc==7),"arguments");struct stat st{};
 if(!fixture){auto d=fs::current_path();need(d.parent_path()=="/mnt/nvme2n1/erikg/TAPEREADER/sandbox" && d.filename().string().rfind("vocab-10g-",0)==0,"private calibration directory");need(!stat(".",&st) && st.st_uid==getuid() && (st.st_mode&0777)==0700,"directory permissions");checkGrant();}
 int fd=open("corpus",O_RDONLY|O_NOFOLLOW|O_NONBLOCK);need(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode),"private raw");File raw(fdopen(fd,"rb"),fclose);need(bool(raw),"fdopen");c.n=st.st_size;need(fixture?(c.n && c.n<=10000000):(c.n==TEN && st.st_nlink==1 && st.st_uid==getuid() && (st.st_mode&0777)==0400),"raw extent/mode");c.ioLimit=24*c.n;
 if(argc==7){c.seg=number(argv[3]);need(c.seg>=32 && c.seg<=8*Mi,"segment");auto collisions=number(argv[4]);need(collisions<=1,"collision switch");c.collide=collisions;c.base=number(argv[5]);need(c.base<=UINT64_MAX-c.n,"base overflow");c.ioLimit=min(c.ioLimit,number(argv[6]));}
 need(fs::create_directory("vocab.tmp"),"scratch exists");created=true;c.scan(raw.get());struct stat after{};need(!fstat(fd,&after) && after.st_size==st.st_size && after.st_ctim.tv_sec==st.st_ctim.tv_sec && after.st_ctim.tv_nsec==st.st_ctim.tv_nsec,"raw changed");need(fs::remove("vocab.tmp"),"unretired files");created=false;c.report();cout.flush();need(bool(cout),"output");return 0;
 }catch(const exception& e){cerr<<"refused: "<<e.what()<<'\n';if(created){error_code ec;fs::remove_all("vocab.tmp",ec);}return 75;}}
