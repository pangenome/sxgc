// Bounded explicit I/O for parsed-space construction. No mmap.
#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace sxi_external {
inline void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(std::string("external: ") + what);
}
inline uint64_t setting(const char* name, uint64_t fallback) {
    const char* s = std::getenv(name);
    if (!s) return fallback;
    char* end = nullptr; errno = 0;
    unsigned long long v = std::strtoull(s, &end, 10);
    require(*s && *s != '-' && end && !*end && !errno && v, name);
    return v;
}
inline void read_at(int fd, void* dst, uint64_t bytes, uint64_t pos) {
    auto p = static_cast<char*>(dst);
    while (bytes) {
        ssize_t n = pread(fd, p, std::min<uint64_t>(bytes, 1ULL<<26), pos);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "short read"); p += n; pos += n; bytes -= n;
    }
}
inline void write_at(int fd, const void* src, uint64_t bytes, uint64_t pos) {
    auto p = static_cast<const char*>(src);
    while (bytes) {
        ssize_t n = pwrite(fd, p, std::min<uint64_t>(bytes, 1ULL<<26), pos);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "short write / disk full"); p += n; pos += n; bytes -= n;
    }
}
// Each run is allocated on the next configured volume. Unique owned files only.
struct TempFile {
    int fd = -1; std::string path;
    explicit TempFile(uint64_t stripe = 0) {
        const char* env = std::getenv("SXI_SCRATCH_DIRS");
        std::string dirs = env ? env : ".";
        std::vector<std::string> roots;
        size_t at = 0, end;
        do {
            end = dirs.find(':', at); roots.push_back(dirs.substr(at, end-at));
            require(!roots.back().empty(), "empty scratch directory"); at=end+1;
        }
        while (end != std::string::npos);
        path = roots[stripe % roots.size()] + "/sxi-external-XXXXXX";
        std::vector<char> name(path.begin(), path.end()); name.push_back(0);
        fd = mkstemp(name.data()); require(fd >= 0, "create scratch file"); path=name.data();
    }
    ~TempFile() { if (fd >= 0) close(fd); if (!path.empty()) unlink(path.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};
// Direct-mapped read cache with a fixed allocation. Collisions affect speed only.
class PagedBytes {
    int fd_; uint64_t size_, pad_;
    static constexpr uint64_t page_size = 65536;
    mutable std::vector<uint64_t> tags;
    mutable std::vector<uint8_t> pages;
public:
    PagedBytes(const std::string& path, uint64_t w, uint64_t cache_bytes)
        : fd_(open(path.c_str(), O_RDONLY)), size_(0), pad_(0) {
        struct stat st{}; require(fd_ >= 0 && !fstat(fd_, &st) && st.st_size > 0, "open dictionary");
        size_=st.st_size;
        uint64_t dollars=0; uint8_t c;
        while (dollars < size_) { read_at(fd_, &c, 1, dollars); if (c != 2) break; ++dollars; }
        require(dollars && dollars <= w, "dictionary padding"); pad_=w-dollars;
        uint64_t count=std::max<uint64_t>(1, cache_bytes/page_size);
        tags.assign(count, UINT64_MAX); pages.resize(count*page_size);
        require((*this)[size()-1] == 0, "dictionary terminator");
    }
    ~PagedBytes() { if (fd_ >= 0) close(fd_); }
    PagedBytes(const PagedBytes&) = delete;
    uint64_t size() const { return size_+pad_; }
    uint8_t operator[](uint64_t pos) const {
        require(pos < size(), "dictionary position");
        if (pos < pad_) return 2;
        pos -= pad_; uint64_t page=pos/page_size, slot=page%tags.size();
        if (tags[slot] != page) {
            read_at(fd_, pages.data()+slot*page_size, std::min(page_size,size_-page*page_size), page*page_size);
            tags[slot]=page;
        }
        return pages[slot*page_size+pos%page_size];
    }
};
}
