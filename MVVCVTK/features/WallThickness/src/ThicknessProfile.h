#pragma once
// 私有性能诊断：未启用时不采集；不参与计算、业务结果或公开接口。
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace ThicknessProfile
{
enum Counter { Sources, TraceCalls, TraceCells, TraceBlocks, PathCalls, PathCells,
    PathBlocks, PathMixedCells, IntervalCertificates, ExactFallbacks, Count };
enum Timer { TraceTime, PathTime, NormalTime, TimeCount };
struct Data {
    std::array<std::uint64_t, Count> counts{};
    std::array<std::uint64_t, TimeCount> nanos{};
    std::uint64_t sampledSources = 0;
    bool sample = false;
    void Add(const Data& other) {
        for (unsigned i=0;i<Count;++i) counts[i]+=other.counts[i];
        for (unsigned i=0;i<TimeCount;++i) nanos[i]+=other.nanos[i];
        sampledSources+=other.sampledSources;
    }
};
inline thread_local Data* current = nullptr;
struct Binding {
    Data* previous;
    explicit Binding(Data* data) : previous(current) { current=data; }
    ~Binding() { current=previous; }
};
inline void Increment(Counter counter) { if (current) ++current->counts[counter]; }
struct Scope {
    Data* data;
    Timer timer;
    std::chrono::steady_clock::time_point begin;
    explicit Scope(Timer value) : data(current && current->sample ? current : nullptr), timer(value) {
        if (data) begin=std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (data) data->nanos[timer]+=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-begin).count();
    }
};
inline void Print(const char* phase, double seconds, std::size_t completed, std::size_t total, const Data& data) {
    std::fprintf(stderr,"WallProfile phase=%s elapsed=%.6f completed=%zu total=%zu sampled=%llu counts=",
        phase,seconds,completed,total,static_cast<unsigned long long>(data.sampledSources));
    for (auto v:data.counts) std::fprintf(stderr,"%llu,",static_cast<unsigned long long>(v));
    std::fprintf(stderr," nanos=");
    for (auto v:data.nanos) std::fprintf(stderr,"%llu,",static_cast<unsigned long long>(v));
    std::fprintf(stderr,"\n"); std::fflush(stderr);
}
}
