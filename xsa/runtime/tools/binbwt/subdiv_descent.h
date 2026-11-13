// subdiv_descent.h - the DESCENT's two strategies (WAVE_SUBDIVISION.md §3).
//
// The plan hands the descent a key-space partition (waves of bands). The descent
// only has to ROUTE positions into it, and it may do so two ways that must be
// byte-identical:
//
//   LOCAL   walk the class's ascending rolling position file through corpus
//           windows: one byte per position per level, cost = the 1 MiB pages the
//           class's own positions occupy, never n.
//   GLOBAL  one sequential sweep over the corpus, routing the class's positions
//           as the cursor passes them (several digits resolved per position).
//
// Every routing decision is a comparison of the position's corpus bytes against
// a BAND FENCE's stored prefix, so neither strategy needs a corpus read to learn
// what a fence is - and the two strategies cannot disagree, because they compare
// the same bytes against the same prefixes.

#pragma once

// ---------------------------------------------------------------------------
// A BAND FENCE WITH ITS PREFIX RESOLVED. plan_tape names a fence by (pos,depth);
// the routing needs its PREFIX BYTES, and reading them per position would be a
// corpus read per position. They are read ONCE, from the survey's prefix cache
// when the fence position is a sample (depth <= P, the common case: zero corpus
// reads) and from the text otherwise.
// ---------------------------------------------------------------------------
struct FencePfx {
    u64 pos = 0;
    u64 depth = 0;
    u64 flen = 0;            // real corpus bytes the fence owns (<= depth)
    bool after = false;
    std::vector<u8> pfx;     // flen bytes, in symbol space (REMAP applied)
};

// The comparison prefix_cmp(a, fence.pos, fence.depth) performs, but against
// stored prefix bytes. `q` is the FIRST byte of the position `p` in `src` and
// `avail` is how many real (non-END) bytes are available there.
static inline int fence_cmp(const u8* q, u64 p, u64 avail, const FencePfx& f) {
    const u64 cap = std::min<u64>(std::min<u64>(f.depth, avail), f.flen);
    for (u64 i = 0; i < cap; ++i) {
        const unsigned x = q[i], y = f.pfx[size_t(i)];
        if (x != y) return x < y ? -1 : 1;
    }
    if (cap == f.depth || p == f.pos) return 0;
    // One side ran out of text before the prefix did: the shorter finite suffix
    // is the smaller one (dollar mode), exactly as prefix_cmp spells it.
    return avail < f.flen ? -1 : 1;
}

// THE ROUTE. The fence list is ascending in key order and `after` carries the
// "this key is already past the fence" bit, exactly as the upper_bound comparator
// in the wave loop spells it:
//     first index whose fence satisfies (c > 0 || (c == 0 && !after))
static inline u64 route_fences(const u8* q, u64 p, u64 avail,
                              const std::vector<FencePfx>& fences) {
    u64 b = 0;
    for (const FencePfx& f : fences) {
        const int c = fence_cmp(q, p, avail, f);
        // upper_bound's rule, spelled out: `b` counts the fences the key is at or
        // past, so a row EQUAL to a fence lands in the band that fence starts -
        // which is the band whose first key it is. (Getting this backwards is not a
        // boundary nicety: it splits one 24-byte key's tie group across two bands,
        // and the band-local sort then orders each half without seeing the other.
        // Measured: sa_check ORDER_FAIL at rank 223590 when this was flipped.)
        if (c > 0 || (c == 0 && !f.after)) ++b;
        else break;
    }
    return b;
}

// ---------------------------------------------------------------------------
// THE LOCAL READER: a page-aligned sliding window over the corpus. Bytes are
// charged in the unit the spec's coverage is defined in (the 1 MiB page), so
// bytes_read and the plan's coverage estimate are directly comparable.
// ---------------------------------------------------------------------------
struct LocalReader {
    int fd = -1;
    u64 fileN = 0;
    std::vector<u8> buf;
    u64 base = 0, len = 0;      // live window [base, base+len)
    u64 charged_hi = 0;         // highest byte offset already charged
    u64 bytes_read = 0, refills = 0, pages = 0;

    void init(int f, u64 n) { fd = f; fileN = n; buf.resize(size_t(SS_SURVEY_PAGE)); }

    // Ensure p-need_lo .. p+need_hi are live; returns the byte pointer for p.
    const u8* fetch(u64 p, u64 need_lo, u64 need_hi) {
        if (p >= fileN) return nullptr;
        const u64 want_lo = p > need_lo ? p - need_lo : 0;
        const u64 want_hi = std::min<u64>(fileN, p + need_hi);
        if (!(base <= want_lo && want_hi <= base + len) || !len) {
            const u64 nb = want_lo / SS_SURVEY_PAGE * SS_SURVEY_PAGE;
            // Two pages: the position may sit anywhere in the first one, so the
            // 24-byte key (and the look-ahead the fences need) always fits.
            const u64 nl = std::min<u64>(2 * SS_SURVEY_PAGE, fileN - nb);
            read_window(fd, nb, nl, buf);           // remapped in place, buffered pread
            base = nb; len = nl;
            ++refills;
            const u64 hi = base + len;
            const u64 from = std::max<u64>(charged_hi, base);
            if (hi > from) { bytes_read += hi - from; charged_hi = hi; }
            pages = bytes_read / SS_SURVEY_PAGE;
        }
        return buf.data() + (p - base);
    }
    // Real bytes available at p (the text is finite; the END symbol is virtual).
    u64 avail_at(u64 p) const { return p < fileN ? fileN - p : 0; }
};

// ---------------------------------------------------------------------------
// THE POSITION FILE (the rolling window the spec names). Ascending u64 LE rows,
// written by the scatter pass and walked linearly by the local descent.
// ---------------------------------------------------------------------------
static void posfile_write(int fd, const u64* v, size_t n) {
    if (!n) return;
    const u8* p = (const u8*)v;
    size_t off = 0, bytes = n * sizeof(u64);
    while (off < bytes) {
        ssize_t w = ::write(fd, p + off, bytes - off);
        if (w <= 0) die("write position file");
        off += size_t(w);
    }
}

struct PosFile {
    int fd = -1;
    u64 rows = 0, read_rows = 0;
    std::vector<u64> buf;
    size_t at = 0, fill = 0;
    bool open_read(const std::string& path);
    bool next(u64& p);
    void close_() { if (fd >= 0) { ::close(fd); fd = -1; } }
};

bool PosFile::open_read(const std::string& path) {
    fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0) die("stat position file");
    rows = u64(st.st_size) / sizeof(u64);
    buf.resize(1u << 16);
    at = fill = 0;
    read_rows = 0;
    return true;
}

bool PosFile::next(u64& p) {
    if (at == fill) {
        ssize_t r = ::read(fd, buf.data(), buf.size() * sizeof(u64));
        if (r < 0) die("read position file");
        if (r == 0) return false;
        if (r % ssize_t(sizeof(u64))) die("truncated position file");
        fill = size_t(r) / sizeof(u64);
        at = 0;
    }
    p = buf[at++];
    ++read_rows;
    return true;
}

// THE CLASS LABEL the journals carry: the prefix in hex and as text (the corpus
// bytes, not the remapped codes - printing 33 as '!' would be a lie).
static std::string prefix_label(const u8* pre, u64 n) {
    std::string s;
    for (u64 i = 0; i < n && i < 24; ++i) {
        const unsigned b = pre[i];
        if (b == 32) s += "\\x20";
        else if (b > 32 && b < 127) s += char(b);
        else { char t[8]; std::snprintf(t, sizeof t, "\\x%02x", b); s += t; }
    }
    return s.empty() ? std::string("<root>") : s;
}
