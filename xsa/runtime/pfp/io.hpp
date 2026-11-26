#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pfp {
using U = std::uint64_t;
namespace fs = std::filesystem;
static_assert(sizeof(off_t) == 8 && std::endian::native == std::endian::little);
[[noreturn]] inline void fail(const char* why) {
    std::fprintf(stderr, "pfp: %s\n", why); std::exit(1);
}
inline void check(bool ok, const char* why) { if (!ok) fail(why); }
inline U number(std::string_view s) {
    U x; auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), x);
    check(e == std::errc{} && p == s.data() + s.size() && x <= INT64_MAX, "invalid unsigned extent");
    return x;
}
inline U ceildiv(U x, U y) { return x / y + (x % y != 0); }
struct File {
    int fd = -1;
    File() = default;
    explicit File(const fs::path& path, bool create = false) {
        fd = ::open(path.c_str(), O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                    (create ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY), 0600);
        if (fd < 0) { std::perror(path.c_str()); fail("open regular file"); }
    }
    File(const File&) = delete;
    File(File&& o) noexcept : fd(std::exchange(o.fd, -1)) {}
    File& operator=(File&& o) noexcept { close(); fd = std::exchange(o.fd, -1); return *this; }
    void close() { if (fd >= 0) check(!::close(std::exchange(fd, -1)), "close file"); }
    ~File() { close(); }
    U size() const { struct stat s{}; check(!fstat(fd, &s) && S_ISREG(s.st_mode) && s.st_size >= 0, "expected regular file"); return s.st_size; }
    void resize(U n) const { check(n <= INT64_MAX && !ftruncate(fd, n), "size file"); }
    void read(U pos, void* dst, U n) const {
        auto* p = static_cast<char*>(dst);
        while (n) { auto got = pread(fd, p, std::min<U>(n, 1<<26), pos);
            if (got < 0 && errno == EINTR) continue;
            check(got > 0, "short file read"); pos += got; p += got; n -= got;
        }
    }
    void write(U pos, const void* src, U n) const {
        auto* p = static_cast<const char*>(src);
        while (n) { auto sent = pwrite(fd, p, std::min<U>(n, 1<<26), pos);
            if (sent < 0 && errno == EINTR) continue;
            check(sent > 0, "short file write"); pos += sent; p += sent; n -= sent;
        }
    }
};
struct Scratch {
    U limit, live = 0, peak = 0;
    void take(U n) {
        if (n > limit-live) { std::fprintf(stderr,"pfp: scratch live=%" PRIu64 " requested=%" PRIu64 " limit=%" PRIu64 "\n", live,n,limit); fail("scratch budget exceeded"); }
        live += n; peak = std::max(peak,live);
    }
    void retire(const fs::path& name, File& f) {
        auto bytes = f.size(); check(bytes <= live && fs::remove(name), "retire temporary file"); f.close(); live -= bytes;
    }
};
template<class T, size_t K = 256> struct Writer {
    const File& f;
    std::array<T,K> buf;
    U used = 0, pos = 0;
    void flush() { f.write(pos,buf.data(),used*sizeof(T)); pos += used*sizeof(T); used=0; }
    void put(T x) { buf[used++] = x; if (used==K) flush(); }
};
// On disk these are three LE u64s in increasing lexical-ID order. The
// terminal is unambiguous even when a nonterminal's overlap ends at N.
struct Phrase { U start, stride, count; };
static_assert(sizeof(Phrase)==24);
// ID-level census in symbols, not bytes. This is a fixed-size private wire.
struct Census { U n, m, k, owned, max_id; };
static_assert(sizeof(Census)==40);
}
