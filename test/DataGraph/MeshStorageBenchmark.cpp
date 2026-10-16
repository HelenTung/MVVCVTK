#include "PagedArray.h"
#include <chrono>
#include <cstdio>
#include <future>
#include <random>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t values = 24000000, sequential = 6000000, randomReads = 100000;
constexpr unsigned repeats = 3;
volatile double observed = 0;

template<class Read> double ReadSequential(const Read& read)
{
    double sum = 0;
    for (std::size_t i = 0; i < sequential; ++i) sum += read(i);
    return sum;
}
template<class Read> double ReadRandom(const Read& read, const std::vector<std::size_t>& indices, unsigned threads)
{
    const auto run = [&](unsigned worker) {
        double sum = 0;
        for (auto i = indices.size() * worker / threads; i < indices.size() * (worker + 1) / threads; ++i)
            sum += read(indices[i]);
        return sum;
    };
    if (threads == 1) return run(0);
    std::vector<std::future<double>> workers;
    for (unsigned i = 0; i < threads; ++i) workers.push_back(std::async(std::launch::async, run, i));
    double sum = 0;
    for (auto& worker : workers) sum += worker.get();
    return sum;
}
template<class Work> double Measure(const char* name, std::size_t operations, double expected, const Work& work)
{
    std::array<double, repeats> times{};
    // 每个场景先预热一次，再保留全部三次测量；校验和进入可观察输出，防止消除工作。
    for (unsigned run = 0; run <= repeats; ++run) {
        const auto start = Clock::now();
        const auto sum = work();
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / operations;
        if (sum != expected) throw std::runtime_error("Benchmark checksum differs.");
        observed = sum;
        if (run) times[run - 1] = ns;
    }
    auto sorted = times;
    std::sort(sorted.begin(), sorted.end());
    std::printf("%s ns/op=[%.3f,%.3f,%.3f] median=%.3f checksum=%.1f\n",
        name, times[0], times[1], times[2], sorted[1], expected);
    std::fflush(stdout);
    return sorted[1];
}
}

int main()
{
    try {
        std::vector<double> raw(values);
        PagedArray<double> paged;
        paged.reserve(values);
        for (std::size_t i = 0; i < values; ++i) {raw[i] = double(i % 1024) * .25; paged.push_back(raw[i]);}
        std::mt19937_64 random(3);
        std::vector<std::size_t> indices(randomReads);
        for (auto& index : indices) index = random() % values;
        double randomSum = 0, sequentialSum = 0;
        for (auto index : indices) randomSum += raw[index];
        for (std::size_t i = 0; i < sequential; ++i) sequentialSum += raw[i];
        const auto heap = [&](std::size_t i) {return raw[i];};
        const auto frozen = paged.Finish();
        const auto mapped = [&](std::size_t i) {return frozen[i];};
        std::printf("values=%zu bytes=%zu random_operations=%zu repeats=%u (warm cache, Release)\n",
            values, values * sizeof(double), randomReads, repeats);
        Measure("vector sequential", sequential, sequentialSum, [&] {return ReadSequential(heap);});
        Measure("mapped sequential", sequential, sequentialSum, [&] {return ReadSequential(mapped);});
        for (unsigned threads : {1U, 8U}) {
            std::printf("threads=%u (includes worker creation and join)\n", threads);
            Measure("vector random", indices.size(), randomSum, [&] {return ReadRandom(heap, indices, threads);});
            Measure("mapped random", indices.size(), randomSum, [&] {return ReadRandom(mapped, indices, threads);});
        }
        Measure("mapped chunks", sequential, sequentialSum, [&] {
            double sum = 0;
            for (std::size_t offset = 0; offset < sequential;) {
                const auto chunk = frozen.GetChunk(offset, sequential - offset);
                if (chunk.error != MeshReadError::None || chunk.values.empty()) throw std::runtime_error("Chunk read failed.");
                for (double value : chunk.values) sum += value;
                offset = chunk.nextOffset;
            }
            return sum;
        });
        PagedArray<double> writable(values, 0.);
        std::vector<double> baseline(values, 0.);
        double expected = 0;
        // 读改写逐轮累积，两个容器均校验全部元素；校验本身在计时窗口外。
        for (unsigned run = 0; run <= repeats; ++run) {
            const auto execute = [&](const char* name, auto& array) {
                const auto start = Clock::now();
                for (auto index : indices) array[index] += 1.;
                const auto ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / indices.size();
                if (run) std::printf("%s run=%u ns/op=%.3f\n", name, run, ns);
            };
            execute("vector rmw", baseline); execute("mapped rmw", writable);
            expected += double(indices.size());
            double sum = 0;
            for (std::size_t i = 0; i < values; ++i) {
                if (writable[i] != baseline[i]) throw std::runtime_error("Write benchmark values differ.");
                sum += baseline[i];
            }
            if (sum != expected) throw std::runtime_error("Write benchmark checksum differs.");
            observed = sum;
            std::fflush(stdout);
        }
        std::printf("write checksum=%.1f\n", expected);
        return 0;
    } catch (const std::exception& error) {std::fprintf(stderr, "%s\n", error.what()); return 1;}
}
