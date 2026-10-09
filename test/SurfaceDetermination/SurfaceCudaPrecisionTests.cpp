#include "SurfaceCudaCompute.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

int main() {
    SurfaceCudaVolume v;
    const int extent[6]{-3, 12, 2, 16, -4, 7};
    std::copy_n(extent, 6, v.extent);
    v.dimensions[0] = 16;
    v.dimensions[1] = 15;
    v.dimensions[2] = 12;
    v.scalarBytes = 8;
    v.origin[0] = 11.1;
    v.origin[1] = -4.6;
    v.origin[2] = 7;
    // 非零 extent、旋转和各向异性；解析灰度梯度为 (1,2,4)。
    v.modelToIndex[1] = .5;
    v.modelToIndex[3] = -1;
    v.modelToIndex[8] = 2;
    std::vector<double> values(16U * 15U * 12U);
    for (int z = -4; z <= 7; ++z)
        for (int y = 2; y <= 16; ++y)
            for (int x = -3; x <= 12; ++x)
                values[(std::size_t(z + 4) * 15 + (y - 2)) * 16 + (x + 3)] = x + 2. * y + 4. * z;
    v.values = values.data();
    const std::array<double, 6> points{1.5, 5, 2, 2.5, 7, 4};
    SurfaceCudaCompute compute;
    if (!compute.Start(v, 1024U * 1024U * 1024U, 1024U * 1024U)) {
        std::cerr << "CUDA device/backend unavailable\n";
        return 77;
    }
    std::array<SurfaceCudaSample, 2> samples;
    if (!compute.Read(points.data(), samples.size(), samples.data()))
        return 1;
    for (const auto &sample : samples) {
        if (sample.support != 1 || sample.gradientValid != 1 || sample.gradient[0] != -2 ||
            sample.gradient[1] != .5 || sample.gradient[2] != 8 || sample.magnitude != std::sqrt(68.25))
            return 1;
    }
    // 饱和发生在源节点；全饱和线性场应得到零梯度。
    v.hasMaterialRange = 1;
    v.materialRange[0] = -100;
    v.materialRange[1] = -50;
    SurfaceCudaCompute saturation;
    if (!saturation.Start(v, 1024U * 1024U * 1024U, 1024U * 1024U) ||
        !saturation.Read(points.data(), samples.size(), samples.data()))
        return 1;
    for (const auto &sample : samples)
        if (sample.gradientValid || sample.magnitude != 0 || sample.support != 1)
            return 1;
    // 缺失节点不能被饱和处理改成有效支持。
    std::vector<unsigned char> mask(values.size(), 0);
    v.validity = mask.data();
    SurfaceCudaCompute missing;
    if (!missing.Start(v, 1024U * 1024U * 1024U, 1024U * 1024U) ||
        !missing.Read(points.data(), samples.size(), samples.data()))
        return 1;
    for (const auto &sample : samples)
        if (sample.support != 2 || sample.gradientValid)
            return 1;
    // float32 输入保持原类型；精确线性场应给出同一解析梯度。
    std::vector<float> floats(values.size());
    std::transform(values.begin(), values.end(), floats.begin(),
                   [](double x) { return static_cast<float>(x); });
    v.values = floats.data();
    v.validity = nullptr;
    v.scalarBytes = 4;
    v.hasMaterialRange = 0;
    SurfaceCudaCompute single;
    if (!single.Start(v, 1024U * 1024U * 1024U, 1024U * 1024U) ||
        !single.Read(points.data(), samples.size(), samples.data()))
        return 1;
    for (const auto &sample : samples)
        if (sample.support != 1 || !sample.gradientValid || sample.gradient[0] != -2 ||
            sample.gradient[1] != .5 || sample.gradient[2] != 8)
            return 1;
    if (single.Read(points.data(), surfaceCudaBatchCapacity + 1, samples.data()))
        return 1;
    single.Reset();
    floats[(6U * 15U + 3U) * 16U + 4U] = std::numeric_limits<float>::quiet_NaN();
    SurfaceCudaCompute nonFinite;
    if (!nonFinite.Start(v, 1024U * 1024U * 1024U, 1024U * 1024U) ||
        !nonFinite.Read(points.data(), samples.size(), samples.data()) || samples[0].gradientValid)
        return 1;
    std::cout << "CUDA analytic gradients, node saturation and missing support PASS; buffers="
              << compute.GetDeviceBytes() << '\n';
    return 0;
}
