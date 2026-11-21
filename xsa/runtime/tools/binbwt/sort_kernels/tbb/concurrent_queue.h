// LOCAL STUB (recorded in ../PROVENANCE.md): the box has no TBB and
// the sequential ips4o::sort never runs the Scheduler; this stub only
// lets the parallel-support headers compile. The parallel machinery
// (IPS4O_BUILD_PARALLEL) is NOT built here.
#pragma once
namespace tbb {
template <class T>
class concurrent_queue {
public:
    void push(const T&) {}
    bool try_pop(T&) { return false; }
    bool empty() const { return true; }
};
}  // namespace tbb
