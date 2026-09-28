#include "agc_decompressor_lib.h"
#include <iostream>
#include <unordered_map>
#include <stdexcept>
class Probe : public CAGCDecompressorLibrary {
 std::unordered_map<uint64_t,std::vector<uint32_t>> lengths;
 std::unordered_map<uint32_t,uint64_t> refs;
 uint64_t corrected=0, segments=0;
 int64_t number(const std::vector<uint8_t>&v,size_t&p) {bool neg=false;if(v.at(p)=='-'){neg=true;++p;}int64_t n=0;bool seen=false;while(p<v.size()&&v[p]>='0'&&v[p]<='9'){seen=true;n=n*10+v[p++]-'0';}if(!seen)throw std::runtime_error("invalid number");return neg?-n:n;}
 uint64_t decoded_length(const std::vector<uint8_t>&v,size_t begin,size_t end,uint64_t ref){if(begin==end)return ref;uint64_t len=0,pred=0;size_t p=begin;while(p<end){auto c=v.at(p);if((c>='A'&&c<='U')||c=='!'){++len;++pred;++p;}else if(c==30){++p;len+=number(v,p)+4;++p;}else{int64_t pos=pred+number(v,p);uint64_t n=0;if(v.at(p)==','){++p;n=number(v,p)+min_match_len;}else n=ref-pos;if(v.at(p++)!='.'||pos<0||pos+n>ref)throw std::runtime_error("match");pred=pos+n;len+=n;}}return len;}
 uint32_t length(segment_desc_t d){auto g=d.group_id;auto id=d.in_group_id;const auto base=ss_base(archive_version,g);uint64_t ref=0;
 if(g>=no_raw_groups){auto it=refs.find(g);if(it==refs.end()){std::vector<uint8_t> packed;uint64_t raw=0;in_archive->GetPart(base+ss_ref_ext(archive_version),0,packed,raw);ref=raw?raw:packed.size();refs[g]=ref;}else ref=it->second;if(id==0)return ref;--id;}
 auto pack=id/pack_cardinality;uint64_t key=(uint64_t(g)<<32)|pack;auto it=lengths.find(key);if(it==lengths.end()){std::vector<uint8_t> packed,raw;uint64_t n=0;in_archive->GetPart(base+ss_delta_ext(archive_version),pack,packed,n);if(n){raw.resize(n);auto got=ZSTD_decompress(raw.data(),raw.size(),packed.data(),packed.size()-1);if(ZSTD_isError(got))throw std::runtime_error(ZSTD_getErrorName(got));if(got!=n){std::cerr<<"PACK "<<g<<":"<<pack<<" raw="<<n<<" got="<<got<<"\n";raw.resize(got);}}else raw=std::move(packed);std::vector<uint32_t> ls;size_t b=0;for(size_t e=0;e<raw.size();++e)if(raw[e]==255){ls.push_back(g<no_raw_groups?e-b:decoded_length(raw,b,e,ref));b=e+1;}if(b!=raw.size())throw std::runtime_error("unterminated pack");it=lengths.emplace(key,std::move(ls)).first;}return it->second.at(id%pack_cardinality);}
public:
 Probe():CAGCDecompressorLibrary(false){}
 void run(const char* path){if(!Open(path,false))throw std::runtime_error("open");if(archive_version<3000)throw std::runtime_error("probe v3 only");std::vector<std::string> samples;collection_desc->get_samples_list(samples,false);uint64_t off=0;for(auto&s:samples){std::vector<std::string> cs;ListContigs(s,cs);for(auto &c:cs){auto name=c;std::vector<segment_desc_t>ds;if(!collection_desc->get_contig_desc(s,name,ds))throw std::runtime_error("desc");uint64_t len=0;for(size_t i=0;i<ds.size();++i){auto n=length(ds[i]);++segments;if(n!=ds[i].raw_length){++corrected;std::cerr<<"CORRECTION "<<s<<' '<<c<<' '<<ds[i].group_id<<':'<<ds[i].in_group_id<<' '<<ds[i].raw_length<<" -> "<<n<<'\n';}len+=n-(i?kmer_length:0);}std::cout<<s<<'\t'<<c<<'\t'<<off<<'\t'<<len<<'\n';off+=len+1;}}std::cerr<<"total="<<off<<" segments="<<segments<<" corrections="<<corrected<<" packs="<<lengths.size()<<'\n';}
};
int main(int argc,char**argv){try{Probe p;p.run(argv[1]);}catch(std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
