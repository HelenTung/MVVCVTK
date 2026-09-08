#include "SurfaceSeedBuilder.h"

#include <vtkMarchingCubesTriangleCases.h>
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <type_traits>

namespace
{
using Point = std::array<double, 3>;
using Edge = std::array<std::int64_t, 4>;
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
bool GetBudget(const std::size_t pointCount, const std::size_t triangleCount, const std::size_t budget,
               SurfaceExecutionStats &stats)
{
    // 同时覆盖 edge/clip 映射、组件、原/新网格、records 和发布载荷的保守估算。
    constexpr std::size_t perPoint = 8 * sizeof(Point) + 4 * sizeof(SurfacePointRecord) + 256;
    constexpr std::size_t perTriangle = 8 * sizeof(SurfaceSeedTriangle) + 256;
    constexpr std::size_t fixed = 64 * 1024;
    if (pointCount > UINT32_MAX || pointCount > (SIZE_MAX - fixed) / perPoint ||
        triangleCount > (SIZE_MAX - fixed - pointCount * perPoint) / perTriangle)
        return false;
    const auto estimate = fixed + pointCount * perPoint + triangleCount * perTriangle;
    stats.estimatedWorkingBytes = std::max(stats.estimatedWorkingBytes, estimate);
    return estimate <= budget;
}
SurfaceSeedStatus ClipMesh(const std::array<double, 6> &roi, const std::size_t budget,
                           const std::function<bool()> &cancelled, std::vector<Point> &points,
                           std::vector<SurfaceSeedTriangle> &triangles, SurfaceExecutionStats &stats)
{
    std::map<std::tuple<std::uint32_t, std::uint32_t, unsigned>, std::uint32_t> intersections;
    std::vector<SurfaceSeedTriangle> clipped;
    std::vector<std::uint32_t> polygon, next;
    polygon.reserve(10);
    next.reserve(10);
    for (std::size_t t = 0; t < triangles.size(); ++t)
    {
        if ((t & 255U) == 0 && cancelled && cancelled())
            return SurfaceSeedStatus::Cancelled;
        polygon.assign(triangles[t].vertices.begin(), triangles[t].vertices.end());
        for (unsigned plane = 0; plane < 6 && polygon.size() >= 3; ++plane)
        {
            const unsigned axis = plane / 2;
            const double bound = roi[plane];
            const auto distance = [&](std::uint32_t id) {
                return (points[id][axis] - bound) * (plane % 2 ? -1.0 : 1.0);
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
                const auto key = std::make_tuple(low, high, plane);
                auto found = intersections.find(key);
                if (found == intersections.end())
                {
                    if (!GetBudget(points.size() + 1, triangles.size() + clipped.size(), budget, stats))
                        return SurfaceSeedStatus::BudgetExceeded;
                    const double fraction =
                        (bound - points[low][axis]) / (points[high][axis] - points[low][axis]);
                    Point point{};
                    for (unsigned c = 0; c < 3; ++c)
                        point[c] = points[low][c] + fraction * (points[high][c] - points[low][c]);
                    point[axis] = bound;
                    const auto id = static_cast<std::uint32_t>(points.size());
                    points.push_back(point);
                    found = intersections.emplace(key, id).first;
                }
                next.push_back(found->second);
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
} // namespace

std::uint64_t SurfaceSeedBuilder::GetLabel(const LabelMap3DPayload &labels, const std::size_t index)
{
    return std::visit(
        [index](const auto &values) -> std::uint64_t {
            if (!values || index >= values->size())
                return UINT64_MAX;
            const auto value = (*values)[index];
            if constexpr (std::is_signed_v<std::decay_t<decltype(value)>>)
                if (value < 0)
                    return UINT64_MAX;
            const auto result = static_cast<std::uint64_t>(value);
            return result <= UINT32_MAX ? result : UINT64_MAX;
        },
        labels.GetValues());
}

SurfaceSeedStatus SurfaceSeedBuilder::BuildMesh(
    const SurfaceSeedGrid &grid, const double iso, const std::optional<SurfaceMaterialPair> &materials,
    const std::optional<std::array<double, 6>> &roi, const double haloModel, const std::uint32_t blockDepth,
    const std::size_t budget, const std::function<bool()> &cancelled, std::vector<Point> &points,
    std::vector<SurfaceSeedTriangle> &triangles, SurfaceExecutionStats &stats)
{
    const auto started = std::chrono::steady_clock::now();
    if (!grid.values || !grid.readScalar || !blockDepth || !points.empty() || !triangles.empty())
        return SurfaceSeedStatus::InvalidInput;
    if (!GetBudget(0, 0, budget, stats))
        return SurfaceSeedStatus::BudgetExceeded;
    if (materials && !grid.labels)
        return SurfaceSeedStatus::InvalidInput;
    std::array<int, 6> range = grid.extent;
    if (roi)
    {
        std::array<double, 6> bounds{DBL_MAX, -DBL_MAX, DBL_MAX, -DBL_MAX, DBL_MAX, -DBL_MAX};
        for (unsigned i = 0; i < 8; ++i)
        {
            const auto index =
                ToIndex(grid, {(*roi)[(i & 1) ? 1 : 0], (*roi)[(i & 2) ? 3 : 2], (*roi)[(i & 4) ? 5 : 4]});
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
    if (grid.initialMesh)
    {
        if (!grid.initialMesh->GetValid())
            return SurfaceSeedStatus::InvalidInput;
        const auto &vertices = grid.initialMesh->GetVertices();
        const auto &inputTriangles = grid.initialMesh->GetTriangles();
        if (!GetBudget(vertices.size() / 3, inputTriangles.size() / 3, budget, stats))
            return SurfaceSeedStatus::BudgetExceeded;
        for (std::size_t i = 0; i < vertices.size(); i += 3)
            points.push_back({vertices[i], vertices[i + 1], vertices[i + 2]});
        for (std::size_t i = 0; i < inputTriangles.size(); i += 3)
        {
            if ((i & 1023U) == 0 && cancelled && cancelled())
                return SurfaceSeedStatus::Cancelled;
            if (materials)
            {
                Point center{}, u{}, v{}, normal{};
                const auto a = points[static_cast<std::size_t>(inputTriangles[i])],
                           b = points[static_cast<std::size_t>(inputTriangles[i + 1])],
                           c = points[static_cast<std::size_t>(inputTriangles[i + 2])];
                for (unsigned d = 0; d < 3; ++d)
                {
                    center[d] = (a[d] + b[d] + c[d]) / 3;
                    u[d] = b[d] - a[d];
                    v[d] = c[d] - a[d];
                }
                normal = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
                const auto norm = std::hypot(normal[0], normal[1], normal[2]);
                if (norm == 0)
                    continue;
                Point directionIndex{};
                for (unsigned d = 0; d < 3; ++d)
                    for (unsigned e = 0; e < 3; ++e)
                        directionIndex[d] += grid.modelToIndex[d * 3 + e] * normal[e] / norm;
                const double indexNorm = std::hypot(directionIndex[0], directionIndex[1], directionIndex[2]);
                auto index = ToIndex(grid, center);
                std::array<std::uint64_t, 2> side{};
                bool valid = true;
                for (unsigned sideIndex = 0; sideIndex < 2; ++sideIndex)
                {
                    std::array<int, 3> nearest{};
                    for (unsigned d = 0; d < 3; ++d)
                    {
                        const double coordinate =
                            index[d] + (sideIndex ? 1 : -1) * directionIndex[d] / indexNorm;
                        if (!std::isfinite(coordinate) || coordinate < grid.extent[d * 2] ||
                            coordinate > grid.extent[d * 2 + 1])
                        {
                            valid = false;
                            break;
                        }
                        nearest[d] = static_cast<int>(std::floor(coordinate + 0.5));
                    }
                    if (!valid)
                        break;
                    const auto tuple = GetIndex(grid, nearest[0], nearest[1], nearest[2]);
                    if (grid.validity && !grid.validity[tuple])
                    {
                        valid = false;
                        break;
                    }
                    side[sideIndex] = GetLabel(*grid.labels, tuple);
                }
                if (!valid || !((side[0] == materials->materialA && side[1] == materials->materialB) ||
                                (side[0] == materials->materialB && side[1] == materials->materialA)))
                    continue;
            }
            triangles.push_back({{static_cast<std::uint32_t>(inputTriangles[i]),
                                  static_cast<std::uint32_t>(inputTriangles[i + 1]),
                                  static_cast<std::uint32_t>(inputTriangles[i + 2])}});
        }
    }
    else
    {
        std::map<Edge, std::uint32_t> edgeIds;
        const auto *cases = vtkMarchingCubesTriangleCases::GetCases();
        for (std::int64_t block = range[4]; block < range[5]; block += blockDepth)
        {
            ++stats.blockCount;
            const auto end = std::min<std::int64_t>(range[5], block + blockDepth);
            for (std::int64_t zz = block; zz < end; ++zz)
                for (std::int64_t yy = range[2]; yy < range[3]; ++yy)
                {
                    if (cancelled && cancelled())
                        return SurfaceSeedStatus::Cancelled;
                    for (std::int64_t xx = range[0]; xx < range[1]; ++xx)
                    {
                        if ((stats.scannedCellCount++ & 1023U) == 0 && cancelled && cancelled())
                            return SurfaceSeedStatus::Cancelled;
                        std::array<double, 8> scalar{};
                        std::array<std::uint64_t, 8> labels{};
                        unsigned code = 0;
                        bool supported = true;
                        for (unsigned c = 0; c < 8; ++c)
                        {
                            const auto index = GetIndex(grid, static_cast<int>(xx) + corners[c][0],
                                                        static_cast<int>(yy) + corners[c][1],
                                                        static_cast<int>(zz) + corners[c][2]);
                            scalar[c] = grid.readScalar(grid.values, index);
                            supported = supported && (!grid.validity || grid.validity[index]) &&
                                        std::isfinite(scalar[c]);
                            if (materials)
                            {
                                labels[c] = GetLabel(*grid.labels, index);
                                if (labels[c] == UINT64_MAX)
                                    return SurfaceSeedStatus::InvalidInput;
                                supported = supported && (labels[c] == materials->materialA ||
                                                          labels[c] == materials->materialB);
                                // 几何始终从 canonical 较小材料构造；方向在后续单独处理。
                                if (labels[c] == std::min(materials->materialA, materials->materialB))
                                    code |= 1U << c;
                            }
                            else if (scalar[c] >= iso)
                                code |= 1U << c;
                        }
                        if (!supported)
                        {
                            ++stats.skippedCellCount;
                            continue;
                        }
                        const auto *entries = cases[code].edges;
                        for (unsigned t = 0; t < 15 && entries[t] >= 0; t += 3)
                        {
                            SurfaceSeedTriangle triangle;
                            for (unsigned v = 0; v < 3; ++v)
                            {
                                const int a = edges[entries[t + v]][0], b = edges[entries[t + v]][1];
                                Point indexA{double(xx + corners[a][0]), double(yy + corners[a][1]),
                                             double(zz + corners[a][2])};
                                Point indexB{double(xx + corners[b][0]), double(yy + corners[b][1]),
                                             double(zz + corners[b][2])};
                                double fraction =
                                    materials ? 0.5 : (iso - scalar[a]) / (scalar[b] - scalar[a]);
                                fraction = std::clamp(fraction, 0.0, 1.0);
                                Edge key{};
                                unsigned axis = 0;
                                for (unsigned d = 0; d < 3; ++d)
                                {
                                    key[d] = static_cast<std::int64_t>(std::min(indexA[d], indexB[d]));
                                    if (indexA[d] != indexB[d])
                                        axis = d;
                                }
                                key[3] = axis;
                                if (fraction == 0 || fraction == 1)
                                {
                                    const auto &corner = fraction == 0 ? indexA : indexB;
                                    for (unsigned d = 0; d < 3; ++d)
                                        key[d] = static_cast<std::int64_t>(corner[d]);
                                    key[3] = 3;
                                }
                                auto found = edgeIds.find(key);
                                if (found == edgeIds.end())
                                {
                                    if (!GetBudget(points.size() + 1, triangles.size() + 1, budget, stats))
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
                                    found = edgeIds.emplace(key, id).first;
                                }
                                triangle.vertices[v] = found->second;
                            }
                            if (triangle.vertices[0] != triangle.vertices[1] &&
                                triangle.vertices[1] != triangle.vertices[2] &&
                                triangle.vertices[0] != triangle.vertices[2])
                            {
                                if (!GetBudget(points.size(), triangles.size() + 1, budget, stats))
                                    return SurfaceSeedStatus::BudgetExceeded;
                                triangles.push_back(triangle);
                            }
                        }
                    }
                }
        }
    }
    if (roi)
    {
        const auto status = ClipMesh(*roi, budget, cancelled, points, triangles, stats);
        if (status != SurfaceSeedStatus::Succeeded)
            return status;
    }
    // 删除未被最终 ROI 三角形引用的 seed 顶点，索引按首次生成顺序稳定压缩。
    std::vector<std::uint32_t> remap(points.size(), UINT32_MAX);
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
