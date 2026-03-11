#pragma once
// =====================================================================
// VoidDetector.h — 空洞检测纯算法层
// =====================================================================

#include "VolumeBuffer.h"
#include "GapAnalysisTypes.h"
#include <vtkSMPTools.h>
#include <vector>
#include <cstdint>
#include <queue>
#include <map>
#include <cmath>
#include <array>
#include <mutex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── 增强型并查集（路径压缩）─────────────────────────────────────────
struct UnionFind {
    static int find(int* parent, int i) noexcept {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]]; // 路径减半
            i = parent[i];
        }
        return i;
    }
    static void unite(int* parent, int i, int j) noexcept {
        int ri = find(parent, i), rj = find(parent, j);
        if (ri != rj) parent[ri] = rj;
    }
};

class VoidDetector {
public:
    // ── Step 1：泛洪填充构建内部掩码 ───────────────────────────────
    static std::vector<uint8_t> CreateInteriorMask(
        const VolumeBuffer& vol,
        float               isoValue);

    // ── Step 2：候选空洞提取（灰度阈值筛选）────────────────────────
    static std::vector<uint8_t> ExtractCandidates(
        const VolumeBuffer& vol,
        const std::vector<uint8_t>& interiorMask,
        const VoidDetectionParams& params);

    // ── Step 3：结构张量场驱动的连通域分析 ─────────────────────────
    static std::vector<VoidRegion> LabelAndAnalyze(
        const VolumeBuffer& vol,
        std::vector<uint8_t>& candidateMask,
        const VoidDetectionParams& params,
        std::vector<int>& outLabelVol);

private:
    static std::array<float, 3> GetPrincipalDirection(
        const VolumeBuffer& vol, int x, int y, int z, int window) noexcept;
};

// ─────────────────────────────────────────────────────────────────────

inline std::array<float, 3> VoidDetector::GetPrincipalDirection(
    const VolumeBuffer& vol, int x, int y, int z, int window) noexcept
{
    float m11 = 0, m22 = 0, m33 = 0;
    for (int dz = -window; dz <= window; ++dz)
        for (int dy = -window; dy <= window; ++dy)
            for (int dx = -window; dx <= window; ++dx) {
                const float gx =
                    (vol.at(x + dx + 1, y + dy, z + dz) - vol.at(x + dx - 1, y + dy, z + dz)) * 0.5f;
                const float gy =
                    (vol.at(x + dx, y + dy + 1, z + dz) - vol.at(x + dx, y + dy - 1, z + dz)) * 0.5f;
                const float gz =
                    (vol.at(x + dx, y + dy, z + dz + 1) - vol.at(x + dx, y + dy, z + dz - 1)) * 0.5f;
                m11 += gx * gx; m22 += gy * gy; m33 += gz * gz;
            }
    const float len = std::sqrt(m11 + m22 + m33 + 1e-9f);
    return { std::sqrt(m11) / len, std::sqrt(m22) / len, std::sqrt(m33) / len };
}

inline std::vector<uint8_t> VoidDetector::CreateInteriorMask(
    const VolumeBuffer& vol, float isoValue)
{
    const int    dx = vol.dims[0];
    const int    dy = vol.dims[1];
    const int    dz = vol.dims[2];
    const size_t total = (size_t)dx * dy * dz;

    std::vector<uint8_t> mask(total, 0);
    std::queue<size_t>   q;

    // 泛洪：从六面边界向内填充外部低密度区域
    auto addSeed = [&](int x, int y, int z) {
        const size_t idx = (size_t)x + (size_t)y * dx + (size_t)z * dx * dy;
        if (vol.at(x, y, z) < isoValue && mask[idx] == 0) {
            mask[idx] = 1;
            q.push(idx);
        }
        };
    for (int z : {0, dz - 1}) for (int y = 0; y < dy; ++y) for (int x = 0; x < dx; ++x) addSeed(x, y, z);
    for (int y : {0, dy - 1}) for (int z = 0; z < dz; ++z) for (int x = 0; x < dx; ++x) addSeed(x, y, z);
    for (int x : {0, dx - 1}) for (int z = 0; z < dz; ++z) for (int y = 0; y < dy; ++y) addSeed(x, y, z);

    constexpr int nb[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    while (!q.empty()) {
        const size_t ci = q.front(); q.pop();
        const int cz = (int)(ci / (dx * dy));
        const int cy = (int)((ci / dx) % dy);
        const int cx = (int)(ci % dx);
        for (int i = 0; i < 6; ++i) {
            const int nx = cx + nb[i][0], ny = cy + nb[i][1], nz = cz + nb[i][2];
            if (nx >= 0 && nx < dx && ny >= 0 && ny < dy && nz >= 0 && nz < dz) {
                const size_t ni = (size_t)nx + (size_t)ny * dx + (size_t)nz * dx * dy;
                if (mask[ni] == 0 && vol.at(nx, ny, nz) < isoValue) {
                    mask[ni] = 1; q.push(ni);
                }
            }
        }
    }

    // 取反：外部=0，内部=1
    // 用 vtkSMPTools::For 替代 #pragma omp parallel for
    vtkSMPTools::For(0, static_cast<vtkIdType>(total),
        [&](vtkIdType begin, vtkIdType end) {
            for (vtkIdType i = begin; i < end; ++i)
                mask[i] = (mask[i] == 1) ? 0 : 1;
        }
    );
    return mask;
}

inline std::vector<uint8_t> VoidDetector::ExtractCandidates(
    const VolumeBuffer& vol,
    const std::vector<uint8_t>& interiorMask,
    const VoidDetectionParams& params)
{
    const size_t total = (size_t)vol.dims[0] * vol.dims[1] * vol.dims[2];
    std::vector<uint8_t> out(total, 0);

    // 用 vtkSMPTools::For 替代 #pragma omp parallel for
    vtkSMPTools::For(0, static_cast<vtkIdType>(total),
        [&](vtkIdType begin, vtkIdType end) {
            for (vtkIdType i = begin; i < end; ++i) {
                if (interiorMask[i] > 0
                    && vol.voxels[i] >= params.grayMin
                    && vol.voxels[i] <= params.grayMax)
                    out[i] = 1;
            }
        }
    );
    return out;
}

inline std::vector<VoidRegion> VoidDetector::LabelAndAnalyze(
    const VolumeBuffer& vol,
    std::vector<uint8_t>& mask,
    const VoidDetectionParams& params,
    std::vector<int>& outLabelVol)
{
    const int    dx = vol.dims[0];
    const int    dy = vol.dims[1];
    const int    dz = vol.dims[2];
    const size_t total = (size_t)dx * dy * dz;
    outLabelVol.assign(total, -1);

    const float cosThresh =
        std::cos(params.angleThresholdDeg * (float)M_PI / 180.f);

    // ── Step A：预计算结构张量向量场 ─────────────────────────────────
    // 用 vtkSMPTools::For 替代 #pragma omp parallel for
    std::vector<std::array<float, 3>> dirs(total);
    vtkSMPTools::For(0, static_cast<vtkIdType>(total),
        [&](vtkIdType begin, vtkIdType end) {
            for (vtkIdType i = begin; i < end; ++i) {
                if (mask[i] == 0) continue;
                const int cz = (int)(i / (dx * dy));
                const int cy = (int)((i / dx) % dy);
                const int cx = (int)(i % dx);
                dirs[i] = GetPrincipalDirection(
                    vol, cx, cy, cz, params.tensorWindowSize);
            }
        }
    );

    // ── Step B：场驱动并查集合并（扫描线，单线程，数据依赖顺序敏感）─
    // 注意：并查集扫描线本身存在前向依赖（当前体素需要引用已处理的邻居），
    // 不适合并行化，保持单线程顺序扫描。
    constexpr int nOff[13][3] = {
        {-1,0,0},{0,-1,0},{0,0,-1},
        {-1,-1,0},{-1,1,0},{-1,0,-1},{-1,0,1},
        {0,-1,-1},{0,-1,1},
        {-1,-1,-1},{-1,-1,1},{-1,1,-1},{-1,1,1}
    };
    for (size_t z = 0; z < dz; ++z)
        for (size_t y = 0; y < dy; ++y)
            for (size_t x = 0; x < dx; ++x) {
                const size_t curr = x + y * dx + z * dx * dy;
                if (mask[curr] == 0) continue;
                outLabelVol[curr] = (int)curr;

                const auto& dirCurr = dirs[curr];
                for (int i = 0; i < 13; ++i) {
                    const int nx = x + nOff[i][0];
                    const int ny = y + nOff[i][1];
                    const int nz = z + nOff[i][2];
                    if (nx >= 0 && nx < dx && ny >= 0 && ny < dy && nz >= 0 && nz < dz) {
                        const size_t ni = (size_t)nx + (size_t)ny * dx + (size_t)nz * dx * dy;
                        if (outLabelVol[ni] != -1) {
                            const auto& dirNi = dirs[ni];
                            const float dot = std::abs(
                                dirCurr[0] * dirNi[0] +
                                dirCurr[1] * dirNi[1] +
                                dirCurr[2] * dirNi[2]);
                            if (dot >= cosThresh)
                                UnionFind::unite(outLabelVol.data(),
                                    (int)curr, (int)ni);
                        }
                    }
                }
            }

    // ── Step C：ID 扁平化 + 体积统计（单线程，依赖前一步结果）──────
    std::map<int, int>       rootMap;
    std::vector<VoidRegion> regions;
    int nextId = 1;

    for (size_t i = 0; i < total; ++i) {
        if (outLabelVol[i] == -1) { outLabelVol[i] = 0; continue; }
        const int root = UnionFind::find(outLabelVol.data(), (int)i);
        if (rootMap.find(root) == rootMap.end()) {
            rootMap[root] = nextId++;
            VoidRegion r; r.id = rootMap[root];
            regions.push_back(r);
        }
        const int fid = rootMap[root];
        outLabelVol[i] = fid;
        regions[fid - 1].voxelCount++;
    }

    const double vVol = vol.spacing[0] * vol.spacing[1] * vol.spacing[2];
    std::vector<VoidRegion> filtered;
    for (auto& r : regions) {
        r.volumeMM3 = r.voxelCount * vVol;
        if (r.volumeMM3 >= params.minVolumeMM3) {
            r.equivalentDiameterMM =
                std::pow(6.0 * r.volumeMM3 / M_PI, 1.0 / 3.0);
            filtered.push_back(r);
        }
    }
    return filtered;
}