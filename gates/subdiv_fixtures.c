// subdiv_fixtures - the deterministic WAVE_SUBDIVISION.md fixture set.
//
//   subdiv_fixtures OUTDIR BYTES
//
// Every fixture is generated from a fixed seed-free rule, so two runs produce the
// same bytes on any machine. The shapes are the adversarial ones G2 names:
//
//   eofrun     a long run of ONE byte at the very end (the maximal single-symbol
//              tail run: the point-mass branch's provable case)
//   docsep     document-boundary terminators (0x1E) between documents
//   internsep  0x1E INSIDE words as well (a separator that is not a boundary)
//   r0         the END row's neighbourhood: the last 8 bytes are all distinct so
//              every finite tail is its own class
//   mixed      text + DNA + high bytes, i.e. several alphabets in one file
//   period2    "ababab..." - one sampled digit everywhere, the shape where
//              coverage refuses to fall and the class is NOT the tail run
//   single     one repeated byte
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using u8 = uint8_t;
using u64 = uint64_t;

static const char* kWords[] = {
    "the ", "of ", "and ", "a ", "to ", "in ", "is ", "that ", "for ", "it ",
    "Document ", "title ", "section ", "figure ", "table ", "analysis ",
    "mississippi ", "banana ", "aaaa ", "zzzz ", "0x1E ", "r=0 ", "suffix ",
    "array ", "BWT ", "wave ", "subdivision ", "descent ", "plan ", "survey ",
};

static void write_all(const std::string& path, const std::vector<u8>& v) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "subdiv_fixtures: cannot create %s\n", path.c_str()); std::exit(2); }
    if (!v.empty() && std::fwrite(v.data(), 1, v.size(), f) != v.size()) {
        std::fprintf(stderr, "subdiv_fixtures: short write %s\n", path.c_str()); std::exit(2);
    }
    std::fclose(f);
    std::printf("SUBDIV_FIXTURE name=%s bytes=%zu\n", path.c_str(), v.size());
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "subdiv_fixtures OUTDIR BYTES\n"); return 2; }
    const std::string dir = argv[1];
    const u64 bytes = std::strtoull(argv[2], nullptr, 10);
    if (!bytes) return 2;
    const unsigned nw = unsigned(sizeof(kWords) / sizeof(kWords[0]));
    std::vector<u8> buf;
    buf.reserve(size_t(bytes));

    // eofrun: prose, then a 1/16-of-the-file run of 'Z' at the end.
    {
        buf.clear();
        const u64 run = 32768, body = bytes - run;
        u64 s = 12345;
        while (buf.size() < body) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            const char* w = kWords[(s >> 33) % nw];
            for (const char* p = w; *p && buf.size() < body; ++p) buf.push_back(u8(*p));
        }
        while (buf.size() < bytes) buf.push_back(u8('Z'));
        write_all(dir + "/eofrun.txt", buf);
    }
    // docsep: documents separated by 0x1E, each a run of words.
    {
        buf.clear();
        u64 s = 99991, inDoc = 0;
        while (buf.size() < bytes) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            if (inDoc > 400) {
                buf.push_back(0x1E);
                const char* hdr = "Document end. ";
                for (const char* p = hdr; *p && buf.size() < bytes; ++p) buf.push_back(u8(*p));
                inDoc = 0;
                continue;
            }
            const char* w = kWords[(s >> 33) % nw];
            for (const char* p = w; *p && buf.size() < bytes; ++p) buf.push_back(u8(*p));
            ++inDoc;
        }
        write_all(dir + "/docsep.txt", buf);
    }
    // internsep: the same terminator sprinkled inside words.
    {
        buf.clear();
        u64 s = 424242;
        while (buf.size() < bytes) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            const char* w = kWords[(s >> 37) % nw];
            for (const char* p = w; *p && buf.size() < bytes; ++p) {
                buf.push_back(u8(*p));
                if ((s >> 20) % 37 == 0 && buf.size() < bytes) buf.push_back(0x1E);
            }
        }
        write_all(dir + "/internsep.txt", buf);
    }
    // r0: distinct bytes at the tail so every finite suffix is its own class.
    {
        buf.clear();
        const u64 run = 4096, tail = 8, body = bytes - run - tail;
        u64 s = 31337;
        while (buf.size() < body) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            const char* w = kWords[(s >> 33) % nw];
            for (const char* p = w; *p && buf.size() < body; ++p) buf.push_back(u8(*p));
        }
        for (u64 i = 0; i < run; ++i) buf.push_back(u8('q'));      // one short run
        for (u64 i = 0; i < tail; ++i) buf.push_back(u8(1 + i * 31));  // 8 distinct, none zero
        write_all(dir + "/r0.txt", buf);
    }
    // mixed: three alphabets interleaved.
    {
        buf.clear();
        u64 s = 777;
        while (buf.size() < bytes) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            const unsigned fam = unsigned((s >> 40) % 3);
            const unsigned len = unsigned((s >> 24) % 40) + 1;
            for (unsigned i = 0; i < len && buf.size() < bytes; ++i) {
                if (fam == 0) buf.push_back(u8("ACGT"[i & 3]));
                else if (fam == 1) buf.push_back(u8("acgtACGT"[i & 7]));
                else buf.push_back(u8(1 + ((s >> (i & 15)) % 31)));   // low control codes, never 0
            }
            if (buf.size() < bytes) buf.push_back(0x1E);
        }
        write_all(dir + "/mixed.txt", buf);
    }
    // period2: a PERIODIC RUN embedded in prose. The run is what matters (one
    // sampled digit at every offset inside the class, the shape where coverage
    // refuses to fall); it is kept short because the band sort's tie walk on a
    // periodic string is quadratic in the run length, and a fixture that cannot
    // finish is not a gate.
    {
        buf.clear();
        const u64 run = 4096, body = bytes - run;
        u64 s = 5150;
        while (buf.size() < body) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            const char* w = kWords[(s >> 33) % nw];
            for (const char* p = w; *p && buf.size() < body; ++p) buf.push_back(u8(*p));
        }
        for (u64 i = 0; i < run; ++i) buf.push_back(u8((i & 1) ? 'b' : 'a'));
        write_all(dir + "/period2.txt", buf);
    }
    // single: one short run of one repeated byte, then prose. (A whole-file run
    // is quadratic in the band sort's tie walk; the SHAPE is the point, and 4 KB of
    // it is enough to make a whole prefix class share every sampled digit.)
    {
        buf.clear();
        const u64 run = 4096;
        for (u64 i = 0; i < run; ++i) buf.push_back(u8('a'));
        u64 s = 24680;
        while (buf.size() < bytes) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            const char* w = kWords[(s >> 33) % nw];
            for (const char* p = w; *p && buf.size() < bytes; ++p) buf.push_back(u8(*p));
        }
        write_all(dir + "/single.txt", buf);
    }
    // refuse: ONE short phrase repeated everywhere, so a prefix class exceeds the
    // wave goal while NOT being the maximal single-symbol tail run - the shape the
    // census cannot prove and must refuse (G5), with numbers.
    {
        buf.clear();
        const char* phrase = "spam ";
        while (buf.size() < bytes)
            for (const char* p = phrase; *p && buf.size() < bytes; ++p) buf.push_back(u8(*p));
        write_all(dir + "/refuse.txt", buf);
    }
    return 0;
}
