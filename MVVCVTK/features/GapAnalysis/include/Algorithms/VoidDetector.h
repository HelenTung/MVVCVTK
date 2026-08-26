#pragma once
// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/include/Algorithms/VoidDetector.h
// VoidDetector.h — 空洞检测纯算法层
// =====================================================================

#include "VolumeBuffer.h"
#include "GapAnalysisTypes.h"
#include <vtkSMPTools.h>
#include <vtkMath.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <queue>
#include <vector>

// 空洞分析的纯 CPU 流水线；不持有 VTK/服务状态，所有中间 mask 都与输入体素采用
// x-fast 布局 `x + y*dimX + z*dimX*dimY`。三个公开步骤必须按顺序消费彼此产物。
class VoidDetector {
public:
    // ── Step 1：从体积六个边界做 6 邻域泛洪；返回 x-fast uint8 mask，1 表示未连通外界的 sub-ISO voxel ──
    static std::vector<uint8_t> CreateInteriorMask(
        const GapVolumeBuffer& vol,
        float isoValue,
        const std::atomic<bool>* stopState = nullptr);

    // ── Step 2：在内部 mask 上应用 grayMax 与六邻域腐蚀，再从幸存种子回长原始候选 ──
    // 当前只消费 grayMax 与 erosionIterations；grayMin、角度和张量窗口不参与本阶段。
    static std::vector<uint8_t> BuildCandidates(
        const GapVolumeBuffer& vol,
        const std::vector<uint8_t>& interiorMask,
        const GapVoidParams& params,
        const std::atomic<bool>* stopState = nullptr);

    // ── Step 3：按 6 邻域生成连通区域、统计与 x-fast 标签体 ─────────
    // outLabelVol 会被重建为 dims 乘积个元素；0 为未保留 voxel，正值与返回区域 id 对应。
    // 当前只消费 minVolumeMM3；未执行结构张量或角度合并。
    static std::vector<VoidRegion> BuildRegions(
        const GapVolumeBuffer& vol,
        std::vector<uint8_t>& candidateMask,
        const GapVoidParams& params,
        std::vector<int>& outLabelVol,
        const std::atomic<bool>* stopState = nullptr);

private:
    struct VolumeSize final {
        std::size_t slice = 0;
        std::size_t total = 0;
    };

    struct NeighborSet final {
        std::array<std::size_t, 6> indices{};
        std::size_t count = 0;
    };

    static NeighborSet GetNeighbors(
        std::size_t index,
        const std::array<int, 3>& dims) noexcept;
    static bool GetVolumeSize(
        const std::array<int, 3>& dims,
        VolumeSize& size) noexcept;
    static bool GetPlaneSize(
        std::size_t first,
        std::size_t second,
        std::size_t& size) noexcept;
    static bool GetValidSpacing(
        const GapVolumeBuffer& vol,
        const VolumeSize& size,
        double& voxelVolume) noexcept;
    static bool GetIsStopped(
        const std::atomic<bool>* stopState) noexcept;
};

// ─────────────────────────────────────────────────────────────────────

inline bool VoidDetector::GetPlaneSize(
    const std::size_t first,
    const std::size_t second,
    std::size_t& size) noexcept
{
    size = 0;
    if (first == 0 || second == 0
        || first > std::numeric_limits<std::size_t>::max() / second) {
        return false;
    }
    size = first * second;
    return true;
}

inline bool VoidDetector::GetIsStopped(
    const std::atomic<bool>* stopState) noexcept
{
    return stopState
        && stopState->load(std::memory_order_relaxed);
}

inline bool VoidDetector::GetVolumeSize(
    const std::array<int, 3>& dims,
    VolumeSize& size) noexcept
{
    size = {};
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        return false;
    }
    const auto dimX = static_cast<std::size_t>(dims[0]);
    const auto dimY = static_cast<std::size_t>(dims[1]);
    const auto dimZ = static_cast<std::size_t>(dims[2]);
    if (!GetPlaneSize(dimX, dimY, size.slice)
        || size.slice
            > std::numeric_limits<std::size_t>::max() / dimZ) {
        size = {};
        return false;
    }
    size.total = size.slice * dimZ;
    if (size.total > static_cast<std::size_t>(
            std::numeric_limits<vtkIdType>::max())) {
        size = {};
        return false;
    }
    return true;
}

inline bool VoidDetector::GetValidSpacing(
    const GapVolumeBuffer& vol,
    const VolumeSize& size,
    double& voxelVolume) noexcept
{
    voxelVolume = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(vol.spacing[axis])
            || vol.spacing[axis] <= 0.0) {
            return false;
        }
        const double span = static_cast<double>(
            vol.dims[axis] - 1) * vol.spacing[axis];
        if (!std::isfinite(span)) return false;
    }
    voxelVolume = vol.spacing[0]
        * vol.spacing[1] * vol.spacing[2];
    return std::isfinite(voxelVolume)
        && voxelVolume > 0.0
        && std::isfinite(
            static_cast<double>(size.total) * voxelVolume);
}

inline VoidDetector::NeighborSet VoidDetector::GetNeighbors(
    const std::size_t index,
    const std::array<int, 3>& dims) noexcept
{
    NeighborSet neighbors;
    VolumeSize size;
    if (!GetVolumeSize(dims, size)) return neighbors;
    const auto dimX = static_cast<std::size_t>(dims[0]);
    const auto dimY = static_cast<std::size_t>(dims[1]);
    const auto dimZ = static_cast<std::size_t>(dims[2]);
    if (index >= size.total) {
        return neighbors;
    }

    const auto x = index % dimX;
    const auto y = (index / dimX) % dimY;
    const auto z = index / size.slice;
    const auto add = [&neighbors](const std::size_t neighbor) {
        neighbors.indices[neighbors.count++] = neighbor;
    };

    if (x > 0) add(index - 1);
    if (x + 1 < dimX) add(index + 1);
    if (y > 0) add(index - dimX);
    if (y + 1 < dimY) add(index + dimX);
    if (z > 0) add(index - size.slice);
    if (z + 1 < dimZ) add(index + size.slice);
    return neighbors;
}

inline std::vector<uint8_t> VoidDetector::CreateInteriorMask(
    const GapVolumeBuffer& vol,
    const float isoValue,
    const std::atomic<bool>* stopState)
{
    // 路径：六个体边界的 sub-ISO voxel 入队 -> 6 邻域泛洪标记 exterior ->
    // 反转语义，仅保留“低于 iso 且无法连通边界”的内部空隙。
    const int    dx = vol.dims[0];
    const int    dy = vol.dims[1];
    const int    dz = vol.dims[2];

    VolumeSize size;
    if (!vol.voxelsPtr || !GetVolumeSize(vol.dims, size)
        || GetIsStopped(stopState)) {
        return {};
    }
    const size_t slice = size.slice;
    const size_t total = size.total;
    const float* data = vol.voxelsPtr;

    std::vector<uint8_t> exterior(total, 0);

    std::deque<std::size_t> queue;

    const auto getOpen = [&](size_t idx) {
        return !vol.GetVoxelValid(idx) || data[idx] < isoValue;
    };
    auto pushNode = [&](int x, int y, int z) {
        size_t idx = (size_t)x + (size_t)y * dx + (size_t)z * slice;
        if (getOpen(idx) && exterior[idx] == 0) {
            exterior[idx] = 1;
            queue.push_back(idx);
        }
    };

    // 1. mask=0 表示分析域外；每个无效 voxel 都是 exterior 种子，使与裁切边界相邻的
    // 低灰度有效 voxel 能连通域外，而不会被误判为封闭孔隙。
    for (int z = 0; z < dz; ++z) {
        if (GetIsStopped(stopState)) return {};
        for (int y = 0; y < dy; ++y) {
            for (int x = 0; x < dx; ++x) {
                const size_t idx = (size_t)x
                    + (size_t)y * dx
                    + (size_t)z * slice;
                if (!vol.GetVoxelValid(idx)) {
                    pushNode(x, y, z);
                }
            }
        }
    }

    // 2. 六个面分三组扫描；边/角允许重复调用，但 exterior 门铃保证只入队一次。
    for (int y = 0; y < dy; ++y) {
        if (GetIsStopped(stopState)) return {};
        for (int x = 0; x < dx; ++x) {
            pushNode(x, y, 0);
            pushNode(x, y, dz - 1);
        }
    }
    for (int z = 1; z < dz - 1; ++z) {
        if (GetIsStopped(stopState)) return {};
        for (int y = 0; y < dy; ++y) {
            pushNode(0, y, z);
            pushNode(dx - 1, y, z);
        }
    }
    for (int z = 1; z < dz - 1; ++z) {
        if (GetIsStopped(stopState)) return {};
        for (int x = 1; x < dx - 1; ++x) {
            pushNode(x, 0, z);
            pushNode(x, dy - 1, z);
        }
    }

    while (!queue.empty() && !GetIsStopped(stopState)) {
        const auto current = queue.front();
        queue.pop_front();
        const auto neighbors = GetNeighbors(current, vol.dims);
        for (std::size_t offset = 0;
            offset < neighbors.count; ++offset) {
            const auto neighbor = neighbors.indices[offset];
            if (exterior[neighbor] == 0 && getOpen(neighbor)) {
                exterior[neighbor] = 1;
                queue.push_back(neighbor);
            }
        }
    }
    if (GetIsStopped(stopState)) return {};

    for (std::size_t i = 0; i < total; ++i) {
        if (GetIsStopped(stopState)) return {};
        if (vol.GetVoxelValid(i)
            && data[i] < isoValue
            && exterior[i] == 0) {
            exterior[i] = 1; // 内部孔隙
        }
        else {
            exterior[i] = 0; // 固体或外部空气
        }
    }

    return exterior;
}

inline std::vector<uint8_t> VoidDetector::BuildCandidates(
    const GapVolumeBuffer& vol,
    const std::vector<uint8_t>& interiorMask,
    const GapVoidParams& params,
    const std::atomic<bool>* stopState)
{
    // 路径：interior 与 grayMax 求交 -> N 轮六邻域腐蚀得到稳定种子 ->
    // 沿原始 raw mask 回长，恢复与稳定种子连通的完整候选区域。
    VolumeSize size;
    if (!vol.voxelsPtr || !GetVolumeSize(vol.dims, size)
        || GetIsStopped(stopState)) {
        return {};
    }
    const size_t total = size.total;
    if (interiorMask.size() != total) {
        return {};
    }

    std::vector<uint8_t> rawMask(total, 0);

    vtkSMPTools::For(0, static_cast<vtkIdType>(total),
        [&](vtkIdType begin, vtkIdType end) {
            for (vtkIdType i = begin; i < end; ++i) {
                if (GetIsStopped(stopState)) return;
                if (vol.GetVoxelValid(static_cast<size_t>(i))
                    && interiorMask[i] > 0
                    && vol.voxelsPtr[i] <= params.grayMax) {
                    rawMask[i] = 1;
                }
            }
        });
    if (GetIsStopped(stopState)) return {};

    const int erosionIterations = params.erosionIterations;
    std::vector<uint8_t> eroded = rawMask;
    std::vector<uint8_t> nextEroded(total, 0);

    for (int iter = 0; iter < erosionIterations; ++iter) {
        if (GetIsStopped(stopState)) return {};
        std::fill(nextEroded.begin(), nextEroded.end(), 0);
        vtkSMPTools::For(0, static_cast<vtkIdType>(total),
            [&](vtkIdType begin, vtkIdType end) {
                for (vtkIdType value = begin; value < end; ++value) {
                    if (GetIsStopped(stopState)) return;
                    const auto index = static_cast<std::size_t>(value);
                    if (!eroded[index]) {
                        continue;
                    }
                    const auto neighbors = GetNeighbors(index, vol.dims);
                    if (neighbors.count != 6) {
                        continue;
                    }
                    bool isKept = true;
                    for (std::size_t offset = 0;
                        offset < neighbors.count; ++offset) {
                        if (!eroded[neighbors.indices[offset]]) {
                            isKept = false;
                            break;
                        }
                    }
                    nextEroded[index] = isKept ? 1 : 0;
                }
            });
        if (GetIsStopped(stopState)) return {};
        std::swap(eroded, nextEroded);
    }

    std::vector<uint8_t> candidates(total, 0);
    std::queue<size_t> bfsQueue;

    for (size_t i = 0; i < total; ++i) {
        if (GetIsStopped(stopState)) return {};
        if (eroded[i]) {
            bfsQueue.push(i);
            candidates[i] = 1;
        }
    }

    while (!bfsQueue.empty() && !GetIsStopped(stopState)) {
        const auto current = bfsQueue.front();
        bfsQueue.pop();
        const auto neighbors = GetNeighbors(current, vol.dims);
        for (std::size_t offset = 0;
            offset < neighbors.count; ++offset) {
            const auto neighbor = neighbors.indices[offset];
            if (rawMask[neighbor] && !candidates[neighbor]) {
                candidates[neighbor] = 1;
                bfsQueue.push(neighbor);
            }
        }
    }
    if (GetIsStopped(stopState)) return {};
    return candidates;
}

inline std::vector<VoidRegion> VoidDetector::BuildRegions(
    const GapVolumeBuffer& vol,
    std::vector<uint8_t>& candidateMask,
    const GapVoidParams& params,
    std::vector<int>& outLabelVol,
    const std::atomic<bool>* stopState)
{
    // 路径：扫描未标记候选 -> BFS 收集单区体素并累计一/二阶矩 -> 按最小体积筛选 ->
    // 为保留区计算灰度、bbox、PCA、投影面积与近似表面积；被筛掉区的临时标签恢复为 0。
    const int dx = vol.dims[0];
    const int dy = vol.dims[1];
    const int dz = vol.dims[2];
    VolumeSize size;
    if (!vol.voxelsPtr || !GetVolumeSize(vol.dims, size)
        || GetIsStopped(stopState)) {
        outLabelVol.clear();
        return {};
    }
    const size_t slice = size.slice;
    const size_t total = size.total;
    if (candidateMask.size() != total) {
        outLabelVol.clear();
        return {};
    }

    double voxelVol = 0.0;
    if (!GetValidSpacing(vol, size, voxelVol)
        || !std::isfinite(params.minVolumeMM3)
        || params.minVolumeMM3 < 0.0) {
        outLabelVol.clear();
        return {};
    }
    outLabelVol.assign(total, 0);

    std::vector<VoidRegion> regions;
    int currentID = 1;

    // 13 个无符号方向及其反向共同形成 26 邻域穿越计数，只用于近似表面积，不改变 6 邻域连通标签。
    const std::vector<std::array<int, 3>> directions13 = {
        {1,0,0}, {0,1,0}, {0,0,1},
        {1,1,0}, {1,-1,0}, {1,0,1}, {1,0,-1}, {0,1,1}, {0,1,-1},
        {1,1,1}, {1,1,-1}, {1,-1,1}, {1,-1,-1}
    };

    std::queue<size_t> q;

    for (size_t i = 0; i < total; ++i) {
        if (GetIsStopped(stopState)) {
            outLabelVol.clear();
            return {};
        }
        if (vol.GetVoxelValid(i)
            && candidateMask[i] > 0
            && outLabelVol[i] == 0) {

            if (currentID == std::numeric_limits<int>::max()) {
                outLabelVol.clear();
                return {};
            }

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

            while (!q.empty() && !GetIsStopped(stopState)) {
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

                const auto neighbors = GetNeighbors(curr, vol.dims);
                for (std::size_t offset = 0;
                    offset < neighbors.count; ++offset) {
                    const auto neighbor = neighbors.indices[offset];
                    if (vol.GetVoxelValid(neighbor)
                        && candidateMask[neighbor] > 0
                        && outLabelVol[neighbor] == 0) {
                        outLabelVol[neighbor] = currentID;
                        q.push(neighbor);
                    }
                }
            }

            region.volumeMM3 = static_cast<double>(
                region.voxelCount) * voxelVol;
            if (!std::isfinite(region.volumeMM3)) {
                outLabelVol.clear();
                return {};
            }
            if (GetIsStopped(stopState)) {
                outLabelVol.clear();
                return {};
            }

            if (region.volumeMM3 >= params.minVolumeMM3) {
                // 只有达到阈值的区域才消费 currentID；这样 regions、labelVolume 正标签与 region.id 保持一一对应。
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

                // 5. PCA：协方差在无 origin 的 physical offset 上计算；平移不改变特征值。
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

                // vtkMath::Jacobi 返回降序特征值，单位为 mm^2；这里保存特征值而不是主轴长度。
                region.pcaAxes = { eigenVals[0], eigenVals[1], eigenVals[2] };
                if (eigenVals[0] > 1e-9) {
                    region.elongation = std::sqrt(std::max(0.0, eigenVals[1] / eigenVals[0]));
                    if (eigenVals[1] > 1e-9)
                        region.flatness = std::sqrt(std::max(0.0, eigenVals[2] / eigenVals[1]));
                    region.pcaDeviation1 = eigenVals[0] / (eigenVals[0] + eigenVals[1] + eigenVals[2]);
                    region.pcaMaxDeviationRatio = (eigenVals[2] > 1e-9) ? (eigenVals[0] / eigenVals[2]) : 0.0;
                }

                // 6. 三个轴向占据投影面积，以及 13 方向边界穿越的近似表面积。
                const auto w = static_cast<std::size_t>(
                    static_cast<std::uint64_t>(region.bbox[1])
                    - static_cast<std::uint64_t>(region.bbox[0]) + 1U);
                const auto h = static_cast<std::size_t>(
                    static_cast<std::uint64_t>(region.bbox[3])
                    - static_cast<std::uint64_t>(region.bbox[2]) + 1U);
                const auto d = static_cast<std::size_t>(
                    static_cast<std::uint64_t>(region.bbox[5])
                    - static_cast<std::uint64_t>(region.bbox[4]) + 1U);
                std::size_t xySize = 0;
                std::size_t xzSize = 0;
                std::size_t yzSize = 0;
                if (!GetPlaneSize(
                        w,
                        h,
                        xySize)
                    || !GetPlaneSize(
                        w,
                        d,
                        xzSize)
                    || !GetPlaneSize(
                        h,
                        d,
                        yzSize)) {
                    outLabelVol.clear();
                    return {};
                }
                std::vector<uint8_t> projXY(xySize, 0);
                std::vector<uint8_t> projXZ(xzSize, 0);
                std::vector<uint8_t> projYZ(yzSize, 0);

                size_t crossCount13 = 0;
                std::vector<float> dists;
                std::vector<size_t> boundaryVoxels;
                const auto setCross = [&crossCount13]() {
                    if (crossCount13
                        == std::numeric_limits<std::size_t>::max()) {
                        return false;
                    }
                    ++crossCount13;
                    return true;
                };

                // boundaryVoxels 当前只记录诊断性边界集合，后续公式仅使用 crossCount13；
                // dists 同样未进入现有统计，不能据此声称已计算边界距离分布。

                for (size_t vIdx : regionVoxels) {
                    if (GetIsStopped(stopState)) {
                        outLabelVol.clear();
                        return {};
                    }
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
                            if (!setCross()) {
                                outLabelVol.clear();
                                return {};
                            }
                            isBoundary = true;
                        }
                        else {
                            size_t nIdx = (size_t)nx + (size_t)ny * dx + (size_t)nz * slice;
                            if (outLabelVol[nIdx] != currentID) {
                                if (!setCross()) {
                                    outLabelVol.clear();
                                    return {};
                                }
                                isBoundary = true;
                            }
                        }
                        // 反向也要查，或者遍历完后乘以2，这里我们查全部13个方向的邻居
                        int mx = cx - dir[0]; int my = cy - dir[1]; int mz = cz - dir[2];
                        if (mx < 0 || my < 0 || mz < 0 || mx >= dx || my >= dy || mz >= dz) {
                            if (!setCross()) {
                                outLabelVol.clear();
                                return {};
                            }
                            isBoundary = true;
                        }
                        else {
                            size_t mIdx = (size_t)mx + (size_t)my * dx + (size_t)mz * slice;
                            if (outLabelVol[mIdx] != currentID) {
                                if (!setCross()) {
                                    outLabelVol.clear();
                                    return {};
                                }
                                isBoundary = true;
                            }
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
                if (!std::isfinite(region.projectedAreaXYMM2)
                    || !std::isfinite(region.projectedAreaXZMM2)
                    || !std::isfinite(region.projectedAreaYZMM2)
                    || !std::isfinite(region.surfaceAreaMM2)) {
                    outLabelVol.clear();
                    return {};
                }

                // 7. Compactness & Sphericity
                if (region.surfaceAreaMM2 > 1e-9) {
                    region.compactness = (36.0 * pi * region.volumeMM3 * region.volumeMM3) / std::pow(region.surfaceAreaMM2, 3.0);
                    region.sphericity = std::pow(std::max(0.0, region.compactness), 1.0 / 3.0);
                }

                // 8. Gap (Characteristic thickness)
                // 粗略估计：使用体积/表面积比率 (V/S) 的两倍
                if (region.surfaceAreaMM2 > 1e-9)
                    region.gapMM = 2.0 * (region.volumeMM3 / region.surfaceAreaMM2);

                if (!std::isfinite(region.compactness)
                    || !std::isfinite(region.sphericity)
                    || !std::isfinite(region.gapMM)) {
                    outLabelVol.clear();
                    return {};
                }

                regions.push_back(region);
                currentID++;
            }
            else
            {
                // 小区域在 BFS 时已临时写入 currentID；筛除时必须逐 voxel 回滚，且 currentID 不递增。
                for (size_t vIdx : regionVoxels) {
                    outLabelVol[vIdx] = 0;
                }
            }

        }
    }

    return regions;
}
