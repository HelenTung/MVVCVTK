#pragma once
#include <array>
#include <cstdint>
#include <functional>
namespace ThicknessGrayField {
using Index=std::array<std::int64_t,3>;
struct Field {
    struct UniformRegion {std::array<int,6> extent{};int sign=0;};
    std::array<int,6> extent{};double threshold=0;
    std::function<bool(const Index&,double&)> node;
    std::function<void()> check;
    std::function<UniformRegion(const Index&)> uniformRegion;
};
}
