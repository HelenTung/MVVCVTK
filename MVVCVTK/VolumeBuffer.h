#pragma once
// =====================================================================
// VolumeBuffer.h — 纯体素访问层（无文件加载，无 I/O）
//
// 职责：持有 float 体素数组，提供安全访问 + 三线性插值。
// 构造：由 GapAnalysisService 从 vtkImageData 填充（前处理阶段）。
//
// 不依赖 VTK 加载器、OpenCV、线程。
// =====================================================================

#include <vector>
#include <array>
#include <algorithm>
#include <cmath>

class VolumeBuffer {
public:
    std::vector<float>    voxels;
    std::array<int, 3>     dims = { 0, 0, 0 };
    std::array<double, 3>  spacing = { 1.0, 1.0, 1.0 };
    std::array<double, 3>  origin = { 0.0, 0.0, 0.0 };
    float                 minVal = 0.f;
    float                 maxVal = 0.f;

    // ── 安全取值（含边界检查）───────────────────────────────────────
    inline float at(int x, int y, int z) const noexcept {
        if (x < 0 || x >= dims[0] ||
            y < 0 || y >= dims[1] ||
            z < 0 || z >= dims[2]) return 0.f;
        return voxels[(size_t)x + (size_t)y * dims[0] + (size_t)z * dims[0] * dims[1]];
    }

    // ── 三线性插值：体素索引坐标 ─────────────────────────────────────
    inline float sampleTrilinearIndex(float fx, float fy, float fz) const noexcept {
        int x0 = static_cast<int>(std::floor(fx));
        int y0 = static_cast<int>(std::floor(fy));
        int z0 = static_cast<int>(std::floor(fz));
        int x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
        if (x0 < 0 || y0 < 0 || z0 < 0 ||
            x1 >= dims[0] || y1 >= dims[1] || z1 >= dims[2]) return 0.f;

        float tx = fx - x0, ty = fy - y0, tz = fz - z0;
        float c00 = at(x0, y0, z0) * (1 - tx) + at(x1, y0, z0) * tx;
        float c01 = at(x0, y0, z1) * (1 - tx) + at(x1, y0, z1) * tx;
        float c10 = at(x0, y1, z0) * (1 - tx) + at(x1, y1, z0) * tx;
        float c11 = at(x0, y1, z1) * (1 - tx) + at(x1, y1, z1) * tx;
        float c0 = c00 * (1 - ty) + c10 * ty;
        float c1 = c01 * (1 - ty) + c11 * ty;
        return c0 * (1 - tz) + c1 * tz;
    }

    // ── 三线性插值：世界坐标 (mm) ────────────────────────────────────
    inline float sampleTrilinearWorld(double x, double y, double z) const noexcept {
        return sampleTrilinearIndex(
            static_cast<float>((x - origin[0]) / spacing[0]),
            static_cast<float>((y - origin[1]) / spacing[1]),
            static_cast<float>((z - origin[2]) / spacing[2]));
    }
};