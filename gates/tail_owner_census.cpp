// Read-only audit against a banked level-0 bin map. Args: corpus modeb byte_budget.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
template<class T> struct Col {
 size_t n; const T* p;
 explicit Col(std::string path):n(std::filesystem::file_size(path)/sizeof(T)) {
  int fd=open(path.c_str(),O_RDONLY); if(fd<0 || n==0) throw std::runtime_error(path);
  auto m=mmap(nullptr,n*sizeof(T),PROT_READ,MAP_PRIVATE,fd,0); close(fd);
  if(m==MAP_FAILED) throw std::runtime_error(path); p=static_cast<const T*>(m);
 }
 ~Col(){munmap((void*)p,n*sizeof(T));}
 T operator[](size_t i)const{if(i>=n) throw std::out_of_range("column index");return p[i];}
};
int main(int argc,char** argv){
 if(argc!=4)return 2; std::string d=argv[2]; Col<uint8_t> t(argv[1]); Col<uint64_t> ref(d+"/dict_refpos.bin"),start(d+"/pstart.bin");
 Col<uint32_t> len(d+"/dict_len.bin"),os(d+"/occ_start.bin"),ol(d+"/occ_list.bin");
 std::array<uint64_t,65536> hist{},ownrows{}; std::array<unsigned,65536> owner{};
 auto budget=std::stoull(argv[3]); uint64_t target=std::max<uint64_t>(1024,(budget/5*2)/280);
 for(size_t v=0;v<len.n;v++){
  auto p=ref[v]; auto l=len[v]; if(!l||p>t.n||l>t.n-p)throw std::runtime_error("bad phrase");
  uint64_t freq=os[v+1]-os[v]; for(size_t i=0;i<l;i++){
   auto c=t.p[p+i]*256+(i+1<l?t.p[p+i+1]:0);++hist[c];ownrows[c]+=freq;
  }
 }
 uint64_t acc=0; unsigned bins=0; for(size_t c=0;c<hist.size();c++){owner[c]=bins;acc+=hist[c];if(acc>=target){bins++;acc=0;}}bins++;
 uint64_t moved=0,hops=0,ranges=0,tails=0,back=0;std::vector<uint64_t> load(bins);
 for(size_t v=0;v<len.n;v++){
  auto l=len[v];unsigned c=t[ref[v]+l-1]*256,from=owner[c],prev=bins;
  for(size_t i=os[v];i<os[v+1];i++){
   auto occ=ol[i]; auto end=start[occ]+l;unsigned to=owner[c+(end<t.n?t[end]:0)];
   ++tails;if(to<prev && prev!=bins)++back;
   if(to!=from){++moved;hops+=to-from;ranges+=(prev!=to);--ownrows[c];++load[to];}prev=to;
  }
 }
 for(size_t c=0;c<hist.size();c++)load[owner[c]]+=ownrows[c];uint64_t rows=0;for(auto n:load)rows+=n;
 if(rows!=t.n)throw std::runtime_error("coverage mismatch");
 printf("bins=%u rows=%llu tails=%llu cross_bin_occurrences=%llu next_bin_record_hops=%llu contiguous_outbound_ranges=%llu dest_order_inversions=%llu max_final_bin_rows=%llu\n",bins,(unsigned long long)rows,(unsigned long long)tails,(unsigned long long)moved,(unsigned long long)hops,(unsigned long long)ranges,(unsigned long long)back,(unsigned long long)*std::max_element(load.begin(),load.end()));
}
