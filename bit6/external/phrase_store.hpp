// Included inside vcfbwt::pfp after the generic Dictionary template.
// Byte phrases AND hash-chain records live on disk. Only bucket heads and
// sorted phrase IDs/short keys are resident: O(number of phrases), never D.
// The integer dictionary remains the existing parse-sized implementation.
#pragma once
template<> class Dictionary<vcfbwt::char_type> {
    struct Record {
        uint64_t hash=0, offset=0, length=0, next=0;
        size_type rank=0;
        std::array<uint8_t,16> key{};
    };
    struct Key { std::array<uint8_t,16> prefix; uint64_t id; };
    struct Shard {
        sxi_external::TempFile data, index;
        std::vector<uint64_t> heads;
        uint64_t count=0, end=0;
        explicit Shard(uint64_t stripe):data(stripe),index(stripe),heads(1024,0) {}
        Record record(uint64_t id) const {
            sxi_external::require(id && id<=count,"phrase record ID");
            Record r; sxi_external::read_at(index.fd,&r,sizeof r,(id-1)*sizeof r); return r;
        }
        // Rehash only metadata, in bounded sequential blocks. Phrase bytes stay put.
        void grow() {
            heads.assign(heads.size()*2,0);
            std::vector<Record> buffer(std::min<uint64_t>(count,16384));
            for(uint64_t start=0;start<count;) {
                uint64_t take=std::min<uint64_t>(buffer.size(),count-start);
                sxi_external::read_at(index.fd,buffer.data(),take*sizeof(Record),start*sizeof(Record));
                for(uint64_t j=0;j<take;++j) {
                    auto& r=buffer[j];uint64_t bucket=(r.hash>>10)%heads.size();
                    r.next=heads[bucket];heads[bucket]=start+j+1;
                }
                sxi_external::write_at(index.fd,buffer.data(),take*sizeof(Record),start*sizeof(Record));
                start+=take;
            }
        }
    };
    std::vector<std::unique_ptr<Shard>> shards;
    std::vector<Key> order;
    std::vector<vcfbwt::char_type> fetched;
    size_t fetched_id=SIZE_MAX;
    uint64_t total_=0, count_=0;
    bool sorted_=false;
    std::mutex mutex;
    // IDs pack a shard (10 low bits) and one-based local record number.
    Record record(uint64_t id) const {return shards[id&1023]->record(id>>10);}
    std::vector<vcfbwt::char_type> read(uint64_t id,const Record& r) const {
        std::vector<vcfbwt::char_type> out(r.length);
        sxi_external::read_at(shards[id&1023]->data.fd,out.data(),out.size(),r.offset);
        return out;
    }
    uint64_t find(hash_type hash) const {
        uint64_t shard=hash%shards.size();const auto& s=*shards[shard];
        for(uint64_t id=s.heads[(hash>>10)%s.heads.size()];id;) {
            auto r=s.record(id);if(r.hash==hash)return (id<<10)|shard;id=r.next;
        }
        return 0;
    }
public:
    Dictionary() {
        const uint64_t count=sxi_external::setting("SXI_PHRASE_SHARDS",16);
        sxi_external::require(count<=1024,"phrase shard count");
        for(uint64_t i=0;i<count;++i)shards.emplace_back(new Shard(i));
    }
    uint64_t total_length() const {return total_;}
    size_type size() const {return count_;}
    hash_type get(const std::vector<vcfbwt::char_type>& phrase) const {
        return string_hash(reinterpret_cast<const char*>(phrase.data()),phrase.size());
    }
    hash_type check_and_add(const std::vector<vcfbwt::char_type>& phrase) {
        std::lock_guard<std::mutex> lock(mutex);
        hash_type hash=get(phrase);uint64_t id=find(hash);
        if(id) {
            auto r=record(id);
            sxi_external::require(r.length==phrase.size() && read(id,r)==phrase,"phrase hash collision");
            return hash;
        }
        sxi_external::require(count_<std::numeric_limits<size_type>::max()-1000ULL,"phrase IDs exceed PFP ABI");
        auto& s=*shards[hash%shards.size()];
        if(s.count>=4*s.heads.size())s.grow();
        uint64_t bucket=(hash>>10)%s.heads.size();
        Record r{};r.hash=hash;r.offset=s.end;r.length=phrase.size();r.next=s.heads[bucket];
        std::copy_n(phrase.begin(),std::min(phrase.size(),r.key.size()),r.key.begin());
        sxi_external::write_at(s.data.fd,phrase.data(),phrase.size(),s.end);
        sxi_external::write_at(s.index.fd,&r,sizeof r,s.count*sizeof r);
        s.end+=phrase.size();s.heads[bucket]=++s.count;
        ++count_;total_+=phrase.size();sorted_=false;
        return hash;
    }
    hash_type add(const std::vector<vcfbwt::char_type>& phrase) {
        sxi_external::require(!contains(phrase),"duplicate phrase in add");return check_and_add(phrase);
    }
    bool contains(const std::vector<vcfbwt::char_type>& phrase) {
        std::lock_guard<std::mutex> lock(mutex);
        uint64_t id=find(get(phrase));return id && read(id,record(id))==phrase;
    }
    void sort() {
        std::lock_guard<std::mutex> lock(mutex);
        if(sorted_)return;
        order.clear();order.reserve(count_);
        for(uint64_t s=0;s<shards.size();++s)
            for(uint64_t i=1;i<=shards[s]->count;++i)order.push_back({shards[s]->record(i).key,(i<<10)|s});
        std::sort(order.begin(),order.end(),[this](const Key& a,const Key& b) {
            if(a.prefix!=b.prefix)return a.prefix<b.prefix;
            return read(a.id,record(a.id)) < read(b.id,record(b.id));
        });
        for(size_t i=0;i<order.size();++i) {
            uint64_t id=order[i].id;size_type rank=i+1;
            sxi_external::write_at(shards[id&1023]->index.fd,&rank,sizeof rank,
                ((id>>10)-1)*sizeof(Record)+offsetof(Record,rank));
        }
        sorted_=true;fetched_id=SIZE_MAX;
        uint64_t heads=0;for(const auto& s:shards)heads+=s->heads.capacity()*8;
        spdlog::info("EXTERNAL_PHRASES phrases={} phrase_disk_bytes={} metadata_disk_bytes={} bucket_bytes={} order_bytes={} shards={}",
            count_,total_,count_*sizeof(Record),heads,order.capacity()*sizeof(Key),shards.size());
    }
    size_type hash_to_rank(hash_type hash) {
        if(!sorted_)sort();uint64_t id=find(hash);
        sxi_external::require(id!=0,"unknown phrase hash");return record(id).rank;
    }
    const std::vector<vcfbwt::char_type>& sorted_entry_at(size_t i) {
        if(!sorted_)sort();
        if(i!=fetched_id) {uint64_t id=order.at(i).id;fetched=read(id,record(id));fetched_id=i;}
        return fetched;
    }
};
