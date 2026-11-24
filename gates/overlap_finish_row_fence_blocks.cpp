// Offline differential witness only. For each authenticated m14 directory
// entry decode all real values from the block, then independently price them.
#include "overlap_finish_row_fence.hpp"
#include <cstdio>
using namespace rowfence;
static_assert(std::endian::native==std::endian::little);
static U le(const uint8_t* p,unsigned width){U x=0;for(unsigned i=0;i<width;++i)x|=U(p[i])<<(8*i);return x;}
int main() try {
    U blocks=0,values=0,total=0,escaped=0,allones=0;std::array<U,3> modes{};std::array<uint8_t,16384> data;
    while(true){std::array<uint32_t,3> hdr{};size_t got=fread(hdr.data(),1,sizeof hdr,stdin);
        if(!got){need(feof(stdin) && !ferror(stdin),"input failed");break;}
        auto [n,size,esc]=hdr;need(got==sizeof hdr && n && n<=1024 && size>=8 && size%8==0 && size<=data.size() && esc<=n && blocks<65536,"bounded frame");
        need(fread(data.data(),1,size,stdin)==size,"truncated payload");auto* p=data.data();unsigned mode=p[0];need(mode<=2,"unknown codec");
        unsigned w=mode==2?p[1]:0;need(mode!=2 || (w && w<=32),"width");unsigned at=mode==0?1:mode==1?1+n:2+(n*w+7)/8;
        unsigned end=at+(mode?4+10*esc:0);need((end+7)/8*8==size && (mode?le(p+at,4)==esc:esc==0),"block directory span/escapes");
        need(std::all_of(p+end,p+size,[](unsigned char c){return !c;}),"nonzero block padding");
        Block histogram;unsigned e=0;U limit=mode==2?(U(1)<<w)-1:255;
        for(unsigned j=0;j<n;++j){U x=0;if(mode==1)x=p[1+j];else if(mode==2){unsigned bit=j*w,shift=bit%8;
                x=(le(p+2+bit/8,(shift+w+7)/8)>>shift)&limit;}
            if(mode && x==limit){need(e<esc,"missing escape");const auto* ptr=p+at+4+10*e++;
                need(le(ptr,2)==j,"escape index");x=le(ptr+2,8);need(x>=limit,"small escape");allones+=x==limit;}
            histogram.add(x);
        }
        auto z=histogram.cost();need(e==esc && z.escapes==esc && z.bytes==size && mode==z.mode && w==z.width,"priced block mismatch");
        ++blocks;values+=n;total+=size;escaped+=esc;++modes[mode];
    }
    need(blocks,"empty block proof");printf("BLOCK_COST blocks=%llu values=%llu payload=%llu escapes=%llu equal_markers=%llu zero=%llu u8=%llu packed=%llu\n",
        (unsigned long long)blocks,(unsigned long long)values,(unsigned long long)total,(unsigned long long)escaped,
        (unsigned long long)allones,(unsigned long long)modes[0],(unsigned long long)modes[1],(unsigned long long)modes[2]);
    return 0;
} catch(const std::exception& e){fprintf(stderr,"m14 proof refused: %s\n",e.what());return 70;}
