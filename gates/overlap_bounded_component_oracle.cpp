// Direct row43 -> checked narrow LE hash + raw suffix proof. No row spool.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/mman.h>
#include <unistd.h>

namespace fs = std::filesystem;
using U = uint32_t;
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && sizeof(U) == 4);
constexpr char expected[] = "82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e";
void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
void stop(const char *flag) { if (fs::exists(flag)) std::exit(75); }
int main(int ac, char **av) try {
    require(ac == 4, "--component-100m PRIVATE_RAW STOP");
    auto size = fs::file_size(av[2]);
    bool exact = std::string(av[1]) == "--component-100m" && size == 100000503;
#ifdef COMPONENT_TINY_TEST
    require(std::string(av[1]) == "--tiny-test" && size && size <= 4096, "fixture rung");
#else
    require(exact, "wrong component rung");
#endif
    stop(av[3]);
    U n = size;
    int fd = open(av[2], O_RDONLY | O_NOFOLLOW);
    require(fd >= 0, "open raw");
    auto raw = static_cast<const unsigned char*>(mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0));
    require(raw != MAP_FAILED, "map raw");
    EVP_MD_CTX *sha = EVP_MD_CTX_new();
    require(sha && EVP_DigestInit_ex(sha, EVP_sha256(), nullptr), "start sha");
    std::array<U, 3*8192> buf;
    U prev = n, rows = 0;
    for (;;) {
        stop(av[3]);
        auto bytes = std::fread(buf.data(), 1, sizeof buf, stdin);
        if (!bytes) break;
        require(bytes % 12 == 0, "partial row");
        for (size_t i = 0; i < bytes/4; i += 3) {
            U p = buf[i], h = buf[i+1], tag = buf[i+2], b = tag & 511;
            // Both 11-bit high fields must be zero BEFORE narrowing.
            require((tag & ~511u) == 0x80000000 && b <= 256, "non-narrow row43");
            require(rows <= n && (rows ? p < n : p == n) && h <= n-std::max(prev,p), "extent");
            require(b == (p ? raw[p-1] : 256u), "predecessor");
            require(!h || !memcmp(raw+prev, raw+p, h), "prefix");
            if (rows) require(prev != p && (prev+h == n || (p+h < n && raw[prev+h] < raw[p+h])), "order/LCP");
            buf[i+2] = b;
            ++rows; prev = p;
        }
        require(EVP_DigestUpdate(sha, buf.data(), bytes), "hash narrow rows");
    }
    require(feof(stdin) && !ferror(stdin) && rows == n+1, "incomplete stream");
    U rank = 1;
    for (U p = 0; p < n; ++p) {
        if (!(p % (1u<<20))) stop(av[3]);
        int cmp = raw[p] - raw[prev];
        if (!cmp) cmp = memcmp(raw+p, raw+prev, n-std::max(p,prev));
        rank += cmp < 0 || (cmp == 0 && p >= prev);
    }
    require(rank == rows, "final rank");
    unsigned char digest[32]; unsigned len;
    require(EVP_DigestFinal_ex(sha,digest,&len) && len == 32, "finish sha");
    EVP_MD_CTX_free(sha);
    char hex[65]; for (unsigned i=0; i<len; ++i) snprintf(hex+2*i,3,"%02x",digest[i]);
    require(!exact || !strcmp(hex,expected), "legacy narrow row SHA mismatch");
    printf("VERIFIED rows=%u final_rank=%u narrow_sha256=%s\n",rows,rank,hex);
    return 0;
} catch (const std::exception &e) { fprintf(stderr,"component oracle: %s\n",e.what()); return 70; }
