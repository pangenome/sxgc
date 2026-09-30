// Blocked, AVL-indexed dynamic RLBWT for the PFP cyclic frame.
// Design independently follows the B+ rope idea of ropebwt2/3 (MIT);
// no upstream source code is used here.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
using U = uint64_t;
static void check(bool x,const char* s) { if(!x) throw std::runtime_error(s); }
static void u64(std::ofstream& f,U x) { for(int i=0;i<8;++i) f.put(char(x>>(8*i))); }
static void u32(std::ofstream& f,uint32_t x) { for(int i=0;i<4;++i) f.put(char(x>>(8*i))); }
struct Run { uint32_t n; uint8_t c; };
// Runs longer than 2^32-1 are represented as adjacent same-symbol segments.
// Serialization rejoins them, preserving the v1 run contract.
static constexpr size_t CAP=512;
struct Node {
    Node *l=nullptr,*r=nullptr;
    std::vector<Run> a;
    std::array<U,256> freq{};
    U len=0,nruns=0,local=0,local_runs=0;
    uint8_t first=0,last=0;
    int height=1;
    Node() {a.reserve(CAP+3);}
};
static U len(Node* p) {return p?p->len:0;}
static U nruns(Node* p) {return p?p->nruns:0;}
static int height(Node* p) {return p?p->height:0;}
static void refresh_runs(Node* p) {
    p->nruns=nruns(p->l)+p->local_runs+nruns(p->r);
    if(p->l && p->local_runs && p->l->last==p->a.front().c)--p->nruns;
    if(p->r && p->local_runs && p->r->first==p->a.back().c)--p->nruns;
    if(p->a.empty() && p->l && p->r && p->l->last==p->r->first)--p->nruns;
    p->first=p->l?p->l->first:(p->local_runs?p->a.front().c:p->r->first);
    p->last=p->r?p->r->last:(p->local_runs?p->a.back().c:p->l->last);
}
static void pull(Node* p) {
    p->local=0;p->local_runs=0;p->freq.fill(0);
    uint8_t prev=0;
    for(auto x:p->a) {
        p->local+=x.n;p->freq[x.c]+=x.n;
        if(!p->local_runs || x.c!=prev)++p->local_runs;
        prev=x.c;
    }
    p->len=p->local+len(p->l)+len(p->r);
    refresh_runs(p);
    for(int c=0;c<256;++c) p->freq[c]+=(p->l?p->l->freq[c]:0)+(p->r?p->r->freq[c]:0);
    p->height=1+std::max(height(p->l),height(p->r));
}
static Node* rotl(Node* p) {Node* q=p->r;p->r=q->l;q->l=p;pull(p);pull(q);return q;}
static Node* rotr(Node* p) {Node* q=p->l;p->l=q->r;q->r=p;pull(p);pull(q);return q;}
static Node* balance(Node* p) {
    pull(p);
    if(height(p->l)-height(p->r)>1) {
        if(height(p->l->r)>height(p->l->l))p->l=rotl(p->l);
        return rotr(p);
    }
    if(height(p->r)-height(p->l)>1) {
        if(height(p->r->l)>height(p->r->r))p->r=rotr(p->r);
        return rotl(p);
    }
    return p;
}
static Node* insert_first(Node* root,Node* x) {
    if(!root)return x;
    root->l=insert_first(root->l,x);
    return balance(root);
}
struct Hit {uint8_t c;U offset,begin,index,runlen;};
struct BCR {
    Node* root=nullptr;
    std::array<U,256> counts{};
    U length=10,primary=0,steps=0,max_runs;
    explicit BCR(U limit):max_runs(limit) {
        root=new Node;root->a.push_back({10,2});pull(root);counts[2]=10;
    }
    Hit find(U row) const {
        check(row<length,"row out of range");
        Node* p=root;U begin=0,ri=0;uint8_t prev=0;bool have=false;
        while(p) {
            U ll=len(p->l);
            if(row<begin+ll) {p=p->l;continue;}
            if(p->l) {
                ri+=nruns(p->l)-(have && prev==p->l->first);
                prev=p->l->last;have=true;
            }
            begin+=ll;
            if(row<begin+p->local) {
                for(auto x:p->a) {
                    if(!have || prev!=x.c)++ri;
                    if(row<begin+x.n)return {x.c,row-begin,begin,ri-1,x.n};
                    begin+=x.n;prev=x.c;have=true;
                }
                throw std::runtime_error("block invariant");
            }
            for(auto x:p->a) {
                if(!have || prev!=x.c)++ri;
                prev=x.c;have=true;
            }
            begin+=p->local;p=p->r;
        }
        throw std::runtime_error("tree invariant");
    }
    U rank(uint8_t c,U row) const {
        check(row<=length,"rank row out of range");
        Node* p=root;U ans=0,base=0;
        while(p) {
            U ll=len(p->l);
            if(row<base+ll) {p=p->l;continue;}
            if(p->l)ans+=p->l->freq[c];
            base+=ll;
            if(row<=base+p->local) {
                for(auto x:p->a) {
                    if(base>=row)break;
                    U take=std::min<U>(x.n,row-base);
                    if(x.c==c)ans+=take;
                    base+=take;
                }
                return ans;
            }
            for(auto x:p->a)if(x.c==c)ans+=x.n;
            base+=p->local;p=p->r;
        }
        return ans;
    }
    static void compact(std::vector<Run>& a) {
        size_t w=0;
        for(auto x:a) {
            if(!x.n)continue;
            if(w && a[w-1].c==x.c && U(a[w-1].n)+x.n<=UINT32_MAX)a[w-1].n+=x.n;
            else a[w++]=x;
        }
        a.resize(w);
    }
    // A character edit changes only one leaf. The enclosing AVL path is
    // updated; a full frequency rebuild is needed only on rare block splits.
    Node* edit(Node* p,U row,uint8_t c,bool insertion,U& old) {
        U ll=len(p->l);
        if(row<ll) {
            p->l=edit(p->l,row,c,insertion,old);
            if(p->l && height(p->l)-height(p->r)>1)return balance(p);
        } else if(row>ll+p->local || (!insertion && row==ll+p->local)) {
            p->r=edit(p->r,row-ll-p->local,c,insertion,old);
            if(p->r && height(p->r)-height(p->l)>1)return balance(p);
        } else {
            U at=row-ll,acc=0;size_t i=0;
            while(i<p->a.size() && acc+p->a[i].n<=at) {acc+=p->a[i].n;++i;}
            if(insertion) {
                if(i==p->a.size())p->a.push_back({1,c});
                else {
                    Run x=p->a[i];U off=at-acc;
                    if(off==0)p->a.insert(p->a.begin()+i,{1,c});
                    else {
                        p->a[i].n=uint32_t(off);
                        p->a.insert(p->a.begin()+i+1,{{1,c},{uint32_t(x.n-off),x.c}});
                    }
                }
            } else {
                check(i<p->a.size(),"replace target missing");
                Run x=p->a[i];U off=at-acc;old=x.c;
                p->a[i].n=uint32_t(off);
                p->a.insert(p->a.begin()+i+1,{{1,c},{uint32_t(x.n-off-1),x.c}});
            }
            compact(p->a);
            if(p->a.size()>CAP) {
                Node* q=new Node;
                size_t mid=p->a.size()/2;
                q->a.assign(p->a.begin()+mid,p->a.end());
                p->a.erase(p->a.begin()+mid,p->a.end());
                // q is the immediate successor of p.
                pull(q);p->r=insert_first(p->r,q);
                return balance(p);
            }
            p->local=0;p->local_runs=0;
            uint8_t prev=0;
            for(auto x:p->a) {
                p->local+=x.n;
                if(!p->local_runs || x.c!=prev)++p->local_runs;
                prev=x.c;
            }
        }
        // Fast path: only the edited symbol and the old symbol can change.
        p->len=length_delta(insertion,p->len);
        refresh_runs(p);
        if(insertion)++p->freq[c];
        else {--p->freq[old];++p->freq[c];}
        return p;
    }
    static U length_delta(bool insertion,U x) {return x+(insertion?1:0);}
    void replace(U row,uint8_t c) {U old=256;root=edit(root,row,c,false,old);check(old<256,"old char missing");--counts[old];++counts[c];}
    void insert(U row,uint8_t c) {
        check(row<=length,"insertion row out of range");
        U old=256;root=edit(root,row,c,true,old);
        ++counts[c];++length;
        check(root->nruns<=max_runs,"run segment limit reached");
    }
    void prepend(uint8_t c) {
        check(c>=6,"text contains reserved byte after remap");
        U q=rank(c,primary);
        for(unsigned x=0;x<c;++x)q+=counts[x];
        replace(primary,c);
        insert(q,2);
        primary=q;++steps;
    }
    template<class F> static void walk(Node* p,F& f) {
        if(!p)return;
        walk(p->l,f);for(auto x:p->a)f(x);walk(p->r,f);
    }
    static void write_at(int fd,U index,U sample) {
        uint8_t b[8];for(int i=0;i<8;++i)b[i]=uint8_t(sample>>(8*i));
        off_t pos=off_t(8+8*index);
        check(pwrite(fd,b,8,pos)==8,"sample pwrite");
    }
    void write(const std::string& prefix) const {
        U final_runs=0,total=0;uint8_t last=0;
        std::array<U,256> rc{};
        auto count=[&](Run x) {
            total+=x.n;
            if(!final_runs || x.c!=last) {++final_runs;++rc[x.c];last=x.c;}
        };
        walk(root,count);check(total==length,"final length mismatch");
        std::ofstream b(prefix+".rlebwt",std::ios::binary),m(prefix+".rlebwt.meta",std::ios::binary);
        check(bool(b)&&bool(m),"open BWT output");
        u64(m,length);u64(m,final_runs);
        for(U x:counts)u64(m,x);
        for(U x:rc)u64(m,x);
        U carry=0;uint8_t sym=0;
        auto flush=[&]() {
            while(carry) {
                U take=std::min<U>(carry,0x7fffff);carry-=take;
                u32(b,uint32_t(sym)|uint32_t(take<<8)|(carry?0x80000000u:0));
            }
        };
        auto output=[&](Run x) {
            if(carry && x.c!=sym)flush();
            sym=x.c;carry+=x.n;
        };
        walk(root,output);flush();
        check(bool(b)&&bool(m),"write BWT output");
        std::string hp=prefix+".ssa",tp=prefix+".ssa_t";
        int h=open(hp.c_str(),O_CREAT|O_EXCL|O_RDWR,0666);
        check(h>=0,"open head output");
        int t=open(tp.c_str(),O_CREAT|O_EXCL|O_RDWR,0666);
        check(t>=0,"open tail output");
        check(ftruncate(h,off_t(8+8*final_runs))==0 && ftruncate(t,off_t(8+8*final_runs))==0,"size samples");
        uint8_t header[8];for(int i=0;i<8;++i)header[i]=uint8_t(final_runs>>(8*i));
        check(pwrite(h,header,8,0)==8 && pwrite(t,header,8,0)==8,"sample headers");
        // LF follows the complete cyclic rotation order from SA[primary]=0.
        // This costs O(n log r) time but O(1) sample RAM. Run IDs are
        // coalesced IDs; adjacent same-symbol block fragments share an ID.
        U q=primary,sa=0,heads=0,tails=0;
        for(U step=0;step<length;++step) {
            Hit x=find(q);
            // Fragmented same-symbol runs can share a final run.
            bool head=(x.offset==0 && (x.begin==0 || find(x.begin-1).c!=x.c));
            bool tail=(x.offset+1==x.runlen && (x.begin+x.runlen==length || find(x.begin+x.runlen).c!=x.c));
            if(head) {write_at(h,x.index,sa);++heads;}
            if(tail) {write_at(t,x.index,sa);++tails;}
            U less=0;for(unsigned c=0;c<x.c;++c)less+=counts[c];
            q=less+rank(x.c,q);
            sa=sa?sa-1:length-1;
        }
        check(q==primary && heads==final_runs && tails==final_runs,"LF sample cycle incomplete");
        check(close(h)==0 && close(t)==0,"close sample outputs");
    }
};
int main(int argc,char** argv) {try {
    check(argc>=3&&argc<=7,"usage: bcr_frontend_v2 INPUT OUT_PREFIX [--remap PFP.remap] [--max-runs N]");
    std::array<uint8_t,256> remap{};for(unsigned i=0;i<256;++i)remap[i]=i;
    U max_runs=UINT64_MAX;
    for(int i=3;i<argc;i+=2) {
        check(i+1<argc,"missing option value");
        if(!strcmp(argv[i],"--remap")) {
            std::ifstream f(argv[i+1],std::ios::binary);check(bool(f),"open remap");
            f.read((char*)remap.data(),256);check(f.gcount()==256&&f.peek()==EOF,"remap must be 256 bytes");
        } else if(!strcmp(argv[i],"--max-runs"))max_runs=std::stoull(argv[i+1]);
        else throw std::runtime_error("unknown option");
    }
    std::array<bool,256> seen{};
    for(uint8_t x:remap) {check(!seen[x],"remap must be permutation");seen[x]=true;}
    check(remap[30]==30,"remap must preserve separator");
    for(const char* ext:{".rlebwt",".rlebwt.meta",".ssa",".ssa_t"})
        check(!std::filesystem::exists(std::string(argv[2])+ext),"output exists; refusing to clobber");
    struct stat st{};check(!stat(argv[1],&st)&&S_ISREG(st.st_mode)&&st.st_size>0,"input must be nonempty regular file");
    U n=st.st_size;
    std::ifstream f(argv[1],std::ios::binary);check(bool(f),"open input");
    BCR b(max_runs);std::array<char,65536> buf{};
    for(U end=n;end;) {
        U take=std::min<U>(end,buf.size()),at=end-take;
        f.seekg(at);f.read(buf.data(),take);check(U(f.gcount())==take,"read input");
        if(end==n)check(uint8_t(buf[take-1])==30,"collection must end in separator");
        for(U j=take;j;)b.prepend(remap[uint8_t(buf[--j])]);
        end=at;
    }
    b.write(argv[2]);
    fprintf(stderr,"BCR_V2 n=%llu padded_n=%llu segments=%llu blocks_tree_height=%d\n",
        (unsigned long long)n,(unsigned long long)b.length,
        (unsigned long long)b.root->nruns,b.root->height);
    return 0;
} catch(const std::exception& e) {fprintf(stderr,"FATAL: %s\n",e.what());return 1;}}
