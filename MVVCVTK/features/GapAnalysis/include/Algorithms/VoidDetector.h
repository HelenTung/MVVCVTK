#pragma once
// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/include/Algorithms/VoidDetector.h
// VoidDetector.h — 空洞检测纯算法层
// =====================================================================

#include "VolumeBuffer.h"
#include "GapAnalysisTypes.h"
#include <vtkSMPTools.h>
#include <vtkMath.h>
#include <vector>
#include <cstdint>
#include <queue>
#include <map>
#include <cmath>
#include <array>
#include <mutex>
#include <deque>

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
    static std::vector<uint8_t> BuildCandidates(
        const VolumeBuffer& vol,
        const std::vector<uint8_t>& interiorMask,
        const VoidDetectionParams& params);

    // ── Step 3：结构张量场驱动的连通域分析 ─────────────────────────
    static std::vector<VoidRegion> BuildRegions(
        const VolumeBuffer& vol,
        std::vector<uint8_t>& candidateMask,
        const VoidDetectionParams& params,
        std::vector<int>& outLabelVol);

private:
    /*static std::array<float, 3> GetPrincipalDirection(
        const VolumeBuffer& vol, int x, int y, int z, int window) noexcept;*/
};

// ─────────────────────────────────────────────────────────────────────

//inline std::array<float, 3> VoidDetector::GetPrincipalDirection(
//    const VolumeBuffer& vol, int x, int y, int z, int window) noexcept
//{
//    float m11 = 0, m22 = 0, m33 = 0;
//    for (int dz = -window; dz <= window; ++dz)
//        for (int dy = -window; dy <= window; ++dy)
//            for (int dx = -window; dx <= window; ++dx) {
//                const float gx =
//                    (vol.GetVoxelValue(x + dx + 1, y + dy, z + dz) - vol.GetVoxelValue(x + dx - 1, y + dy, z + dz)) * 0.5f;
//                const float gy =
//                    (vol.GetVoxelValue(x + dx, y + dy + 1, z + dz) - vol.GetVoxelValue(x + dx, y + dy - 1, z + dz)) * 0.5f;
//                const float gz =
//                    (vol.GetVoxelValue(x + dx, y + dy, z + dz + 1) - vol.GetVoxelValue(x + dx, y + dy, z + dz - 1)) * 0.5f;
//                m11 += gx * gx; m22 += gy * gy; m33 += gz * gz;
//            }
//    double direction[3] = { std::sqrt(m11), std::sqrt(m22), std::sqrt(m33) };
//    vtkMath::Normalize(direction);
//    return {
//        static_cast<float>(direction[0]),
//        static_cast<float>(direction[1]),
//        static_cast<float>(direction[2])
//    };
//}

inline std::vector<uint8_t> VoidDetector::CreateInteriorMask(
    const VolumeBuffer& vol, float isoValue)
{
    const int    dx = vol.dims[0];
    const int    dy = vol.dims[1];
    const int    dz = vol.dims[2];

    const size_t slice = (size_t)dx * dy;
    const size_t total = slice * dz;
    const float* data = vol.voxelsPtr;

    std::vector<uint8_t> exterior(total, 0);

    struct QNode {
        size_t idx;
        int x, y, z;
    };

    std::deque<QNode> q;

    auto push_node = [&](int x, int y, int z) {
        size_t idx = (size_t)x + (size_t)y * dx + (size_t)z * slice;
        if (data[idx] < isoValue && exterior[idx] == 0) {
            exterior[idx] = 1;
            q.push_back({ idx, x, y, z });
        }
        };

    // 1. 初始化种子：完全对齐原版边界逻辑
    for (int y = 0; y < dy; ++y) {
        for (int x = 0; x < dx; ++x) {
            push_node(x, y, 0);
            push_node(x, y, dz - 1);
        }
    }
    for (int z = 1; z < dz - 1; ++z) {
        for (int y = 0; y < dy; ++y) {
            push_node(0, y, z);
            push_node(dx - 1, y, z);
        }
    }
    for (int z = 1; z < dz - 1; ++z) {
        for (int x = 1; x < dx - 1; ++x) {
            push_node(x, 0, z);
            push_node(x, dy - 1, z);
        }
    }

    const size_t off[6] = { 1,static_cast<size_t>(0) - 1,          // 代替 (size_t)-1
    static_cast<size_t>(dx),
    static_cast<size_t>(0) - static_cast<size_t>(dx),   // 代替 (size_t)-dx
    slice,
    static_cast<size_t>(0) - slice       // 代替 (size_t)-slice
    };
    const int dxs[6] = { 1, -1, 0, 0, 0, 0 };
    const int dys[6] = { 0, 0, 1, -1, 0, 0 };
    const int dzs[6] = { 0, 0, 0, 0, 1, -1 };

    while (!q.empty()) {
        QNode curr = q.front();
        q.pop_front();

        if (curr.x > 0 && curr.x < dx - 1 && curr.y > 0 && curr.y < dy - 1 && curr.z > 0 && curr.z < dz - 1) {
            for (int k = 0; k < 6; ++k) {
                size_t nidx = curr.idx + off[k];
                if (exterior[nidx] == 0 && data[nidx] < isoValue) {
                    exterior[nidx] = 1;
                    q.push_back({ nidx, curr.x + dxs[k], curr.y + dys[k], curr.z + dzs[k] });
                }
            }
        }
        else {
            for (int k = 0; k < 6; ++k) {
                int nx = curr.x + dxs[k];
                int ny = curr.y + dys[k];
                int nz = curr.z + dzs[k];
                if (nx >= 0 && nx < dx && ny >= 0 && ny < dy && nz >= 0 && nz < dz) {
                    size_t nidx = (size_t)nx + (size_t)ny * dx + (size_t)nz * slice;
                    if (exterior[nidx] == 0 && data[nidx] < isoValue) {
                        exterior[nidx] = 1;
                        q.push_back({ nidx, nx, ny, nz });
                    }
                }
            }
        }
    }

    for (long long i = 0; i < (long long)total; ++i) {
        if (data[i] < isoValue && exterior[i] == 0) {
            exterior[i] = 1; // 内部孔隙
        }
        else {
            exterior[i] = 0; // 固体或外部空气
        }
    }

    return exterior;
}

inline std::vector<uint8_t> VoidDetector::BuildCandidates(
    const VolumeBuffer& vol,
    const std::vector<uint8_t>& interiorMask,
    const VoidDetectionParams& params)
{
    const int dx = vol.dims[0];
    const int dy = vol.dims[1];
    const int dz = vol.dims[2];
    const size_t slice = (size_t)dx * dy;
    const size_t total = slice * dz;

    std::vector<uint8_t> raw_mask(total, 0);

    vtkSMPTools::For(0, static_cast<vtkIdType>(total),
        [&](vtkIdType begin, vtkIdType end) {
            for (vtkIdType i = begin; i < end; ++i) {
                if (interiorMask[i] > 0 && vol.voxelsPtr[i] <= params.grayMax) {
                    raw_mask[i] = 1;
                }
            }
        });

    const int erosionIterations = params.erosionIterations;
    std::vector<uint8_t> eroded = raw_mask;
    std::vector<uint8_t> eroded_next(total, 0);

    for (int iter = 0; iter < erosionIterations; ++iter) {
        std::fill(eroded_next.begin(), eroded_next.end(), 0);
        vtkSMPTools::For(1, dz - 1, [&](vtkIdType begin, vtkIdType end) {
            for (int z = begin; z < end; ++z) {
                for (int y = 1; y < dy - 1; ++y) {
                    for (int x = 1; x < dx - 1; ++x) {
                        size_t idx = (size_t)z * slice + (size_t)y * dx + (size_t)x;
                        if (eroded[idx] &&
                            eroded[idx + 1] && eroded[idx - 1] &&
                            eroded[idx + dx] && eroded[idx - dx] &&
                            eroded[idx + slice] && eroded[idx - slice]) {
                            eroded_next[idx] = 1;
                        }
                    }
                }
            }
            });
        std::swap(eroded, eroded_next);
    }

    std::vector<uint8_t> candidates(total, 0);
    std::queue<size_t> bfsQueue;

    for (size_t i = 0; i < total; ++i) {
        if (eroded[i]) {
            bfsQueue.push(i);
            candidates[i] = 1;
        }
    }

    const std::array<long long, 6> offsets = { 1, -1, (long long)dx, -(long long)dx,
                                               (long long)slice, -(long long)slice };
    while (!bfsQueue.empty()) {
        size_t cur = bfsQueue.front();
        bfsQueue.pop();
        for (long long off : offsets) {
            size_t nb = (size_t)((long long)cur + off);
            if (nb < total && raw_mask[nb] && !candidates[nb]) {
                candidates[nb] = 1;
                bfsQueue.push(nb);
            }
        }
    }
    return candidates;
}

inline std::vector<VoidRegion> VoidDetector::BuildRegions(
    const VolumeBuffer& vol,
    std::vector<uint8_t>& candidateMask,
    const VoidDetectionParams& params,
    std::vector<int>& outLabelVol)
{
    const int dx = vol.dims[0];
    const int dy = vol.dims[1];
    const int dz = vol.dims[2];
    const size_t slice = (size_t)dx * dy;
    const size_t total = slice * dz;

    const double voxelVol = vol.spacing[0] * vol.spacing[1] * vol.spacing[2];
    outLabelVol.assign(total, 0);

    std::vector<VoidRegion> regions;
    int currentID = 1;

    const std::array<long long, 6> offsets6 = { 1, -1, (long long)dx, -(long long)dx, (long long)slice, -(long long)slice };

    // 13个独立方向 (用于表面积计算)
    const std::vector<std::array<int, 3>> directions13 = {
        {1,0,0}, {0,1,0}, {0,0,1},
        {1,1,0}, {1,-1,0}, {1,0,1}, {1,0,-1}, {0,1,1}, {0,1,-1},
        {1,1,1}, {1,1,-1}, {1,-1,1}, {1,-1,-1}
    };

    std::queue<size_t> q;

    for (size_t i = 0; i < total; ++i) {
        if (candidateMask[i] > 0 && outLabelVol[i] == 0) {

            VoidRegion region;
            region.id = currentID;

            // 初始化统计变量
            double sumGray = 0;
            double sumGraySq = 0;
            region.minGray = 1e9;
            region.maxGray = -1e9;

            int sz0 = (int)(i / slice);
            int sy0 = (int)((i / dx) % dy);
            int sx0 = (int)(i % dx);
            region.seedVoxel = { sx0, sy0, sz0 };
            region.bbox = { sx0, sx0, sy0, sy0, sz0, sz0 };

            double sumX = 0, sumY = 0, sumZ = 0;
            double sumXX = 0, sumYY = 0, sumZZ = 0;
            double sumXY = 0, sumXZ = 0, sumYZ = 0;

            std::vector<size_t> regionVoxels;
            q.push(i);
            outLabelVol[i] = currentID;

            while (!q.empty()) {
                size_t curr = q.front();
                q.pop();
                regionVoxels.push_back(curr);

                int cz = (int)(curr / slice);
                int cy = (int)((curr / dx) % dy);
                int cx = (int)(curr % dx);

                // --- 基础统计 ---
                region.voxelCount++;
                double px = (double)cx * vol.spacing[0];
                double py = (double)cy * vol.spacing[1];
                double pz = (double)cz * vol.spacing[2];

                sumX += px; sumY += py; sumZ += pz;
                sumXX += px * px; sumYY += py * py; sumZZ += pz * pz;
                sumXY += px * py; sumXZ += px * pz; sumYZ += py * pz;

                // --- 灰度统计 ---
                double val = static_cast<double>(vol.voxelsPtr[curr]);
                sumGray += val;
                sumGraySq += val * val;
                region.minGray = std::min(region.minGray, val);
                region.maxGray = std::max(region.maxGray, val);

                // --- 边界框更新 ---
                region.bbox[0] = std::min(region.bbox[0], cx);
                region.bbox[1] = std::max(region.bbox[1], cx);
                region.bbox[2] = std::min(region.bbox[2], cy);
                region.bbox[3] = std::max(region.bbox[3], cy);
                region.bbox[4] = std::min(region.bbox[4], cz);
                region.bbox[5] = std::max(region.bbox[5], cz);

                for (long long off : offsets6) {
                    size_t nb = (size_t)((long long)curr + off);
                    if (nb < total && candidateMask[nb] > 0 && outLabelVol[nb] == 0) {
                        outLabelVol[nb] = currentID;
                        q.push(nb);
                    }
                }
            }

            region.volumeMM3 = region.voxelCount * voxelVol;

            if (region.volumeMM3 >= params.minVolumeMM3) {
                // 1. 重心
                region.centroidMM[0] = sumX / region.voxelCount + vol.origin[0];
                region.centroidMM[1] = sumY / region.voxelCount + vol.origin[1];
                region.centroidMM[2] = sumZ / region.voxelCount + vol.origin[2];

                // 2. 等效直径与半径
                auto pi = std::acos(-1);
                region.equivalentDiameterMM = pow((6.0 * region.volumeMM3) / pi, 1.0 / 3.0);
                region.radius = region.equivalentDiameterMM / 2.0;

                // 3. 灰度统计
                region.meanGray = sumGray / region.voxelCount;
                double variance = (sumGraySq / region.voxelCount) - (region.meanGray * region.meanGray);
                region.stdDevGray = std::sqrt(std::max(0.0, variance));

                // 4. 投影尺寸 (mm)
                region.xProjection = (region.bbox[1] - region.bbox[0] + 1) * vol.spacing[0];
                region.yProjection = (region.bbox[3] - region.bbox[2] + 1) * vol.spacing[1];
                region.zProjection = (region.bbox[5] - region.bbox[4] + 1) * vol.spacing[2];

                // 5. PCA 分析
                double cov[3][3];
                cov[0][0] = sumXX / region.voxelCount - (sumX / region.voxelCount) * (sumX / region.voxelCount);
                cov[1][1] = sumYY / region.voxelCount - (sumY / region.voxelCount) * (sumY / region.voxelCount);
                cov[2][2] = sumZZ / region.voxelCount - (sumZ / region.voxelCount) * (sumZ / region.voxelCount);
                cov[0][1] = cov[1][0] = sumXY / region.voxelCount - (sumX / region.voxelCount) * (sumY / region.voxelCount);
                cov[0][2] = cov[2][0] = sumXZ / region.voxelCount - (sumX / region.voxelCount) * (sumZ / region.voxelCount);
                cov[1][2] = cov[2][1] = sumYZ / region.voxelCount - (sumY / region.voxelCount) * (sumZ / region.voxelCount);

                double* covPtr[3] = { cov[0], cov[1], cov[2] };
                double eigenVals[3], eigenVecs[3][3];
                double* vecPtr[3] = { eigenVecs[0], eigenVecs[1], eigenVecs[2] };
                vtkMath::Jacobi(covPtr, eigenVals, vecPtr);

                // 排序特征值 (vtkMath::Jacobi 已经按降序排序)
                region.pcaAxes = { eigenVals[0], eigenVals[1], eigenVals[2] };
                if (eigenVals[0] > 1e-9) {
                    region.elongation = std::sqrt(std::max(0.0, eigenVals[1] / eigenVals[0]));
                    if (eigenVals[1] > 1e-9)
                        region.flatness = std::sqrt(std::max(0.0, eigenVals[2] / eigenVals[1]));
                    region.pcaDeviation1 = eigenVals[0] / (eigenVals[0] + eigenVals[1] + eigenVals[2]);
                    region.pcaMaxDeviationRatio = (eigenVals[2] > 1e-9) ? (eigenVals[0] / eigenVals[2]) : 0.0;
                }

                // 6. 投影面积与表面积 (13方向)
                std::vector<uint8_t> projXY((size_t)(region.bbox[1] - region.bbox[0] + 1) * (region.bbox[3] - region.bbox[2] + 1), 0);
                std::vector<uint8_t> projXZ((size_t)(region.bbox[1] - region.bbox[0] + 1) * (region.bbox[5] - region.bbox[4] + 1), 0);
                std::vector<uint8_t> projYZ((size_t)(region.bbox[3] - region.bbox[2] + 1) * (region.bbox[5] - region.bbox[4] + 1), 0);

                int w = region.bbox[1] - region.bbox[0] + 1;
                int h = region.bbox[3] - region.bbox[2] + 1;
                int d = region.bbox[5] - region.bbox[4] + 1;

                size_t crossCount13 = 0;
                std::vector<float> dists;
                std::vector<size_t> boundaryVoxels;

                for (size_t vIdx : regionVoxels) {
                    int cz = (int)(vIdx / slice);
                    int cy = (int)((vIdx / dx) % dy);
                    int cx = (int)(vIdx % dx);

                    projXY[(size_t)(cx - region.bbox[0]) + (size_t)(cy - region.bbox[2]) * w] = 1;
                    projXZ[(size_t)(cx - region.bbox[0]) + (size_t)(cz - region.bbox[4]) * w] = 1;
                    projYZ[(size_t)(cy - region.bbox[2]) + (size_t)(cz - region.bbox[4]) * h] = 1;

                    bool isBoundary = false;
                    for (const auto& dir : directions13) {
                        int nx = cx + dir[0]; int ny = cy + dir[1]; int nz = cz + dir[2];
                        if (nx < 0 || ny < 0 || nz < 0 || nx >= dx || ny >= dy || nz >= dz) {
                            crossCount13++; isBoundary = true;
                        }
                        else {
                            size_t nIdx = (size_t)nx + (size_t)ny * dx + (size_t)nz * slice;
                            if (outLabelVol[nIdx] != currentID) { crossCount13++; isBoundary = true; }
                        }
                        // 反向也要查，或者遍历完后乘以2，这里我们查全部13个方向的邻居
                        int mx = cx - dir[0]; int my = cy - dir[1]; int mz = cz - dir[2];
                        if (mx < 0 || my < 0 || mz < 0 || mx >= dx || my >= dy || mz >= dz) {
                            crossCount13++; isBoundary = true;
                        }
                        else {
                            size_t mIdx = (size_t)mx + (size_t)my * dx + (size_t)mz * slice;
                            if (outLabelVol[mIdx] != currentID) { crossCount13++; isBoundary = true; }
                        }
                    }
                    if (isBoundary) boundaryVoxels.push_back(vIdx);
                }

                size_t cXY = 0; for (uint8_t v : projXY) if (v) cXY++;
                size_t cXZ = 0; for (uint8_t v : projXZ) if (v) cXZ++;
                size_t cYZ = 0; for (uint8_t v : projYZ) if (v) cYZ++;

                region.projectedAreaXYMM2 = cXY * vol.spacing[0] * vol.spacing[1];
                region.projectedAreaXZMM2 = cXZ * vol.spacing[0] * vol.spacing[2];
                region.projectedAreaYZMM2 = cYZ * vol.spacing[1] * vol.spacing[2];

                // 表面积估算 (简化的13方向权重)
                // 为简化，使用平均投影面积权重
                double avgCrossArea = (vol.spacing[0] * vol.spacing[1] + vol.spacing[0] * vol.spacing[2] + vol.spacing[1] * vol.spacing[2]) / 3.0;
                region.surfaceAreaMM2 = (double)crossCount13 * avgCrossArea / 13.0;

                // 7. Compactness & Sphericity
                if (region.surfaceAreaMM2 > 1e-9) {
                    region.compactness = (36.0 * pi * region.volumeMM3 * region.volumeMM3) / std::pow(region.surfaceAreaMM2, 3.0);
                    region.sphericity = std::pow(std::max(0.0, region.compactness), 1.0 / 3.0);
                }

                // 8. Gap (Characteristic thickness)
                // 粗略估计：使用体积/表面积比率 (V/S) 的两倍
                if (region.surfaceAreaMM2 > 1e-9)
                    region.gapMM = 2.0 * (region.volumeMM3 / region.surfaceAreaMM2);

                regions.push_back(region);
                currentID++;
            }
            else
            {
                for (size_t vIdx : regionVoxels) {
                    outLabelVol[vIdx] = 0;
                }
            }

        }
    }

    return regions;
}
