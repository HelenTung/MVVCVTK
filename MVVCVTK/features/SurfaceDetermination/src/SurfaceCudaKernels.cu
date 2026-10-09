#include "SurfaceCudaTypes.h"
#include <cmath>

namespace {
__device__ int Sample(const SurfaceCudaVolume &v, const double *position, double &result) {
    int lower[3];
    double fraction[3];
    for (int a = 0; a < 3; ++a) {
        const double low = v.extent[2 * a], high = v.extent[2 * a + 1], x = position[a];
        const double tolerance = 32 * 0x1p-52 * fmax(1., fmax(fabs(low), fabs(high)));
        if (!isfinite(x) || x < low - tolerance || x > high + tolerance)
            return 0;
        const double bounded = x < low ? low : x > high ? high : x;
        int base = int(floor(bounded));
        if (base >= v.extent[2 * a + 1])
            base = v.extent[2 * a + 1] - 1;
        if (base < v.extent[2 * a])
            base = v.extent[2 * a];
        const double weight = bounded - base;
        lower[a] = base;
        fraction[a] = weight < 0 ? 0 : weight > 1 ? 1 : weight;
    }
    result = 0;
    for (int corner = 0; corner < 8; ++corner) {
        const int x = lower[0] + (corner & 1), y = lower[1] + ((corner >> 1) & 1),
                  z = lower[2] + ((corner >> 2) & 1);
        if (z < v.firstSlice || std::int64_t(z) - v.firstSlice >= v.sliceCount)
            return 3;
        const auto index =
            (std::size_t(z - v.firstSlice) * v.dimensions[1] + (y - v.extent[2])) * v.dimensions[0] +
            (x - v.extent[0]);
        if (v.validity && v.validity[index] == 0)
            return 2;
        double value = v.scalarBytes == 4 ? double(static_cast<const float *>(v.values)[index])
                                          : static_cast<const double *>(v.values)[index];
        if (v.hasMaterialRange && isfinite(value)) {
            if (value < v.materialRange[0])
                value = v.materialRange[0];
            else if (value > v.materialRange[1])
                value = v.materialRange[1];
        }
        const double wx = (corner & 1) ? fraction[0] : 1 - fraction[0];
        const double wy = (corner & 2) ? fraction[1] : 1 - fraction[1];
        const double wz = (corner & 4) ? fraction[2] : 1 - fraction[2];
        result += wx * wy * wz * value;
    }
    return 1;
}
} // namespace
extern "C" __global__ void SurfaceSamplePoints(SurfaceCudaVolume v, const double *points, std::size_t count,
                                               SurfaceCudaSample *samples) {
    const auto id = std::size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (id >= count)
        return;
    SurfaceCudaSample out{};
    // 坐标变换由现行 CPU 运算生成；GPU 接续正式插值和梯度计算。
    const auto *index = points + 3 * id;
    double center = 0;
    out.support = Sample(v, index, center);
    double gradient[3]{};
    bool valid = true;
    for (int a = 0; a < 3; ++a) {
        double negative[3]{index[0], index[1], index[2]}, positive[3]{index[0], index[1], index[2]};
        negative[a] -= .5;
        positive[a] += .5;
        double before = 0, after = 0;
        const auto sb = Sample(v, negative, before), sa = Sample(v, positive, after);
        if (sb == 3 || sa == 3)
            out.support = 3;
        if (sb != 1 || sa != 1) {
            valid = false;
            break;
        }
        gradient[a] = after - before;
    }
    if (valid) {
        for (int a = 0; a < 3; ++a)
            out.gradient[a] = v.modelToIndex[a] * gradient[0] + v.modelToIndex[3 + a] * gradient[1] +
                              v.modelToIndex[6 + a] * gradient[2];
        const double squared = out.gradient[0] * out.gradient[0] + out.gradient[1] * out.gradient[1] +
                               out.gradient[2] * out.gradient[2];
        out.magnitude = sqrt(fmax(0., squared));
        out.gradientValid = isfinite(out.magnitude) && out.magnitude > 1e-12;
    }
    samples[id] = out;
}
