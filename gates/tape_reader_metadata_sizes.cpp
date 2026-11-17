// Measure the actual declaration types used by the production reader.
#define main tape_sort_original_main
#include "../xsa/runtime/tools/binbwt/tape_sort.cpp"
#undef main
int main() {
    std::printf("FollowUp=%zu TapeRequest=%zu Chunk=%zu\n",
                sizeof(FollowUp), sizeof(TapeRequest), sizeof(Chunk));
}
