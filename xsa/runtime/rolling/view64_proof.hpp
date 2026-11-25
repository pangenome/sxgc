#pragma once
// Attestation wire only. No parse, rank construction, LCP algorithm or storage assumptions here.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <openssl/evp.h>
namespace viewproof {
using U=uint64_t;
constexpr unsigned shards=64;
using Digest=std::array<unsigned char,32>;
static_assert(std::endian::native==std::endian::little);
inline void ok(bool x){if(!x){std::fprintf(stderr,"view64: invalid digest operation\n");std::_Exit(70);}}
class Sha {
 EVP_MD_CTX* ctx=EVP_MD_CTX_new();std::array<unsigned char,32768> buffer;size_t fill=0;
 void flush(){if(fill){ok(EVP_DigestUpdate(ctx,buffer.data(),fill));fill=0;}}
 public:
 Sha(){ok(ctx && EVP_DigestInit_ex(ctx,EVP_sha256(),nullptr));}
 Sha(const Sha&)=delete;~Sha(){EVP_MD_CTX_free(ctx);}
 void put(const void* data,size_t n){const auto* p=static_cast<const unsigned char*>(data);
  while(n){size_t k=std::min(n,buffer.size()-fill);std::memcpy(buffer.data()+fill,p,k);fill+=k;p+=k;n-=k;if(fill==buffer.size())flush();}}
 void raw(U p,U h,uint16_t b){put(&p,8);put(&h,8);put(&b,2);}
 void navigation(U key,U next){put(&key,8);put(&next,8);}
 void query(U a,U b,U h){put(&a,8);put(&b,8);put(&h,8);}
 Digest finish(){flush();Digest out{};unsigned len=0;ok(EVP_DigestFinal_ex(ctx,out.data(),&len) && len==32);return out;}
};
inline std::string hex(const Digest& v){char s[65];for(size_t i=0;i<v.size();++i)std::sprintf(s+2*i,"%02x",v[i]);return {s,64};}
inline std::string root(const std::array<Digest,shards>& parts){Sha h;for(const auto& p:parts)h.put(p.data(),p.size());return hex(h.finish());}
// Fixed shard boundaries independent of build team, in zero-based slots aligned to 64 where possible.
inline U boundary(U m,unsigned i){U words=m/64+(m%64!=0);return std::min(m,U(__uint128_t(words)*i/shards)*64);}
template<class F>void queries(U m,unsigned i,F f){U lo=boundary(m,i)+1,last=boundary(m,i+1);if(lo>last)return;
 f(0,lo);f(lo,lo);f(lo,last);f(last,std::min(m,last+1));f(last,1+U((__uint128_t(last)*5851+17)%m));}
} // namespace viewproof
