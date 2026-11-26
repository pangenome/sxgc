#pragma once
#include "io.hpp"

namespace pfp {
constexpr U PAGE = 4096;
struct Page { U key = UINT64_MAX; std::array<uint8_t,PAGE> bytes; };
// A pointer lasts until this cache's next access. Use independent caches
// when comparing two page spans. No map of the entire lower-level tape.
struct Cache {
    const File& f; U n; std::vector<Page> pages;
    const uint8_t* at(U p) {
        U key = p / PAGE;
        Page& page = pages[key % pages.size()];
        if (key != page.key) {
            f.read(key*PAGE,page.bytes.data(),std::min(PAGE,n-key*PAGE)); page.key = key;
        }
        return page.bytes.data() + p % PAGE;
    }
    U token(U i) { U x; std::memcpy(&x,at(i*sizeof(U)),sizeof(x)); return x; }
};

// Scatter u64 values by original symbol/occurrence ordinal. Bounded
// write-back pages, including the short last page; no extra ordinal sort.
struct IdPage { U key=UINT64_MAX; std::array<U,PAGE/8> ids; };
struct Scatter {
    const File& f; U m; std::vector<IdPage> pages;
    void flush(IdPage& b) const {
        if (b.key!=UINT64_MAX) { U pos=b.key*PAGE; f.write(pos,b.ids.data(),std::min(PAGE,8*m-pos)); }
    }
    void set(U i,U id) {
        U key=i/(PAGE/8); IdPage& b=pages[key%pages.size()];
        if (b.key!=key) { flush(b); b.key=key; U p=PAGE*key; f.read(p,b.ids.data(),std::min(PAGE,8*m-p)); }
        b.ids[i%(PAGE/8)] = id;
    }
    void finish() { for (auto& b:pages) flush(b); }
};
}
