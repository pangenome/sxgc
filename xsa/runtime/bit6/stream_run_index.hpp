// The two directories sampled while m1 is written (m12 and m13).
// Shared by the ordered-row and piece-based finishers. k is the zero-based run.
struct StreamRunIndex {
    std::vector<uint64_t> runStart, dirRun[256], dirCum[256];
    std::vector<uint16_t> dirStride[256];
    uint64_t row = 0, occ[256]{}, cum[256]{}, lastSample[256]{}, lastSampleOcc[256]{};

    void observe(uint64_t k, unsigned c, uint64_t len) {
        if (k % PF_RUNSTART_STRIDE == 0) runStart.push_back(row);
        if (occ[c] % PF_RUNDIR_OCC == 0 || k - lastSample[c] >= PF_RUNDIR_GAP) {
            if (!dirStride[c].empty()) dirStride[c].back() = uint16_t(occ[c] - lastSampleOcc[c]);
            dirRun[c].push_back(k);
            dirCum[c].push_back(cum[c]);
            dirStride[c].push_back(0);
            lastSample[c] = k;
            lastSampleOcc[c] = occ[c];
        }
        row += len;
        cum[c] += len;
        ++occ[c];
    }

    void write(ContainerOut& out, uint64_t runs) {
        const uint64_t cnt = runStart.size(), l2 = (runs + PF_RUNSTART_L2 - 1) / PF_RUNSTART_L2;
        out.begin(12, 112, cnt);
        for (uint64_t v : std::array<uint64_t,8>{PF_RUNSTART_STRIDE, cnt, runs, row, PF_RUNSTART_L2, l2, 64, 64+8*l2}) out.num(v);
        NumBuf b;
        for (uint64_t j = 0; j < l2; ++j) b.put(out, runStart.at(j * PF_RUNSTART_L2 / PF_RUNSTART_STRIDE));
        for (uint64_t v : runStart) b.put(out, v);
        b.flush(out);

        uint64_t offsets[257]{}, l2total = 0;
        for (unsigned c = 0; c < 256; ++c) {
            offsets[c+1] = offsets[c] + dirRun[c].size();
            l2total += (dirRun[c].size() + PF_RUNDIR_L2 - 1) / PF_RUNDIR_L2;
            if (!dirStride[c].empty()) dirStride[c].back() = uint16_t(occ[c] - lastSampleOcc[c]);
        }
        const uint64_t total = offsets[256], l2off = 72 + 8*(256+257), entoff = l2off + 16*l2total;
        out.begin(13, 115, total);
        for (uint64_t v : std::array<uint64_t,9>{PF_RUNDIR_OCC, PF_RUNDIR_GAP, 256, PF_RUNDIR_L2, total, row, runs, entoff, l2off}) out.num(v);
        for (const auto& entries : dirRun) b.put(out, entries.size());
        for (uint64_t off : offsets) b.put(out, off);
        for (unsigned c = 0; c < 256; ++c) {
            for (size_t j = 0; j < dirRun[c].size(); j += PF_RUNDIR_L2) {
                b.put(out, dirCum[c][j]); b.put(out, dirRun[c][j]);
            }
        }
        for (unsigned c = 0; c < 256; ++c) {
            for (size_t j = 0; j < dirRun[c].size(); ++j) {
                b.put(out, dirCum[c][j]); b.put(out, dirRun[c][j]); b.put(out, dirStride[c][j]);
            }
        }
        b.flush(out);
        pf_check(out.ms.back().bytes == entoff + 24*total, "run directory size");
    }
};
