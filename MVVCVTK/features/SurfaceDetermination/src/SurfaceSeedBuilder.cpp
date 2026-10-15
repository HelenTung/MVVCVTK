#include "FeatureSupport/WorkLimit.h"
#include "SurfaceSeedBuilder.h"

#include <vtkMarchingCubesTriangleCases.h>
#include <vtkType.h>
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <type_traits>
#include <cstring>
#include <Windows.h>

#ifndef MVVCVTK_SURFACE_SEED_CACHE_BYTES
#define MVVCVTK_SURFACE_SEED_CACHE_BYTES 0
#endif

namespace
{
using Point = std::array<double, 3>;
constexpr int corners[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                               {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
// 与锁定 VTK 9.4.2 MC case 表一致的边编号，10/11 的顺序不可套其他库。
constexpr int edges[12][2] = {{0, 1}, {1, 2}, {3, 2}, {0, 3}, {4, 5}, {5, 6},
                              {7, 6}, {4, 7}, {0, 4}, {1, 5}, {3, 7}, {2, 6}};

Point ToModel(const SurfaceSeedGrid &grid, const Point &index)
{
    Point point = grid.origin;
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c)
            point[r] += grid.indexToModel[r * 3 + c] * index[c];
    return point;
}
Point ToIndex(const SurfaceSeedGrid &grid, const Point &point)
{
    Point index{};
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c)
            index[r] += grid.modelToIndex[r * 3 + c] * (point[c] - grid.origin[c]);
    return index;
}
std::size_t GetIndex(const SurfaceSeedGrid &grid, const int x, const int y, const int z)
{
    return (static_cast<std::size_t>(z - grid.extent[4]) * grid.dimensions[1] + (y - grid.extent[2])) *
               grid.dimensions[0] +
           (x - grid.extent[0]);
}
std::optional<std::size_t> GetWorkingEstimate(const std::size_t pointCount,
                                            const std::size_t triangleCount,
                                            const std::size_t extraBytes = 0)
{
    // 同时覆盖 edge/clip 映射、组件、原/新网格、records 和发布载荷的保守估算。
    constexpr std::size_t perPoint = 8 * sizeof(Point) + 4 * sizeof(SurfacePointRecord) + 256;
    constexpr std::size_t perTriangle = 8 * sizeof(SurfaceSeedTriangle) + 256;
    constexpr std::size_t fixed = 64 * 1024;
    if (extraBytes > SIZE_MAX - fixed) return {};
    const auto base = fixed + extraBytes;
    if (pointCount > UINT32_MAX || pointCount > (SIZE_MAX - base) / perPoint ||
        triangleCount > (SIZE_MAX - base - pointCount * perPoint) / perTriangle) return {};
    // 固定窗口覆盖 seed/组件/法向/输出文件和排序工作区；逻辑文件字节不作为 RAM 计费。
    return base + std::min(pointCount*perPoint,std::size_t(192)*1024*1024)
        + std::min(triangleCount*perTriangle,std::size_t(96)*1024*1024);
}
bool GetBudget(const std::size_t pointCount, const std::size_t triangleCount, const WorkLimit budget,
               SurfaceExecutionStats &stats)
{
    (void)budget;
    const auto estimate = GetWorkingEstimate(pointCount, triangleCount);
    if (!estimate) return false;
    stats.estimatedWorkingBytes = std::max(stats.estimatedWorkingBytes, *estimate);
    // 后备数组的逻辑总量不是 RAM 工作区；索引/字节溢出仍在上面拒绝。
    // 目标工作集只影响可重建缓存，不拒绝可表示的完整结果。
    return true;
}

SurfaceSeedStatus ClipMesh(const std::vector<RoiPlane> &planes, const WorkLimit budget,
                           const std::function<bool()> &cancelled, PagedArray<Point> &points,
                           PagedArray<SurfaceSeedTriangle> &triangles, SurfaceExecutionStats &stats)
{
    PagedMap<std::array<std::uint32_t,3>,std::uint32_t,PagedArrayHash<std::uint32_t,3>> intersections;
    PagedArray<SurfaceSeedTriangle> clipped;
    std::vector<std::uint32_t> polygon, next;
    polygon.reserve(10);
    next.reserve(10);
    for (std::size_t t = 0; t < triangles.size(); ++t)
    {
        if ((t & 255U) == 0 && cancelled && cancelled())
            return SurfaceSeedStatus::Cancelled;
        polygon.assign(triangles[t].vertices.begin(), triangles[t].vertices.end());
        for (unsigned plane = 0; plane < planes.size() && polygon.size() >= 3; ++plane)
        {
            const auto& clipping = planes[plane];
            const auto distance = [&](std::uint32_t id) {
                double value = 0;
                for (unsigned axis = 0; axis < 3; ++axis)
                    value += (points[id][axis] - clipping.origin[axis]) * clipping.normal[axis];
                return value;
            };
            next.clear();
            for (std::size_t i = 0; i < polygon.size(); ++i)
            {
                const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
                const auto da = distance(a), db = distance(b);
                if (da >= 0)
                    next.push_back(a);
                if ((da < 0) == (db < 0) || da == 0 || db == 0)
                    continue;
                const auto low = std::min(a, b), high = std::max(a, b);
                const std::array<std::uint32_t,3> key{low,high,plane};
                auto found = intersections.Find(key);
                if (!found)
                {
                    if (!GetBudget(points.size() + 1, triangles.size() + clipped.size(), budget, stats))
                        return SurfaceSeedStatus::BudgetExceeded;
                    // canonical low/high 的端点和浮点表达式不变，复用当前边已算出的距离。
                    const double dLow = low==a?da:db, dHigh = high==b?db:da;
                    const double fraction = dLow / (dLow - dHigh);
                    Point point{};
                    for (unsigned c = 0; c < 3; ++c)
                        point[c] = points[low][c] + fraction * (points[high][c] - points[low][c]);
                    double residual = 0, normal2 = 0;
                    for (unsigned c = 0; c < 3; ++c) {
                        residual += (point[c] - clipping.origin[c]) * clipping.normal[c];
                        normal2 += clipping.normal[c] * clipping.normal[c];
                    }
                    if (!std::isfinite(normal2) || normal2 <= 0) return SurfaceSeedStatus::InvalidInput;
                    for (unsigned c = 0; c < 3; ++c)
                        point[c] -= residual * clipping.normal[c] / normal2;
                    const auto id = static_cast<std::uint32_t>(points.size());
                    points.push_back(point);
                    found = intersections.Insert(key, id);
                }
                next.push_back(*found);
            }
            polygon.swap(next);
        }
        for (std::size_t i = 1; i + 1 < polygon.size(); ++i)
        {
            if (polygon[0] == polygon[i] || polygon[i] == polygon[i + 1] || polygon[0] == polygon[i + 1])
                continue;
            if (!GetBudget(points.size(), triangles.size() + clipped.size() + 1, budget, stats))
                return SurfaceSeedStatus::BudgetExceeded;
            clipped.push_back({{polygon[0], polygon[i], polygon[i + 1]}});
        }
    }
    triangles = std::move(clipped);
    return triangles.empty() ? SurfaceSeedStatus::NoSurface : SurfaceSeedStatus::Succeeded;
}
template <bool HasMask, class ScalarReader>
SurfaceSeedStatus BuildSeedMesh(
    const SurfaceSeedGrid &grid, const double iso,
    const RoiReadSnapshot &roi, const double haloModel, const std::uint32_t blockDepth,
    const WorkLimit budget, const std::function<bool()> &cancelled, PagedArray<Point> &points,
    PagedArray<SurfaceSeedTriangle> &triangles, SurfaceExecutionStats &stats,
    const ScalarReader &readScalar)
{
    const auto started = std::chrono::steady_clock::now();
    if ((!grid.scalarValues && (!grid.values || !grid.readScalar)) ||
        !blockDepth || !points.empty() || !triangles.empty())
        return SurfaceSeedStatus::InvalidInput;
    if (!GetBudget(0, 0, budget, stats))
        return SurfaceSeedStatus::BudgetExceeded;
    std::vector<RoiPlane> planes;
    if (roi) {
        auto clip = roi->GetClipPlanes();
        if (clip.error != RoiError::None) return SurfaceSeedStatus::InvalidInput;
        planes = std::move(clip.planes);
    }
    std::array<int, 6> range = grid.extent;
    if (roi)
    {
        const auto roiBounds = roi->GetBounds();
        std::array<double, 6> bounds{DBL_MAX, -DBL_MAX, DBL_MAX, -DBL_MAX, DBL_MAX, -DBL_MAX};
        for (unsigned i = 0; i < 8; ++i)
        {
            const auto index =
                ToIndex(grid, {roiBounds[(i & 1) ? 1 : 0], roiBounds[(i & 2) ? 3 : 2], roiBounds[(i & 4) ? 5 : 4]});
            for (unsigned a = 0; a < 3; ++a)
            {
                bounds[a * 2] = std::min(bounds[a * 2], index[a]);
                bounds[a * 2 + 1] = std::max(bounds[a * 2 + 1], index[a]);
            }
        }
        for (unsigned a = 0; a < 3; ++a)
        {
            const double norm = std::hypot(grid.modelToIndex[a * 3], grid.modelToIndex[a * 3 + 1],
                                           grid.modelToIndex[a * 3 + 2]);
            const double halo = std::ceil(norm * haloModel) + 2;
            const auto low = std::max(double(grid.extent[a * 2]), std::floor(bounds[a * 2] - halo));
            const auto high = std::min(double(grid.extent[a * 2 + 1]), std::ceil(bounds[a * 2 + 1] + halo));
            if (!std::isfinite(low) || !std::isfinite(high) || low >= high)
                return SurfaceSeedStatus::NoSurface;
            range[a * 2] = static_cast<int>(low);
            range[a * 2 + 1] = static_cast<int>(high);
        }
    }
    stats.processedExtent = range;
    const auto nodeWidth = std::size_t(std::int64_t(range[1]) - range[0] + 1);
    const auto nodeHeight = std::size_t(std::int64_t(range[3]) - range[2] + 1);
    std::array<std::vector<unsigned char>, 2> nodeTags;
    std::size_t cacheBytes = 0;
    char setting[8]{};
    const bool disabled = GetEnvironmentVariableA("MVVCVTK_SURFACE_SEED_CACHE", setting, sizeof(setting)) == 1 &&
                          setting[0] == '0';
    const auto cacheLimit = std::min<std::size_t>(MVVCVTK_SURFACE_SEED_CACHE_BYTES,
                                                 grid.classificationCacheLimit);
    if (grid.scalarValues && !disabled && nodeWidth <= SIZE_MAX / nodeHeight &&
        nodeWidth * nodeHeight <= cacheLimit / 2) {
        const auto bytes = nodeWidth * nodeHeight * 2;
        const auto estimate = GetWorkingEstimate(0, 0, bytes);
        if (estimate && *estimate <= budget) {
            try {
                if (grid.cacheAllocationCheckpoint) grid.cacheAllocationCheckpoint(bytes / 2);
                nodeTags[0].resize(bytes / 2);
                stats.estimatedWorkingBytes = std::max(stats.estimatedWorkingBytes,
                                                      *GetWorkingEstimate(0, 0, bytes / 2));
                if (grid.cacheAllocationCheckpoint) grid.cacheAllocationCheckpoint(bytes / 2);
                nodeTags[1].resize(bytes / 2);
                cacheBytes = bytes;
                stats.estimatedWorkingBytes = std::max(stats.estimatedWorkingBytes, *estimate);
                if (grid.audit) grid.audit->classificationCacheBytes = bytes;
            } catch (const std::bad_alloc &) {
                std::vector<unsigned char>().swap(nodeTags[0]);
                std::vector<unsigned char>().swap(nodeTags[1]);
            }
        }
    }
    const auto releaseCache = [&] {
        std::vector<unsigned char>().swap(nodeTags[0]);
        std::vector<unsigned char>().swap(nodeTags[1]);
        cacheBytes = 0;
    };
    const auto meshBudget = [&](std::size_t pointCount, std::size_t triangleCount) {
        if (cacheBytes) {
            const auto estimate = GetWorkingEstimate(pointCount, triangleCount, cacheBytes);
            if (estimate && (DataArrayStorage::GetCurrent() || *estimate <= budget)) {
                stats.estimatedWorkingBytes = std::max(stats.estimatedWorkingBytes, *estimate);
                return true;
            }
            // 可选缓存不能使原本可完成的网格因预算被拒绝；下一格恢复原读取窗口。
            releaseCache();
            if (grid.audit) ++grid.audit->budgetFallbackCount;
        }
        return GetBudget(pointCount, triangleCount, budget, stats);
    };
    const auto fillTags = [&](std::size_t layer, int z) {
        std::size_t checked = 0, offset = 0;
        for (std::int64_t y = range[2]; y <= range[3]; ++y) {
            if (cancelled && cancelled()) return false;
            for (std::int64_t x = range[0]; x <= range[1]; ++x, ++offset) {
                if ((checked++ & 1023U) == 0 && cancelled && cancelled()) return false;
                const auto index = GetIndex(grid, static_cast<int>(x), static_cast<int>(y), z);
                const auto value = readScalar(index);
                const bool invalid = (HasMask && !grid.validity[index]) || !std::isfinite(value);
                nodeTags[layer][offset] = static_cast<unsigned char>((value >= iso ? 1U : 0U) |
                                                                     (invalid ? 2U : 0U));
                if (grid.audit) ++grid.audit->classifiedNodeCount;
            }
        }
        return true;
    };
    const auto countUniformCells = [&](std::size_t count) {
        while (count) {
            const auto offset = stats.scannedCellCount & 1023U;
            if (!offset) {
                ++stats.scannedCellCount;
                --count;
                if (cancelled && cancelled()) return false;
            } else {
                const auto step = std::min<std::size_t>(count, 1024U - offset);
                stats.scannedCellCount += step;
                count -= step;
            }
        }
        return true;
    };
    {
        // 两层只做查找/插入，输出仍按原单元/MC 边顺序创建。
        std::array<PagedMap<std::uint64_t,std::uint32_t>,2> edgeLayers;
        const auto *cases = vtkMarchingCubesTriangleCases::GetCases();
        for (std::int64_t block = range[4]; block < range[5]; block += blockDepth)
        {
            ++stats.blockCount;
            const auto end = std::min<std::int64_t>(range[5], block + blockDepth);
            for (std::int64_t zz = block; zz < end; ++zz)
            {
                const auto lowerLayer=std::size_t(zz-range[4])&1U;
                const auto upperLayer=lowerLayer^1U;
                edgeLayers[upperLayer].clear();
                if (cacheBytes) {
                    if (zz == range[4] && !fillTags(lowerLayer, static_cast<int>(zz)))
                        return SurfaceSeedStatus::Cancelled;
                    if (!fillTags(upperLayer, static_cast<int>(zz + 1)))
                        return SurfaceSeedStatus::Cancelled;
                }
                for (std::int64_t yy = range[2]; yy < range[3]; ++yy)
                {
                    if (cancelled && cancelled())
                        return SurfaceSeedStatus::Cancelled;
                    std::array<double,8> scalar{};
                    unsigned invalidCorners = 0, code = 0;
                    bool scalarReady = false;
                    for (std::int64_t xx = range[0]; xx < range[1]; ++xx)
                    {
                        const bool shifted = xx != range[0];
                        if (cacheBytes) {
                            const auto x = std::size_t(xx - range[0]);
                            const auto y = std::size_t(yy - range[2]);
                            const auto base = y * nodeWidth + x;
                            // 每个 64 位 word 覆盖八节点；第九节点保证八个 cell 的右边界也同侧。
                            constexpr auto runCells = sizeof(std::uint64_t);
                            if (std::uint64_t(range[1] - xx) >= runCells) {
                                const auto *a = nodeTags[lowerLayer].data() + base;
                                const auto *b = a + nodeWidth;
                                const auto *c = nodeTags[upperLayer].data() + base;
                                const auto *d = c + nodeWidth;
                                const auto tag = a[0];
                                bool uniform = tag <= 1;
                                const auto pattern = tag ? UINT64_C(0x0101010101010101) : UINT64_C(0);
                                for (const auto *row : {a,b,c,d}) {
                                    std::uint64_t word = 0;
                                    std::memcpy(&word, row, sizeof(word));
                                    uniform = uniform && word == pattern && row[runCells] == tag;
                                }
                                if (uniform) {
                                    const auto before = stats.scannedCellCount;
                                    if (!countUniformCells(runCells)) return SurfaceSeedStatus::Cancelled;
                                    if (grid.audit) grid.audit->uniformCellCount += stats.scannedCellCount - before;
                                    code = tag ? 255U : 0U;
                                    invalidCorners = 0;
                                    scalarReady = false;
                                    xx += runCells - 1;
                                    continue;
                                }
                            }
                            if ((stats.scannedCellCount++ & 1023U) == 0 && cancelled && cancelled())
                                return SurfaceSeedStatus::Cancelled;
                            if (shifted) {
                                invalidCorners = ((invalidCorners & 0x22U) >> 1) |
                                                 ((invalidCorners & 0x44U) << 1);
                                code = ((code & 0x22U) >> 1) | ((code & 0x44U) << 1);
                            } else { invalidCorners = 0; code = 0; }
                            for (unsigned corner = 0; corner < 8; ++corner) {
                                if (shifted && (corner == 0 || corner == 3 || corner == 4 || corner == 7)) continue;
                                const auto layer = corners[corner][2] ? upperLayer : lowerLayer;
                                const auto tag = nodeTags[layer][base + corners[corner][1] * nodeWidth +
                                                                corners[corner][0]];
                                if (tag & 2U) invalidCorners |= 1U << corner;
                                if (tag & 1U) code |= 1U << corner;
                            }
                            if (invalidCorners || cases[code].edges[0] < 0) {
                                scalarReady = false;
                                if (invalidCorners) ++stats.skippedCellCount;
                                continue;
                            }
                            const bool reuse = scalarReady && shifted;
                            if (reuse) {
                                scalar[0]=scalar[1];scalar[3]=scalar[2];scalar[4]=scalar[5];scalar[7]=scalar[6];
                            }
                            for (unsigned corner = 0; corner < 8; ++corner) {
                                if (reuse && (corner == 0 || corner == 3 || corner == 4 || corner == 7)) continue;
                                scalar[corner] = readScalar(GetIndex(grid, static_cast<int>(xx) + corners[corner][0],
                                    static_cast<int>(yy) + corners[corner][1], static_cast<int>(zz) + corners[corner][2]));
                            }
                            scalarReady = true;
                        } else {
                            if ((stats.scannedCellCount++ & 1023U) == 0 && cancelled && cancelled())
                                return SurfaceSeedStatus::Cancelled;
                            const bool reuse = scalarReady && shifted;
                            if (reuse) {
                                scalar[0]=scalar[1];scalar[3]=scalar[2];scalar[4]=scalar[5];scalar[7]=scalar[6];
                                invalidCorners=((invalidCorners&0x22U)>>1)|((invalidCorners&0x44U)<<1);
                                code=((code&0x22U)>>1)|((code&0x44U)<<1);
                            } else { invalidCorners = 0; code = 0; }
                            for (unsigned corner = 0; corner < 8; ++corner) {
                                if (reuse && (corner == 0 || corner == 3 || corner == 4 || corner == 7)) continue;
                                const auto index = GetIndex(grid, static_cast<int>(xx) + corners[corner][0],
                                    static_cast<int>(yy) + corners[corner][1], static_cast<int>(zz) + corners[corner][2]);
                                scalar[corner] = readScalar(index);
                                if ((HasMask && !grid.validity[index]) || !std::isfinite(scalar[corner]))
                                    invalidCorners |= 1U << corner;
                                if (scalar[corner] >= iso) code |= 1U << corner;
                            }
                            scalarReady = true;
                        }
                        if (invalidCorners)
                        {
                            ++stats.skippedCellCount;
                            continue;
                        }
                        const auto *entries = cases[code].edges;
                        if (entries[0] < 0) continue;
                        std::array<std::uint32_t, 12> cellIds;
                        cellIds.fill(UINT32_MAX);
                        for (unsigned t = 0; t < 15 && entries[t] >= 0; t += 3)
                        {
                            SurfaceSeedTriangle triangle;
                            for (unsigned v = 0; v < 3; ++v)
                            {
                                const auto edgeId = entries[t + v];
                                if (cellIds[edgeId] != UINT32_MAX) {
                                    triangle.vertices[v] = cellIds[edgeId];
                                    continue;
                                }
                                const int a = edges[edgeId][0], b = edges[edgeId][1];
                                Point indexA{double(xx + corners[a][0]), double(yy + corners[a][1]),
                                             double(zz + corners[a][2])};
                                Point indexB{double(xx + corners[b][0]), double(yy + corners[b][1]),
                                             double(zz + corners[b][2])};
                                double fraction =
                                    (iso - scalar[a]) / (scalar[b] - scalar[a]);
                                fraction = std::clamp(fraction, 0.0, 1.0);
                                std::array<std::int64_t, 3> edgeOrigin{};
                                unsigned axis = 0;
                                for (unsigned d = 0; d < 3; ++d)
                                {
                                    edgeOrigin[d] = static_cast<std::int64_t>(std::min(indexA[d], indexB[d]));
                                    if (indexA[d] != indexB[d])
                                        axis = d;
                                }
                                if (fraction == 0 || fraction == 1)
                                {
                                    const auto &corner = fraction == 0 ? indexA : indexB;
                                    for (unsigned d = 0; d < 3; ++d)
                                        edgeOrigin[d] = static_cast<std::int64_t>(corner[d]);
                                    axis = 3;
                                }
                                // z 由缓存槽承担；XY 与轴/角点标签在已检查的范围内一一对应。
                                const auto xy = std::uint64_t(edgeOrigin[1] - grid.extent[2]) *
                                                    std::uint64_t(grid.dimensions[0]) +
                                                std::uint64_t(edgeOrigin[0] - grid.extent[0]);
                                const auto key = xy * 4U + axis;
                                auto& edgeIds=edgeLayers[edgeOrigin[2]==zz?lowerLayer:upperLayer];
                                auto found = edgeIds.Find(key);
                                if (!found)
                                {
                                    if (!meshBudget(points.size() + 1, triangles.size() + 1))
                                        return SurfaceSeedStatus::BudgetExceeded;
                                    Point index{};
                                    for (unsigned d = 0; d < 3; ++d)
                                        index[d] = indexA[d] + fraction * (indexB[d] - indexA[d]);
                                    const auto point = ToModel(grid, index);
                                    if (!std::all_of(point.begin(), point.end(),
                                                     [](double x) { return std::isfinite(x); }))
                                        return SurfaceSeedStatus::InvalidInput;
                                    const auto id = static_cast<std::uint32_t>(points.size());
                                    points.push_back(point);
                                    found = edgeIds.Insert(key, id);
                                }
                                cellIds[edgeId] = triangle.vertices[v] = *found;
                            }
                            if (triangle.vertices[0] != triangle.vertices[1] &&
                                triangle.vertices[1] != triangle.vertices[2] &&
                                triangle.vertices[0] != triangle.vertices[2])
                            {
                                if (!meshBudget(points.size(), triangles.size() + 1))
                                    return SurfaceSeedStatus::BudgetExceeded;
                                triangles.push_back(triangle);
                            }
                        }
                    }
                }
                // 上表面边/等阈值端点留到下一层，其余缓存在复用时清除。
            }
        }
    }
    releaseCache();
    if (roi)
    {
        const auto status = ClipMesh(planes, budget, cancelled, points, triangles, stats);
        if (status != SurfaceSeedStatus::Succeeded)
            return status;
    }
    // 删除未被最终 ROI 三角形引用的 seed 顶点，索引按首次生成顺序稳定压缩。
    PagedArray<std::uint32_t> remap(points.size(), UINT32_MAX);
    for (const auto &triangle : triangles)
        for (auto id : triangle.vertices)
            remap[id] = 0;
    std::size_t used = 0;
    for (std::size_t i = 0; i < points.size(); ++i)
        if (remap[i] != UINT32_MAX)
        {
            remap[i] = static_cast<std::uint32_t>(used);
            points[used++] = points[i];
        }
    points.resize(used);
    for (auto &triangle : triangles)
        for (auto &id : triangle.vertices)
            id = remap[id];
    stats.seedMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return triangles.empty() ? SurfaceSeedStatus::NoSurface : SurfaceSeedStatus::Succeeded;
}
} // namespace

SurfaceSeedStatus SurfaceSeedBuilder::BuildMesh(
    const SurfaceSeedGrid &grid, const double iso, const RoiReadSnapshot &roi,
    const double haloModel, const std::uint32_t blockDepth, const WorkLimit budget,
    const std::function<bool()> &cancelled, PagedArray<Point> &points,
    PagedArray<SurfaceSeedTriangle> &triangles, SurfaceExecutionStats &stats)
{
    if (grid.audit) *grid.audit = {};
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (grid.dimensions[axis] < 2 ||
            std::int64_t(grid.extent[2 * axis + 1]) - grid.extent[2 * axis] + 1 != grid.dimensions[axis])
            return SurfaceSeedStatus::InvalidInput;
    }
    const auto nx = std::uint64_t(grid.dimensions[0]), ny = std::uint64_t(grid.dimensions[1]);
    if (nx > UINT64_MAX / 4U / ny || nx * ny > SIZE_MAX / std::size_t(grid.dimensions[2]))
        return SurfaceSeedStatus::InvalidInput;

    const auto build = [&](const auto *values) {
        const auto selected = [&](auto hasRange, auto hasMask) {
        const auto read = [&](const std::size_t index) {
            const double value = static_cast<double>(values[index]);
            // 与原 reader 一样，NaN/Inf 保留为缺失支持，饱和只发生在源节点。
            if constexpr (decltype(hasRange)::value)
                return std::isfinite(value)
                       ? std::clamp(value, (*grid.materialRange)[0], (*grid.materialRange)[1]) : value;
            else return value;
        };
        return BuildSeedMesh<decltype(hasMask)::value>(grid, iso, roi, haloModel, blockDepth, budget, cancelled,
                             points, triangles, stats, read);
        };
        if (grid.materialRange) {
            if (grid.validity) return selected(std::true_type{}, std::true_type{});
            return selected(std::true_type{}, std::false_type{});
        }
        if (grid.validity) return selected(std::false_type{}, std::true_type{});
        return selected(std::false_type{}, std::false_type{});
    };
    if (grid.scalarValues) {
        switch (grid.scalarType) {
        case VTK_CHAR: return build(static_cast<const char *>(grid.scalarValues));
        case VTK_SIGNED_CHAR: return build(static_cast<const signed char *>(grid.scalarValues));
        case VTK_UNSIGNED_CHAR: return build(static_cast<const unsigned char *>(grid.scalarValues));
        case VTK_SHORT: return build(static_cast<const short *>(grid.scalarValues));
        case VTK_UNSIGNED_SHORT: return build(static_cast<const unsigned short *>(grid.scalarValues));
        case VTK_INT: return build(static_cast<const int *>(grid.scalarValues));
        case VTK_UNSIGNED_INT: return build(static_cast<const unsigned int *>(grid.scalarValues));
        case VTK_LONG: return build(static_cast<const long *>(grid.scalarValues));
        case VTK_UNSIGNED_LONG: return build(static_cast<const unsigned long *>(grid.scalarValues));
        case VTK_LONG_LONG: return build(static_cast<const long long *>(grid.scalarValues));
        case VTK_UNSIGNED_LONG_LONG: return build(static_cast<const unsigned long long *>(grid.scalarValues));
        case VTK_FLOAT: return build(static_cast<const float *>(grid.scalarValues));
        case VTK_DOUBLE: return build(static_cast<const double *>(grid.scalarValues));
        default: return SurfaceSeedStatus::InvalidInput;
        }
    }
    const auto read = [&](std::size_t index) { return grid.readScalar(grid.values, index); };
    if (grid.validity)
        return BuildSeedMesh<true>(grid, iso, roi, haloModel, blockDepth, budget, cancelled,
                                  points, triangles, stats, read);
    return BuildSeedMesh<false>(grid, iso, roi, haloModel, blockDepth, budget, cancelled,
                               points, triangles, stats, read);
}
