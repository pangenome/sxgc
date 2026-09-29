// A byte-dictionary specialization for the sequential r-pfbwt frontend.
// The integer (L2) dictionary continues to use the parse-sized implementation.
#pragma once
#include "io.hpp"
#include "../pfp_ds_vendor/pfp/sa_workspace.hpp"
#include <array>
#include <numeric>
#include <queue>

namespace sxi_external {
// Phrase boundaries are O(number of phrases), not a dense D-bit vector.
struct Boundaries {
    std::vector<uint64_t> starts;
    uint64_t extent=0;
    mutable uint64_t cached_pos=UINT64_MAX,cached_upper=0;
    uint64_t upper(uint64_t i) const {
        if(cached_pos!=i) {
            cached_upper=std::upper_bound(starts.begin(),starts.end(),i)-starts.begin();cached_pos=i;
        }
        return cached_upper;
    }
    bool operator[](uint64_t i) const {uint64_t r=upper(i);return r && starts[r-1]==i;}
    uint64_t size() const { return extent; }
};
struct Rank {
    const Boundaries* b;
    uint64_t operator()(uint64_t i) const {return i ? b->upper(i-1):0;}
    uint64_t rank(uint64_t i) const {return (*this)(i);}
};
struct Select {
    const Boundaries* b;
    uint64_t operator()(uint64_t i) const { require(i&&i<=b->starts.size(),"boundary select"); return b->starts[i-1]; }
};

// Sorted blocks retain a 32-bit local position and a clipped 16-bit LCP.
// An LCP-aware tournament avoids rescanning common prefixes at every heap
// comparison. Its final merge is replayable; there is no global SA/ISA file.
class SuffixStream {
    static constexpr uint64_t none=UINT64_MAX;
    struct Item { uint32_t pos; uint16_t lcp; };
    struct Run {
        std::unique_ptr<TempFile> file;
        uint64_t begin, count, next=0, buffered_start=none;
        std::vector<uint8_t> buffer;
        Item pop() {
            require(next<count,"SA run exhausted");
            uint64_t capacity=buffer.size()/6, start=next/capacity*capacity;
            if(start!=buffered_start) {
                read_at(file->fd,buffer.data(),6*std::min(capacity,count-start),6*start);
                buffered_start=start;
            }
            Item item{}; const auto* p=buffer.data()+6*(next++-start);
            std::memcpy(&item.pos,p,4);std::memcpy(&item.lcp,p+4,2);return item;
        }
    };
    const PagedBytes& d;
    mutable std::vector<Run> runs;
    mutable std::vector<uint64_t> positions, winners, match_lcp;
    mutable uint64_t leaves=0,index=none,current_lcp=0;
    struct Match { uint64_t winner,lcp; };
    Match compare(uint64_t a,uint64_t b,uint64_t skip=0) const {
        if(a==none)return {b,0};if(b==none)return {a,0};
        uint64_t x=positions[a],y=positions[b],k=skip;
        uint8_t ac,bc;
        for(;;++k) {
            ac=d[x+k];bc=d[y+k];
            if(ac!=bc || ac<=1)break;
        }
        return {(ac!=bc ? ac<bc : x<y) ? a:b,k};
    }
    void reset() const {
        leaves=1;while(leaves<runs.size())leaves*=2;
        winners.assign(2*leaves,none);match_lcp.assign(leaves,0);
        positions.resize(runs.size());
        for(uint64_t i=0;i<runs.size();++i) {
            auto& r=runs[i];r.next=0;r.buffered_start=none;
            if(r.count){positions[i]=r.begin+r.pop().pos;winners[leaves+i]=i;}
        }
        for(uint64_t i=leaves;i-->1;) {
            auto m=compare(winners[2*i],winners[2*i+1]);winners[i]=m.winner;match_lcp[i]=m.lcp;
        }
        index=0;current_lcp=0;
    }
    void advance() const {
        uint64_t old=winners[1],old_pos=positions[old],candidate=none,prefix=0;
        auto& run=runs[old];
        if(run.next<run.count) {
            auto item=run.pop();positions[old]=run.begin+item.pos;candidate=old;prefix=item.lcp;
            // A clipped value is an exact lower bound, not a hash guess.
            if(prefix==UINT16_MAX)
                while(d[old_pos+prefix]>1 && d[old_pos+prefix]==d[positions[old]+prefix])++prefix;
        }
        uint64_t child=leaves+old;winners[child]=candidate;
        while(child>1) {
            uint64_t parent=child/2,other=winners[child^1],old_lcp=match_lcp[parent],new_lcp;
            // Both candidates follow the previous global winner in sorted
            // order. Unequal LCPs against that winner decide order for free.
            if(other==none)new_lcp=0;
            else if(candidate==none){candidate=other;prefix=old_lcp;new_lcp=0;}
            else if(prefix>old_lcp)new_lcp=old_lcp;
            else if(prefix<old_lcp){new_lcp=prefix;candidate=other;prefix=old_lcp;}
            else {auto m=compare(candidate,other,prefix);candidate=m.winner;new_lcp=m.lcp;}
            winners[parent]=candidate;match_lcp[parent]=new_lcp;child=parent;
        }
        require(candidate!=none,"SA stream exhausted");current_lcp=prefix;++index;
        if(!(index%67108864))fprintf(stderr,"EXTERNAL_SA_PROGRESS index=%llu total=%llu\n",
            (unsigned long long)index,(unsigned long long)size());
    }
public:
    explicit SuffixStream(const PagedBytes& dict):d(dict) {}
    void build(const Boundaries& boundaries) {
        const uint64_t block=setting("SXI_SA_BLOCK_BYTES",64ULL<<20);
        require(block<INT32_MAX-1,"SA block must fit signed 32-bit workspace");
        const uint64_t buffer_bytes=setting("SXI_SA_RUN_BUFFER_BYTES",1ULL<<20);
        require(buffer_bytes>=6 && buffer_bytes<=1ULL<<30,"SA run buffer bounds");
        uint64_t begin=0;
        while(begin<d.size()-1) {
            auto stop=std::upper_bound(boundaries.starts.begin(),boundaries.starts.end(),begin+block);
            require(stop!=boundaries.starts.begin(),"SA block boundary");--stop;
            uint64_t end=*stop;
            require(end>begin,"phrase exceeds SXI_SA_BLOCK_BYTES; increase block budget");
            std::vector<uint8_t> text(end-begin+1);
            for(uint64_t i=begin;i<end;++i)text[i-begin]=d[i];text.back()=0;
            std::vector<uint32_t> sa(text.size());std::vector<int32_t> lcp(text.size(),0);
            require(sxi32_gsacak(text.data(),sa.data(),lcp.data(),nullptr,text.size())>=0,"block gSACA-K/LCP");
            require(sa[0]==text.size()-1,"block sentinel SA");
            Run r;r.file=std::make_unique<TempFile>(runs.size());r.begin=begin;r.count=end-begin;
            std::vector<uint8_t> packed(6*65536);
            for(uint64_t at=0;at<r.count;) {
                uint64_t take=std::min<uint64_t>(65536,r.count-at);
                for(uint64_t j=0;j<take;++j) {
                    require(lcp[at+j+1]>=0,"negative block LCP");
                    uint16_t clipped=std::min<uint64_t>(lcp[at+j+1],UINT16_MAX);
                    std::memcpy(packed.data()+6*j,&sa[at+j+1],4);
                    std::memcpy(packed.data()+6*j+4,&clipped,2);
                }
                write_at(r.file->fd,packed.data(),6*take,6*at);at+=take;
            }
            r.buffer.resize(buffer_bytes/6*6);runs.push_back(std::move(r));
            fprintf(stderr,"EXTERNAL_SA_BLOCK begin=%llu end=%llu runs=%zu disk_bytes=%llu\n",
                (unsigned long long)begin,(unsigned long long)end,runs.size(),(unsigned long long)(6*end));
            begin=end;
        }
        Run r;r.file=std::make_unique<TempFile>(runs.size());r.begin=d.size()-1;r.count=1;
        uint8_t zero[6]{};write_at(r.file->fd,zero,6,0);r.buffer.resize(6);runs.push_back(std::move(r));
        fprintf(stderr,"EXTERNAL_SA_READY D=%llu runs=%zu sa_lcp_disk_bytes=%llu run_buffer_bytes=%llu\n",
            (unsigned long long)d.size(),runs.size(),(unsigned long long)(6*d.size()),
            (unsigned long long)((runs.size()-1)*(buffer_bytes/6*6)+6));
    }
    uint64_t size() const {return d.size();}
    uint64_t operator[](uint64_t i) const {
        require(i<size(),"SA position");if(index==none || i<index)reset();
        while(index<i)advance();return positions[winners[1]];
    }
    uint64_t lcp(uint64_t i) const {(*this)[i];return current_lcp;}
};
struct LCPStream { const SuffixStream* sa; uint64_t operator[](uint64_t i) const {return sa->lcp(i);} };
}

namespace pfpds {
template<> class dictionary<uint8_t,std::less<uint8_t>> {
public:
    bool saD_flag=true,isaD_flag=false,daD_flag=false,lcpD_flag=true,colex_id_flag=true;
    long_type w;
    sxi_external::PagedBytes d;
    sxi_external::Boundaries b_d;
    sxi_external::Rank rank_b_d{&b_d};
    sxi_external::Select select_b_d{&b_d};
    sxi_external::SuffixStream saD;
    sxi_external::LCPStream lcpD{&saD};
    // Only phrase IDs, not D-sized arrays.
    sdsl::int_vector<> colex_id,inv_colex_id;
    long_type n_phrases() const {return b_d.starts.size()-1;}
    long_type length_of_phrase(long_type id) const {return select_b_d(id+1)-select_b_d(id)-1;}
    long_type phrase_at_sa(long_type i) const {
        return rank_b_d(saD[i]+1)-1;
    }
    dictionary(std::string filename,long_type window,const std::less<uint8_t>&,
               bool=true,bool=true,bool=true,bool=true,bool=true,bool=true,bool=true)
        :w(window),d(filename+".dict",window,sxi_external::setting("SXI_DICT_CACHE_BYTES",256ULL<<20)),saD(d) {
        b_d.extent=d.size();b_d.starts.push_back(0);
        for(uint64_t i=1;i<d.size();++i)if(d[i-1]==1)b_d.starts.push_back(i);
        sxi_external::require(b_d.starts.back()==d.size()-1,"dictionary phrase terminator");
        sxi_external::require(n_phrases()<=UINT32_MAX,"dictionary phrase ID overflow");
        std::vector<uint32_t> ids(n_phrases());std::iota(ids.begin(),ids.end(),uint32_t(0));
        std::sort(ids.begin(),ids.end(),[this](uint32_t a,uint32_t b) {
            uint64_t ab=select_b_d(uint64_t(a)+1),ae=select_b_d(uint64_t(a)+2)-1;
            uint64_t bb=select_b_d(uint64_t(b)+1),be=select_b_d(uint64_t(b)+2)-1;
            while(ae>ab&&be>bb) {uint8_t ac=d[--ae],bc=d[--be];if(ac!=bc)return ac<bc;}
            return ae==ab&&be!=bb;
        });
        uint8_t bits=1;for(uint64_t n=n_phrases();n>>=1;)++bits;
        colex_id=sdsl::int_vector<>(n_phrases(),0,bits);inv_colex_id=sdsl::int_vector<>(n_phrases(),0,bits);
        for(uint64_t i=0;i<ids.size();++i){colex_id[i]=ids[i];inv_colex_id[ids[i]]=i;}
        ids.clear();ids.shrink_to_fit();saD.build(b_d);
        fprintf(stderr,"EXTERNAL_DICTIONARY phrases=%llu boundaries_bytes=%llu no_dense_D_arrays=1\n",
            (unsigned long long)n_phrases(),(unsigned long long)(b_d.starts.capacity()*8));
    }
};
}
