#include "ArtifactTestSupport.h"
#include "remove_ring.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>

extern "C" void remove_ring(float*, float, float, int, int, int, float, float, float, int, int, int, int, int);

void TestNative()
{
    std::mt19937 engine(7321);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (const int width : { 64, 65 }) {
        for (const int mode : { 0, 1 }) {
            for (const bool constant : { false, true }) {
                std::vector<float> source(static_cast<std::size_t>(width) * 67);
                for (int y = 0; y < 67; ++y) for (int x = 0; x < width; ++x) {
                    const auto radius = std::hypot(x - 30.25, y - 31.5);
                    source[static_cast<std::size_t>(y) * width + x] = constant ? 100.0f
                        : 100.0f + static_cast<float>(5.0 * std::sin(radius)) + noise(engine);
                }
                auto reference = source;
                auto actual = source;
                remove_ring(reference.data(), 30.25f, 31.5f, width, 67, 1, 1000, -1000, 100, 30, 2, mode, 0, 1);
                Require(mvvcvtk_tomopy_remove_ring(actual.data(), width, 67, 30.25f, 31.5f,
                    1000, -1000, 100, 30, 2, mode, nullptr) == MVVCVTK_TOMOPY_OK, "hardened kernel success");
                Require(actual == reference, "native numerical parity with pinned upstream");
            }
        }
    }
    struct AllocationState final { int calls = 0; int failAt = 0; int live = 0; };
    for (int failAt = 0; failAt < 16; ++failAt) {
        AllocationState state{ 0, failAt, 0 };
        MvvcvtkTomoPyAllocator allocator{};
        allocator.context = &state;
        allocator.allocate = [](void* context, std::size_t count, std::size_t size) -> void* {
            auto& value = *static_cast<AllocationState*>(context);
            if (value.calls++ == value.failAt) return nullptr;
            auto* result = std::calloc(count, size);
            if (result) ++value.live;
            return result;
        };
        allocator.release = [](void* context, void* pointer) {
            --static_cast<AllocationState*>(context)->live;
            std::free(pointer);
        };
        std::vector<float> values(64 * 64, 100.0f);
        const auto original = values;
        const int status = mvvcvtk_tomopy_remove_ring(values.data(), 64, 64, 32, 32, 1000, -1000, 100, 30, 2, 0, &allocator);
        Require(state.live == 0, "native allocation cleanup");
        if (failAt < state.calls) {
            Require(status == MVVCVTK_TOMOPY_ALLOCATION_FAILED, "native allocation failure status");
            Require(values == original, "native failure atomicity");
        }
        else Require(status == MVVCVTK_TOMOPY_OK, "native success after all allocation sites");
    }
    MvvcvtkTomoPyLayout layout{};
    // 合法最小半径及零角向半径仍与原核一致；mean的kernel=0无需拒绝。
    for (const int mode : { 0, 1 }) for (const int angular : { 1, 30, 89 }) {
        std::vector<float> original(24 * 24);
        for (auto& value : original) value = 100 + noise(engine);
        auto reference = original;
        auto actual = original;
        remove_ring(reference.data(), 12, 12, 24, 24, 1, 1000, -1000, 100, angular, 1, mode, 0, 1);
        Require(mvvcvtk_tomopy_remove_ring(actual.data(), 24, 24, 12, 12, 1000, -1000, 100, angular, 1, mode, nullptr) == 0,
            "minimum native safe parameter boundary");
        Require(actual == reference, "minimum native upstream parity");
    }
    Require(mvvcvtk_tomopy_get_layout(16, 16, 8, 8, 30, 1, 0, &layout) != 0, "small-radius rejection");
    Require(mvvcvtk_tomopy_get_layout(64, 64, 0, 0, 30, 1, 0, &layout) != 0, "edge-center rejection");
    Require(mvvcvtk_tomopy_get_layout(64, 64, 32, 32, 180, 1, 1, &layout) != 0, "reflect window rejection");
    Require(mvvcvtk_tomopy_get_layout(std::numeric_limits<int>::max(), 64, 32, 32, 30, 1, 0, &layout) != 0, "native dimension overflow rejection");
}
