#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <sys/resource.h>

void* operator new(std::size_t bytes) {
    void* p = std::malloc(bytes ? bytes : 1);
    int saved_errno = errno;
    if (!p || bytes >= 1000000000ULL) {
        struct rlimit lim;
        getrlimit(RLIMIT_AS, &lim);
        std::fprintf(stderr, "TRACE_NEW bytes=%zu result=%p errno=%d rlimit_as=%llu\n",
                     bytes, p, p ? 0 : saved_errno,
                     static_cast<unsigned long long>(lim.rlim_cur));
        std::fflush(stderr);
    }
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
