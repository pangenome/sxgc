// Experimental, single-read reverse-stream BCR construction of the PFP frame.
// This intentionally uses a flat run array: O(r) LF/rank and insertion time,
// O(r) memory. It is a correctness prototype, not a fragment-scale builder.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>

using U = uint64_t;
struct Run { uint8_t c; U n, head, tail; };
static void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void u64(std::ofstream& f,U v) { for(int i=0;i<8;i++) f.put(char(v>>(8*i))); }
static void u32(std::ofstream& f,uint32_t v) { for(int i=0;i<4;i++) f.put(char(v>>(8*i))); }
struct BCR {
    std::vector<Run> runs;
    std::array<U,256> counts{};
    U length=10, start, primary=0, steps=0, max_runs;
    explicit BCR(U n,U limit):start(n),max_runs(limit) {
        runs.push_back({2,10,n,n+9});counts[2]=10;
    }
    struct Hit {size_t index; U offset, begin;};
    Hit find(U row) const {
        check(row<length,"row out of range");U pos=0;
        for(size_t i=0;i<runs.size();++i) {
            if(row<pos+runs[i].n)return {i,row-pos,pos};
            pos+=runs[i].n;
        }
        throw std::runtime_error("run length invariant");
    }
    U rank(uint8_t c,U row) const {
        U pos=0,ans=0;
        for(const auto& r:runs) {
            if(pos>=row)break;
            U take=std::min(r.n,row-pos);if(r.c==c)ans+=take;pos+=take;
        }
        return ans;
    }
    U sa(U row) const {
        // All padding rotations tie before the first text byte. Their
        // deterministic PFP order is their increasing padding position.
        if(!steps)return start+row;
        U q=row;
        for(U walk=0;walk<length;++walk) {
            Hit h=find(q);const Run& r=runs[h.index];
            if(h.offset==0 || h.offset+1==r.n) {
                U sample=h.offset==0?r.head:r.tail;
                return start+(sample-start+walk)%length;
            }
            U less=0;for(unsigned c=0;c<r.c;++c)less+=counts[c];
            q=less+rank(r.c,q);
        }
        throw std::runtime_error("SA sample unreachable by LF at step="+std::to_string(steps)+" row="+std::to_string(row)+" start="+std::to_string(start)+" runs="+std::to_string(runs.size()));
    }
    static void join(std::vector<Run>& rs) {
        size_t out=0;
        for(const Run& r:rs) {
            if(!r.n)continue;
            if(out && rs[out-1].c==r.c) {rs[out-1].n+=r.n;rs[out-1].tail=r.tail;}
            else rs[out++]=r;
        }
        rs.resize(out);
    }
    void replace(U row,uint8_t c,U sample,U left_sa,U right_sa) {
        Hit h=find(row);Run r=runs[h.index];std::vector<Run> pieces;
        if(h.offset)pieces.push_back({r.c,h.offset,r.head,left_sa});
        pieces.push_back({c,1,sample,sample});
        if(h.offset+1<r.n)pieces.push_back({r.c,r.n-h.offset-1,right_sa,r.tail});
        runs.erase(runs.begin()+h.index);
        runs.insert(runs.begin()+h.index,pieces.begin(),pieces.end());
        --counts[r.c];++counts[c];join(runs);
    }
    void insert(U row,uint8_t c,U sample,U left_sa,U right_sa) {
        check(row<=length,"insertion row out of range");
        if(row==length)runs.push_back({c,1,sample,sample});
        else {
            Hit h=find(row);Run r=runs[h.index];
            if(!h.offset)runs.insert(runs.begin()+h.index,{c,1,sample,sample});
            else {
                runs[h.index]={r.c,h.offset,r.head,left_sa};
                runs.insert(runs.begin()+h.index+1,{{c,1,sample,sample},{r.c,r.n-h.offset,right_sa,r.tail}});
            }
        }
        ++counts[c];++length;join(runs);
        check(runs.size()<=max_runs,"run limit reached: prototype O(r) insertion; use a dynamic run tree");
    }
    void prepend(uint8_t c) {
        check(c>=6,"text contains reserved byte after remap");
        U q=rank(c,primary);for(unsigned x=0;x<c;++x)q+=counts[x];
        Hit h=find(primary);U repl_left=0,repl_right=0;
        if(h.offset)repl_left=sa(primary-1);
        if(h.offset+1<runs[h.index].n)repl_right=sa(primary+1);
        U ins_left=0,ins_right=0;
        if(q && q<length) {
            Hit z=find(q);if(z.offset) {ins_left=sa(q-1);ins_right=sa(q);}
        }
        replace(primary,c,start,repl_left,repl_right);
        --start;insert(q,2,start,ins_left,ins_right);primary=q;++steps;
    }
    void write(const std::string& prefix) const {
        U sum=0;std::array<U,256> rc{};
        for(const auto& r:runs){sum+=r.n;++rc[r.c];}
        check(sum==length,"final length mismatch");
        std::ofstream b(prefix+".rlebwt",std::ios::binary),m(prefix+".rlebwt.meta",std::ios::binary),
                      h(prefix+".ssa",std::ios::binary),t(prefix+".ssa_t",std::ios::binary);
        check(bool(b)&&bool(m)&&bool(h)&&bool(t),"open output");
        u64(m,length);u64(m,runs.size());for(U x:counts)u64(m,x);for(U x:rc)u64(m,x);
        u64(h,runs.size());u64(t,runs.size());
        for(const auto& r:runs) {
            U remain=r.n;
            while(remain) {U take=std::min<U>(remain,0x7fffff);remain-=take;
                u32(b,uint32_t(r.c)|uint32_t(take<<8)|(remain?0x80000000u:0));}
            u64(h,r.head);u64(t,r.tail);
        }
        check(bool(b)&&bool(m)&&bool(h)&&bool(t),"write output");
    }
};
int main(int argc,char** argv) { try {
    check(argc>=3&&argc<=7,"usage: bcr_frontend INPUT OUT_PREFIX [--remap PFP.remap] [--max-runs N]");
    std::array<uint8_t,256> remap{};for(unsigned i=0;i<256;++i)remap[i]=i;
    U max_runs=1000000;
    for(int i=3;i<argc;i+=2) {
        check(i+1<argc,"missing option value");
        if(!strcmp(argv[i],"--remap")) {
            std::ifstream f(argv[i+1],std::ios::binary);check(bool(f),"open remap");
            f.read((char*)remap.data(),256);check(f.gcount()==256&&f.peek()==EOF,"remap must be 256 bytes");
        } else if(!strcmp(argv[i],"--max-runs")) max_runs=std::stoull(argv[i+1]);
        else throw std::runtime_error("unknown option");
    }
    std::array<bool,256> seen{};
    for(uint8_t c:remap) {check(!seen[c],"remap must be a permutation");seen[c]=true;}
    check(remap[30]==30,"remap must preserve record separator");
    for(const char* ext:{".rlebwt",".rlebwt.meta",".ssa",".ssa_t"})
        check(!std::filesystem::exists(std::string(argv[2])+ext),"output exists; refusing to clobber");
    struct stat st{};check(!stat(argv[1],&st)&&S_ISREG(st.st_mode)&&st.st_size>0,"input must be nonempty regular file");
    U n=st.st_size;
    std::ifstream f(argv[1],std::ios::binary);check(bool(f),"open input");
    BCR b(n,max_runs);std::array<char,65536> buf{};
    for(U end=n;end;) {
        U take=std::min<U>(end,buf.size());U at=end-take;
        f.seekg(at);f.read(buf.data(),take);check(U(f.gcount())==take,"read input");
        if(end==n)check(uint8_t(buf[take-1])==30,"collection must end in 0x1e");
        for(U j=take;j;)b.prepend(remap[uint8_t(buf[--j])]);
        end=at;
    }
    b.write(argv[2]);
    fprintf(stderr,"BCR_PROTOTYPE n=%llu padded_n=%llu r=%zu steps=%llu flat_run_bytes=%zu\n",
            (unsigned long long)n,(unsigned long long)b.length,b.runs.size(),
            (unsigned long long)b.steps,b.runs.capacity()*sizeof(Run));
    return 0;
} catch(const std::exception& e) {fprintf(stderr,"FATAL: %s\n",e.what());return 1;} }
