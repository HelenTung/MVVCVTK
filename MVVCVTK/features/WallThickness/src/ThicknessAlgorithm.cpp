#include "ThicknessAlgorithm.h"
#include "ThicknessMath.h"
#include "ThicknessMaterialField.h"
#include <vtkCellArray.h>
#include <vtkGenericCell.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkStaticCellLocator.h>
#include <exception>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace ThicknessAlgorithm
{
namespace
{
using namespace ThicknessMath;
constexpr double pi = 3.14159265358979323846;
struct Failure final
{
    ThicknessStatus status;
    const char *message;
};
void CheckWork(const Work &work)
{
    if (work.cancelled && work.cancelled->load(std::memory_order_relaxed))
        throw Failure{ThicknessStatus::Cancelled, "Thickness analysis cancelled."};
    if (std::chrono::steady_clock::now() >= work.deadline)
        throw Failure{ThicknessStatus::DeadlineExceeded, "Thickness deadline exceeded."};
}
bool Positive(double v)
{
    return std::isfinite(v) && v > 0;
}
bool Nonnegative(double v)
{
    return std::isfinite(v) && v >= 0;
}

template <class T> double ReadScalar(const std::uint8_t *bytes, std::size_t index)
{
    T value{};
    std::memcpy(&value, bytes + index * sizeof(T), sizeof(T));
    return static_cast<double>(value);
}

class Kernel final
{
    const Work &m_work;
    const ThicknessParams &m_params;
    const GridGeometry3D &m_geometry;
    const SurfaceMeshPayload &m_mesh;
    const MeshAttribute *m_normal = nullptr;
    double (*m_readScalar)(const std::uint8_t *, std::size_t) = nullptr;
    vtkSmartPointer<vtkPolyData> m_poly;
    vtkSmartPointer<vtkStaticCellLocator> m_locator;
    vtkSmartPointer<vtkGenericCell> m_cell = vtkSmartPointer<vtkGenericCell>::New();
    std::vector<double> m_areas;
    unsigned m_n = 1;
    std::size_t m_sampleCount = 0;
    double m_epsilon = 0, m_minSpacing = 0;
    double m_sourceMargin = 0;
    struct SourceMeasurement { std::size_t triangle = 0; unsigned rule = 0; std::optional<double> value; };
    std::size_t m_nodeBudget = 0, m_sourceCapacity = 0, m_workerCount = 1;

    bool GetIsSourceLocal() const
    {
        return m_params.boundaryPolicy == ThicknessBoundaryPolicy::SourceExtentLocal;
    }
    bool GetInsideExtent(const std::array<std::int64_t, 3> &index) const
    {
        for (unsigned a = 0; a < 3; ++a)
            if (index[a] < m_geometry.extent[2 * a] || index[a] > m_geometry.extent[2 * a + 1])
                return false;
        return true;
    }
    bool GetInsideSource(const ThicknessPoint &point, double margin) const
    {
        const auto index = Index(point);
        if (!Finite(index)) return false;
        for (unsigned a = 0; a < 3; ++a)
            if (index[a] < m_geometry.extent[2 * a] + margin / m_geometry.spacing[a] ||
                index[a] > m_geometry.extent[2 * a + 1] - margin / m_geometry.spacing[a])
                return false;
        return true;
    }
    bool GetIsSourceFace(std::uint64_t first, std::uint64_t second) const
    {
        const auto a = Index(Vertex(first)), b = Index(Vertex(second));
        for (unsigned axis = 0; axis < 3; ++axis)
            for (unsigned side = 0; side < 2; ++side)
            {
                const double face = m_geometry.extent[2 * axis + side];
                if (std::abs(a[axis] - face) <= m_epsilon / m_geometry.spacing[axis] &&
                    std::abs(b[axis] - face) <= m_epsilon / m_geometry.spacing[axis]) return true;
            }
        return false;
    }
    ThicknessMaterialField::Field GetMaterialField() const
    {
        ThicknessMaterialField::Field field;
        field.extent = m_geometry.extent;
        field.threshold = *m_params.materialThreshold;
        field.check = [this] { CheckWork(m_work); };
        field.node = [this](const std::array<std::int64_t, 3> &index, double &value)
        {
            if (!GetInsideExtent(index) || Material(index) == -1) return false;
            std::size_t offset = 0, stride = 1;
            for (unsigned a = 0; a < 3; ++a)
            {
                offset += std::size_t(index[a] - m_geometry.extent[2 * a]) * stride;
                stride *= std::size_t(m_geometry.dimensions[a]);
            }
            const auto &mask = m_work.source->GetValidityMask();
            if (mask && (*mask)[offset] == 0) return false;
            // 路径始终读原始源灰度，不沿用表面生成阶段的饱和值。
            value = m_readScalar(m_work.source->GetValues()->data(), offset);
            return std::isfinite(value);
        };
        return field;
    }

    ThicknessPoint Vertex(std::uint64_t id) const
    {
        const auto *v = m_mesh.GetVertices().data() + id * 3;
        return Sub({v[0], v[1], v[2]}, m_geometry.origin);
    }
    ThicknessPoint Interpolate(std::uint64_t triangle, const ThicknessPoint &bary) const
    {
        ThicknessPoint p{};
        for (std::size_t k = 0; k < 3; ++k)
            p = Add(p, Scale(Vertex(m_mesh.GetTriangles()[triangle * 3 + k]), bary[k]));
        return p;
    }
    ThicknessPoint Index(const ThicknessPoint &p) const
    {
        ThicknessPoint r{};
        for (std::size_t a = 0; a < 3; ++a)
            for (std::size_t k = 0; k < 3; ++k)
                r[a] += m_geometry.direction[k * 3 + a] * p[k] / m_geometry.spacing[a];
        return r;
    }
    ThicknessPoint Model(const ThicknessPoint &i) const
    {
        ThicknessPoint r{};
        for (std::size_t a = 0; a < 3; ++a)
            for (std::size_t k = 0; k < 3; ++k)
                r[a] += m_geometry.direction[a * 3 + k] * i[k] * m_geometry.spacing[k];
        return r;
    }
    // -2缺失支持，-1其他材料/未知，0背景，1唯一选定材料。整数不经double。
    int Material(const std::array<std::int64_t, 3> &i) const
    {
        std::size_t offset = 0, stride = 1;
        for (std::size_t a = 0; a < 3; ++a)
        {
            if (i[a] < m_geometry.extent[a * 2] || i[a] > m_geometry.extent[a * 2 + 1])
                return -2;
            offset += static_cast<std::size_t>(i[a] - m_geometry.extent[a * 2]) * stride;
            stride *= static_cast<std::size_t>(m_geometry.dimensions[a]);
        }
        const auto &mask = m_work.source->GetValidityMask();
        if (mask && (*mask)[offset] == 0)
            return -2;
        return std::visit(
            [&](const auto &values)
            {
                using Value = typename std::decay_t<decltype(*values)>::value_type;
                const auto v = (*values)[offset];
                if constexpr (std::is_signed_v<Value>)
                {
                    if (v < 0)
                        return -1;
                }
                const auto label = static_cast<std::uint64_t>(v);
                return label == m_work.archive.input.materialLabel ? 1 : label == 0 ? 0 : -1;
            },
            m_work.labels->GetValues());
    }
    int MaterialAt(const ThicknessPoint &p) const
    {
        return GetMaterialField().GetMaterialAt(Index(p));
    }
    void ValidateInput()
    {
        if (!m_work.source->GetValid() || !m_work.labels->GetValid() || !m_mesh.GetValid() ||
            m_mesh.GetVertices().empty() || m_mesh.GetTriangles().empty())
            throw Failure{ThicknessStatus::InvalidInput, "Invalid input payload."};
        const auto &g = m_work.source->GetGeometry();
        if (m_work.source->GetComponentCount() != 1 ||
            std::any_of(g.dimensions.begin(), g.dimensions.end(), [](int n) { return n < 2; }))
            throw Failure{ThicknessStatus::InvalidInput, "A scalar grid with at least two nodes per axis is required."};
        switch (m_work.source->GetValueType())
        {
        case ImageValueType::Int8: m_readScalar = ReadScalar<std::int8_t>; break;
        case ImageValueType::UInt8: m_readScalar = ReadScalar<std::uint8_t>; break;
        case ImageValueType::Int16: m_readScalar = ReadScalar<std::int16_t>; break;
        case ImageValueType::UInt16: m_readScalar = ReadScalar<std::uint16_t>; break;
        case ImageValueType::Int32: m_readScalar = ReadScalar<std::int32_t>; break;
        case ImageValueType::UInt32: m_readScalar = ReadScalar<std::uint32_t>; break;
        case ImageValueType::Float32: m_readScalar = ReadScalar<float>; break;
        case ImageValueType::Float64: m_readScalar = ReadScalar<double>; break;
        default: throw Failure{ThicknessStatus::InvalidInput, "Unsupported exact scalar representation."};
        }
        if (g.extent != m_geometry.extent || g.spacing != m_geometry.spacing ||
            g.origin != m_geometry.origin || g.direction != m_geometry.direction ||
            g.coordinateFrame != m_geometry.coordinateFrame ||
            m_mesh.GetCoordinateFrame() != m_geometry.coordinateFrame)
            throw Failure{ThicknessStatus::InvalidInput, "Input geometries or frames differ."};
        for (std::size_t a = 0; a < 3; ++a)
            for (std::size_t b = 0; b < 3; ++b)
            {
                double dot = 0;
                for (std::size_t k = 0; k < 3; ++k)
                    dot += g.direction[k * 3 + a] * g.direction[k * 3 + b];
                if (std::abs(dot - (a == b ? 1.0 : 0.0)) > 1e-8)
                    throw Failure{ThicknessStatus::InvalidInput,
                                  "Grid direction must be orthonormal."};
            }
        m_minSpacing = *std::min_element(g.spacing.begin(), g.spacing.end());
        if (m_params.maxBoundaryError > 0.5 * m_minSpacing)
            throw Failure{ThicknessStatus::InvalidInput,
                          "Endpoint error exceeds half minimum spacing."};
        std::set<std::string> names;
        const MeshAttribute *complete = nullptr;
        for (const auto &a : m_mesh.GetPointAttributes())
        {
            if (!names.insert(a.name).second)
                throw Failure{ThicknessStatus::InvalidInput, "Duplicate mesh attribute."};
            if (a.name == "measurement.normal")
                m_normal = &a;
            if (a.name == "measurement.boundary-complete")
                complete = &a;
        }
        if (!GetIsSourceLocal() && (!complete || complete->componentCount != 1 ||
            !std::all_of(complete->values.begin(), complete->values.end(),
                         [](double v) { return v == 1; })))
            throw Failure{ThicknessStatus::IncompleteBoundary,
                          "Mesh lacks complete-boundary provenance."};
        if (!m_normal || m_normal->componentCount != 3)
            throw Failure{ThicknessStatus::InvalidInput,
                          "Published measurement normals are required."};
    }
    void BuildMesh()
    {
        const auto pc = m_mesh.GetVertices().size() / 3, tc = m_mesh.GetTriangles().size() / 3;
        const auto &limits = m_work.archive.limits;
        if (pc > static_cast<std::size_t>(std::numeric_limits<vtkIdType>::max()) ||
            tc > static_cast<std::size_t>(std::numeric_limits<vtkIdType>::max()) ||
            pc > limits.maxWorkingBytes / 24U || tc > limits.maxWorkingBytes / 256U)
            throw Failure{ThicknessStatus::BudgetExceeded, "Mesh exceeds working budget."};
        if (pc * 24U > limits.maxWorkingBytes - tc * 256U)
            throw Failure{ThicknessStatus::BudgetExceeded, "Mesh exceeds working budget."};
        const auto meshBytes = pc * 24U + tc * 256U;
        if (meshBytes > limits.maxWorkingBytes)
            throw Failure{ThicknessStatus::BudgetExceeded, "Mesh exceeds working budget."};
        auto points = vtkSmartPointer<vtkPoints>::New();
        points->SetDataTypeToDouble();
        points->SetNumberOfPoints(static_cast<vtkIdType>(pc));
        auto minimum = Vertex(0), maximum = minimum;
        for (std::size_t i = 0; i < pc; ++i)
        {
            if ((i & 255U) == 0)
                CheckWork(m_work);
            const auto p = Vertex(i);
            if (!Finite(p))
                throw Failure{ThicknessStatus::InvalidInput, "Coordinate overflow."};
            points->SetPoint(static_cast<vtkIdType>(i), p.data());
            for (std::size_t k = 0; k < 3; ++k)
            {
                minimum[k] = std::min(minimum[k], p[k]);
                maximum[k] = std::max(maximum[k], p[k]);
            }
        }
        m_epsilon = std::max(64 * std::numeric_limits<double>::epsilon() *
                                 std::max(Length(Sub(maximum, minimum)), 1.0),
                             m_minSpacing * 1e-8);
        // 减原点不能恢复输入double在世界坐标处已产生的舍入；计入其ULP下界。
        for (double origin : m_geometry.origin)
            m_epsilon =
                std::max(m_epsilon, 8 * std::numeric_limits<double>::epsilon() * std::abs(origin));
        if (!std::isfinite(m_epsilon) || m_epsilon >= 0.01 * m_minSpacing)
            throw Failure{ThicknessStatus::InvalidInput,
                          "Coordinate precision cannot resolve the input spacing."};
        m_sourceMargin = 0.5 * Length(m_geometry.spacing) + 2 * m_params.maxBoundaryError + 16 * m_epsilon;
        auto cells = vtkSmartPointer<vtkCellArray>::New();
        m_areas.reserve(tc);
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::pair<unsigned, int>> edges;
        double maxEdge = 0;
        for (std::size_t i = 0; i < tc; ++i)
        {
            if ((i & 255U) == 0)
                CheckWork(m_work);
            std::array<vtkIdType, 3> ids{};
            std::array<ThicknessPoint, 3> p{};
            for (std::size_t k = 0; k < 3; ++k)
            {
                ids[k] = static_cast<vtkIdType>(m_mesh.GetTriangles()[i * 3 + k]);
                p[k] = Vertex(ids[k]);
            }
            const double area = 0.5 * Length(Cross(Sub(p[1], p[0]), Sub(p[2], p[0])));
            if (!std::isfinite(area) || area <= m_epsilon * m_epsilon)
                throw Failure{ThicknessStatus::IncompleteBoundary, "Degenerate mesh triangle."};
            m_areas.push_back(area);
            for (std::size_t k = 0; k < 3; ++k)
            {
                const auto a = static_cast<std::uint64_t>(ids[k]),
                           b = static_cast<std::uint64_t>(ids[(k + 1) % 3]);
                auto &e = edges[{std::min(a, b), std::max(a, b)}];
                ++e.first;
                e.second += a < b ? 1 : -1;
                maxEdge = std::max(maxEdge, Length(Sub(p[k], p[(k + 1) % 3])));
            }
            cells->InsertNextCell(3, ids.data());
        }
        for (const auto &e : edges)
        {
            if (e.second.first == 1 && GetIsSourceLocal() && GetIsSourceFace(e.first.first, e.first.second))
                continue;
            if (e.second.first != 2 || e.second.second != 0)
                throw Failure{ThicknessStatus::IncompleteBoundary,
                              "Mesh must be closed and consistently manifold."};
        }
        const double divisions = std::ceil(maxEdge / m_params.sampleSpacing);
        if (!std::isfinite(divisions) || divisions > 64)
            throw Failure{ThicknessStatus::BudgetExceeded, "Surface subdivision limit exceeded."};
        m_n = static_cast<unsigned>(std::max(divisions, 1.0));
        if (tc > limits.maxSamples / (m_n * m_n))
            throw Failure{ThicknessStatus::BudgetExceeded, "Sample count limit exceeded."};
        m_sampleCount = tc * m_n * m_n;
        if (m_sampleCount > (limits.maxWorkingBytes - meshBytes) / (sizeof(ThicknessSample) + 640U))
            throw Failure{ThicknessStatus::BudgetExceeded,
                          "Sampling and topology exceed working budget."};
        // 每个并行槽独占射线单元与有界路径工作区，归并仍按来源顺序执行。
        auto remaining = limits.maxWorkingBytes - meshBytes -
                         m_sampleCount * (sizeof(ThicknessSample) + 640U);
        m_sourceCapacity = tc > limits.maxSamples / 9U ? limits.maxSamples : tc * 9U;
        if (m_sourceCapacity > remaining / sizeof(SourceMeasurement))
            throw Failure{ThicknessStatus::BudgetExceeded, "Source values exceed working budget."};
        remaining -= m_sourceCapacity * sizeof(SourceMeasurement);
        const std::size_t workerBytes = 8U * 1024U * 1024U + tc;
        if (remaining <= workerBytes)
            throw Failure{ThicknessStatus::BudgetExceeded, "Material path workspace exceeds budget."};
        m_workerCount = std::min({std::size_t(8),
            std::size_t(std::max(1U, std::thread::hardware_concurrency())),
            std::max(std::size_t(1), remaining / workerBytes / 2)});
        m_nodeBudget = (remaining - m_workerCount * workerBytes) / 192U;
        m_poly = vtkSmartPointer<vtkPolyData>::New();
        m_poly->SetPoints(points);
        m_poly->SetPolys(cells);
        m_poly->BuildCells();
        m_locator = vtkSmartPointer<vtkStaticCellLocator>::New();
        m_locator->SetDataSet(m_poly);
        m_locator->BuildLocator();
        CheckWork(m_work);
    }
    void CheckBoundary()
    {
        const double tolerance = 0.5 * Length(m_geometry.spacing) + m_params.maxBoundaryError;
        bool hasMaterial = false;
        std::size_t checks = 0;
        for (std::int64_t z = m_geometry.extent[4]; z <= m_geometry.extent[5]; ++z)
            for (std::int64_t y = m_geometry.extent[2]; y <= m_geometry.extent[3]; ++y)
                for (std::int64_t x = m_geometry.extent[0]; x <= m_geometry.extent[1]; ++x)
                {
                    if ((checks++ & 255U) == 0)
                        CheckWork(m_work);
                    const std::array<std::int64_t, 3> i{x, y, z};
                    const auto material = Material(i);
                    if (GetIsSourceLocal() && material == -2)
                        throw Failure{ThicknessStatus::IncompleteBoundary, "Source contains missing validity support."};
                    if (material != 1)
                        continue;
                    hasMaterial = true;
                    for (std::size_t a = 0; a < 3; ++a)
                        for (int sign : {-1, 1})
                        {
                            auto neighbor = i;
                            neighbor[a] += sign;
                            if (GetIsSourceLocal() && !GetInsideExtent(neighbor)) continue;
                            const int label = Material(neighbor);
                            if (label == 1)
                                continue;
                            if (label == -2)
                                throw Failure{
                                    ThicknessStatus::IncompleteBoundary,
                                    "Selected material reaches truncated or invalid support."};
                            ThicknessPoint center{double(x), double(y), double(z)};
                            center[a] += 0.5 * sign;
                            const auto p = Model(center);
                            if (!Finite(p))
                                throw Failure{ThicknessStatus::InvalidInput,
                                              "Grid coordinate overflow."};
                            double closest[3]{}, d2 = 0;
                            vtkIdType cellId = -1;
                            int subId = 0;
                            m_locator->FindClosestPoint(p.data(), closest, m_cell, cellId, subId,
                                                        d2);
                            if (cellId < 0 || !std::isfinite(d2) || d2 > tolerance * tolerance)
                                throw Failure{ThicknessStatus::IncompleteBoundary,
                                              "Selected material boundary missing from mesh."};
                        }
                }
        if (!hasMaterial)
            throw Failure{ThicknessStatus::InvalidInput, "Selected material label absent."};
    }
    bool GetNormal(std::uint64_t triangle, const ThicknessPoint &bary, ThicknessPoint &normal) const
    {
        normal = {};
        for (std::size_t k = 0; k < 3; ++k)
        {
            const auto id = m_mesh.GetTriangles()[triangle * 3 + k];
            const auto *n = m_normal->values.data() + id * 3;
            const auto unit = Unit({n[0], n[1], n[2]});
            if (!Finite(unit) || Length(unit) < 0.5) return false;
            normal = Add(normal, Scale(unit, bary[k]));
        }
        normal = Unit(normal);
        return Finite(normal) && Length(normal) > 0.5;
    }
    bool Inward(const ThicknessPoint &p, ThicknessPoint &normal, bool &outside) const
    {
        const double probe = m_params.maxBoundaryError + 8 * m_epsilon;
        const int plus = MaterialAt(Add(p, Scale(normal, probe))),
                  minus = MaterialAt(Sub(p, Scale(normal, probe)));
        if (plus < 0 || minus < 0) { outside = false; return false; }
        outside = plus != 1 && minus != 1;
        if ((plus == 1) == (minus == 1))
            return false;
        if (plus != 1)
            normal = Scale(normal, -1);
        return true;
    }
    struct Hit final
    {
        ThicknessPoint point{}, bary{};
        std::uint64_t triangle = 0;
        double distance = 0;
    };
    bool Intersect(const ThicknessPoint &p, const ThicknessPoint &direction, Hit &hit, vtkGenericCell *cell) const
    {
        const auto start = Add(p, Scale(direction, 2 * m_epsilon)),
                   end = Add(p, Scale(direction, m_params.maxDistance));
        if (!Finite(start) || !Finite(end))
            throw Failure{ThicknessStatus::InvalidInput, "Ray coordinate overflow."};
        double t = 0, point[3]{}, pc[3]{};
        int subId = 0;
        vtkIdType cellId = -1;
        if (!m_locator->IntersectWithLine(start.data(), end.data(), m_epsilon, t, point, pc, subId,
                                          cellId, cell) ||
            cellId < 0)
            return false;
        hit.point = {point[0], point[1], point[2]};
        hit.bary = {1 - pc[0] - pc[1], pc[0], pc[1]};
        hit.triangle = static_cast<std::uint64_t>(cellId);
        hit.distance = Length(Sub(hit.point, p));
        return std::isfinite(hit.distance) && hit.distance > 4 * m_epsilon &&
               hit.distance <= m_params.maxDistance + m_epsilon;
    }
    bool GetMaterialPath(const ThicknessPoint &p, const ThicknessPoint &q) const
    {
        const double length = Length(Sub(q, p));
        const auto from = Index(p), delta = Sub(Index(q), from);
        if (!Finite(from) || !Finite(delta)) return false;
        const double tolerance = m_work.archive.input.unit == ThicknessUnit::Millimeter ? 1e-9 : 1e-12;
        return GetMaterialField().GetMaterialPath(from, delta, length, m_epsilon, m_params.maxBoundaryError,
                                       tolerance).status == ThicknessMaterialField::Status::Valid;
    }
    std::optional<double> GetMeasurement(std::uint64_t triangle, const ThicknessPoint &bary,
                                         const ThicknessPoint &p, vtkGenericCell *cell) const
    {
        if (GetIsSourceLocal() && !GetInsideSource(p, m_sourceMargin)) return {};
        ThicknessPoint normal{};
        bool outside = false;
        if (!GetNormal(triangle, bary, normal) || !Inward(p, normal, outside)) return {};
        std::size_t axis = 0;
        for (std::size_t k = 1; k < 3; ++k)
            if (std::abs(normal[k]) < std::abs(normal[axis])) axis = k;
        ThicknessPoint ref{};
        ref[axis] = 1;
        const auto u = Unit(Cross(normal, ref)), v = Cross(normal, u);
        std::optional<double> minimum;
        for (std::uint32_t di = 0; di < m_params.directionCount; ++di)
        {
            CheckWork(m_work);
            auto direction = normal;
            if (di)
            {
                const double theta = m_params.coneAngleDegrees * pi / 180,
                             phi = 2 * pi * (di - 1) / (m_params.directionCount - 1);
                direction = Add(Scale(normal, std::cos(theta)),
                    Scale(Add(Scale(u, std::cos(phi)), Scale(v, std::sin(phi))), std::sin(theta)));
            }
            // 局部源盒必须覆盖整个搜索锥，不能因缺边界而选到偏小的一条射线。
            if (GetIsSourceLocal() && !GetInsideSource(Add(p, Scale(direction, m_params.maxDistance)),
                                                m_sourceMargin)) return {};
            Hit forward;
            if (!Intersect(p, direction, forward, cell)) continue;
            ThicknessPoint oppositeNormal{};
            if (!GetNormal(forward.triangle, forward.bary, oppositeNormal) ||
                !Inward(forward.point, oppositeNormal, outside) || Dot(direction, oppositeNormal) > 0 ||
                !GetMaterialPath(p, forward.point)) continue;
            // 正向锥 <= 30° 且对端迎向射线，已排除相同单位法向的退化情形。
            const auto value = GetNormalOffset(Sub(forward.point, p), normal, oppositeNormal);
            if (value && (!minimum || *value < *minimum)) minimum = value;
        }
        // maxDistance 限制搜索弦长；共同法向位移不按该上限截断。
        return minimum;
    }
    struct NodeWeight { std::array<int, 3> index{}; double weight = 0; };
    std::array<NodeWeight, 8> GetNodeWeights(const ThicknessPoint &point) const
    {
        std::array<NodeWeight, 8> result{};
        const auto index = Index(point);
        if (!Finite(index)) throw Failure{ThicknessStatus::InvalidInput, "Source index overflow."};
        if (!GetInsideSource(point, 0)) return result;
        std::array<std::int64_t, 3> cell{};
        for (unsigned a = 0; a < 3; ++a) cell[a] = static_cast<std::int64_t>(std::floor(index[a]));
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            auto node = cell;
            double weight = 1;
            for (unsigned a = 0; a < 3; ++a)
            {
                node[a] += (corner >> a) & 1U;
                weight *= std::max(0.0, 1 - std::abs(index[a] - double(node[a])));
            }
            if (weight <= 0 || !GetInsideExtent(node)) continue;
            if (m_params.evaluationBounds)
            {
                // 只省略对评价区所有 Q1 查询严格零贡献的节点；每个保留节点仍取完整来源。
                const auto center = Add(Model({double(node[0]), double(node[1]), double(node[2])}), m_geometry.origin);
                bool intersects = true;
                for (unsigned a = 0; a < 3; ++a)
                {
                    double radius = m_epsilon;
                    for (unsigned k = 0; k < 3; ++k)
                        radius += std::abs(m_geometry.direction[a * 3 + k]) * m_geometry.spacing[k];
                    if (center[a] + radius < (*m_params.evaluationBounds)[2 * a] ||
                        center[a] - radius > (*m_params.evaluationBounds)[2 * a + 1]) intersects = false;
                }
                if (!intersects) continue;
            }
            result[corner] = {{int(node[0]), int(node[1]), int(node[2])}, weight};
        }
        return result;
    }
    std::shared_ptr<const std::vector<ThicknessNode>> BuildNodes()
    {
        struct Quadrature { ThicknessPoint bary{}; double weight = 0; };
        std::array<Quadrature, 9> rules{};
        const double a = std::sqrt(3.0 / 5.0);
        const std::array<double, 3> abscissas{-a, 0, a}, weights{5.0 / 9, 8.0 / 9, 5.0 / 9};
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < 3; ++j)
            {
                const double u = (abscissas[i] + 1) * 0.5, v = (abscissas[j] + 1) * 0.5;
                rules[3 * i + j] = {{1 - u, u * (1 - v), u * v}, 0.5 * u * weights[i] * weights[j]};
            }
        std::vector<SourceMeasurement> sources;
        sources.reserve(m_sourceCapacity);
        for (std::size_t triangle = 0; triangle < m_areas.size(); ++triangle)
            for (unsigned rule = 0; rule < rules.size(); ++rule)
            {
                CheckWork(m_work);
                const auto support = GetNodeWeights(Interpolate(triangle, rules[rule].bary));
                if (std::none_of(support.begin(), support.end(), [](const auto &w) { return w.weight > 0; })) continue;
                if (sources.size() == m_sourceCapacity)
                    throw Failure{ThicknessStatus::BudgetExceeded, "Source quadrature count limit exceeded."};
                sources.push_back({triangle, rule, {}});
            }
        std::atomic<bool> failed{false};
        std::exception_ptr failure;
        const auto run = [&](std::size_t worker)
            {
                try
                {
                    auto cell = vtkSmartPointer<vtkGenericCell>::New();
                    for (std::size_t i = worker; i < sources.size(); i += m_workerCount)
                        {
                            if (failed.load(std::memory_order_relaxed)) return;
                            CheckWork(m_work);
                            auto &source = sources[i];
                            const auto &bary = rules[source.rule].bary;
                            source.value = GetMeasurement(source.triangle, bary, Interpolate(source.triangle, bary), cell);
                        }
                }
                catch (...)
                {
                    if (!failed.exchange(true)) failure = std::current_exception();
                }
            };
        // 仅本次任务的有界工作线程，不改 VTK/Host 的全局并行配置。
        std::vector<std::thread> workers;
        workers.reserve(m_workerCount - 1);
        try
        {
            for (std::size_t i = 1; i < m_workerCount; ++i) workers.emplace_back(run, i);
        }
        catch (...)
        {
            failed.store(true);
            for (auto &worker : workers) worker.join();
            throw;
        }
        run(0);
        for (auto &worker : workers) worker.join();
        if (failure) std::rethrow_exception(failure);
        struct Accumulator { double valid = 0, total = 0, moment = 0, validError = 0, totalError = 0, momentError = 0; };
        const auto add = [](double &sum, double &error, double value)
        {
            const double adjusted = value - error, next = sum + adjusted;
            error = (next - sum) - adjusted;
            sum = next;
        };
        std::map<std::array<int, 3>, Accumulator> accumulators;
        for (const auto &source : sources)
        {
            CheckWork(m_work);
            const auto &rule = rules[source.rule];
            const auto support = GetNodeWeights(Interpolate(source.triangle, rule.bary));
            for (const auto &node : support)
            {
                if (node.weight <= 0) continue;
                auto found = accumulators.find(node.index);
                if (found == accumulators.end())
                {
                    if (accumulators.size() >= m_nodeBudget)
                        throw Failure{ThicknessStatus::BudgetExceeded, "Sparse node field exceeds budget."};
                    found = accumulators.emplace(node.index, Accumulator{}).first;
                }
                auto &sum = found->second;
                const double weight = m_areas[source.triangle] * rule.weight * node.weight;
                add(sum.total, sum.totalError, weight);
                if (source.value)
                {
                    add(sum.valid, sum.validError, weight);
                    add(sum.moment, sum.momentError, weight * *source.value);
                }
                if (!std::isfinite(sum.total) || !std::isfinite(sum.moment))
                    throw Failure{ThicknessStatus::InvalidInput, "Node accumulation overflow."};
            }
        }
        auto nodes = std::make_shared<std::vector<ThicknessNode>>();
        nodes->reserve(accumulators.size());
        for (const auto &entry : accumulators)
        {
            CheckWork(m_work);
            const auto &sum = entry.second;
            nodes->push_back({entry.first, sum.valid, sum.total, sum.valid > 0 ? sum.moment / sum.valid : 0});
        }
        return nodes;
    }

  public:
    explicit Kernel(const Work &work)
        : m_work(work), m_params(work.archive.params), m_geometry(work.labels->GetGeometry()),
          m_mesh(*work.mesh)
    {
    }
    Candidate Build()
    {
        CheckWork(m_work);
        ValidateInput();
        BuildMesh();
        CheckBoundary();
        Field field;
        field.geometry = m_geometry;
        field.coordinateTolerance = m_epsilon;
        field.nodes = BuildNodes();
        auto samples = std::make_shared<std::vector<ThicknessSample>>();
        auto neighbors = std::make_shared<Neighbors>();
        samples->reserve(m_sampleCount);
        neighbors->reserve(m_sampleCount);
        using VertexKey = std::array<std::pair<std::uint64_t, unsigned>, 3>;
        std::map<VertexKey, std::size_t> vertices;
        std::map<std::pair<std::size_t, std::size_t>, std::pair<std::size_t, std::size_t>> edges;
        auto add =
            [&](std::uint64_t triangle, const std::array<std::array<unsigned, 3>, 3> &corners)
        {
            CheckWork(m_work);
            ThicknessSample sample;
            sample.sourceTriangle = triangle;
            ThicknessPoint bary{};
            std::array<std::size_t, 3> ids{};
            for (std::size_t k = 0; k < 3; ++k)
            {
                VertexKey key{};
                std::size_t count = 0;
                for (std::size_t a = 0; a < 3; ++a)
                {
                    sample.barycentricCorners[k][a] = double(corners[k][a]) / m_n;
                    if (corners[k][a])
                        key[count++] = {m_mesh.GetTriangles()[triangle * 3 + a], corners[k][a]};
                }
                std::sort(key.begin(), key.begin() + count);
                ids[k] = vertices.emplace(key, vertices.size()).first->second;
                bary = Add(bary, Scale(sample.barycentricCorners[k], 1.0 / 3));
            }
            const auto p = Interpolate(triangle, bary);
            sample.source = Add(p, m_geometry.origin);
            sample.area = m_areas[triangle] / (double(m_n) * m_n);
            if (m_params.evaluationBounds)
                for (std::size_t a = 0; a < 3; ++a)
                    if (sample.source[a] < (*m_params.evaluationBounds)[a * 2] ||
                        sample.source[a] > (*m_params.evaluationBounds)[a * 2 + 1])
                        sample.validity = ThicknessValidity::OutsideEvaluation;
            if (sample.validity != ThicknessValidity::OutsideEvaluation)
            {
                const auto value = GetValue(field, sample.source);
                sample.validity = value ? ThicknessValidity::Valid : ThicknessValidity::NoValidSource;
                sample.thickness = value.value_or(0);
            }
            const auto id = samples->size();
            neighbors->push_back({noNeighbor, noNeighbor, noNeighbor});
            for (std::size_t k = 0; k < 3; ++k)
            {
                const std::pair<std::size_t, std::size_t> edge{std::min(ids[k], ids[(k + 1) % 3]),
                                                               std::max(ids[k], ids[(k + 1) % 3])};
                const auto found = edges.find(edge);
                if (found == edges.end())
                    edges.emplace(edge, std::make_pair(id, k));
                else
                {
                    const auto old = found->second;
                    (*neighbors)[id][k] = old.first;
                    (*neighbors)[old.first][old.second] = id;
                    edges.erase(found);
                }
            }
            samples->push_back(std::move(sample));
        };
        // 同一n的整数重心细分，面积恰好为原面/n²；ROI只筛选评价点。
        for (std::size_t triangle = 0; triangle < m_areas.size(); ++triangle)
            for (unsigned row = 0; row < m_n; ++row)
                for (unsigned column = 0; column < m_n - row; ++column)
                {
                    const auto b = [&](unsigned i, unsigned j)
                    { return std::array<unsigned, 3>{m_n - i - j, i, j}; };
                    add(triangle, {b(row, column), b(row + 1, column), b(row, column + 1)});
                    if (row + column + 1 < m_n)
                        add(triangle,
                            {b(row + 1, column), b(row + 1, column + 1), b(row, column + 1)});
                }
        field.samples = std::move(samples);
        field.neighbors = std::move(neighbors);
        field.subdivisions = m_n;
        auto result = BuildEvaluation(field, m_work.archive.evaluation, m_params,
                                      m_work.archive.limits, m_work.cancelled, m_work.deadline);
        result.field = std::move(field);
        return result;
    }
};
} // namespace
bool GetParamsValid(const ThicknessParams &p) noexcept
{
    if (!Positive(p.maxDistance) || !Positive(p.sampleSpacing) || !p.materialThreshold ||
        !std::isfinite(*p.materialThreshold) || !Nonnegative(p.maxBoundaryError) ||
        static_cast<unsigned>(p.boundaryPolicy) > 1 ||
        !Nonnegative(p.coneAngleDegrees) || p.coneAngleDegrees > 30 ||
        (p.directionCount != 1 && p.directionCount != 9 && p.directionCount != 17) ||
        (p.directionCount > 1 && p.coneAngleDegrees == 0))
        return false;
    if (p.evaluationBounds)
        for (std::size_t a = 0; a < 3; ++a)
            if (!std::isfinite((*p.evaluationBounds)[a * 2]) ||
                !std::isfinite((*p.evaluationBounds)[a * 2 + 1]) ||
                (*p.evaluationBounds)[a * 2] >= (*p.evaluationBounds)[a * 2 + 1])
                return false;
    return true;
}
bool GetEvaluationValid(const ThicknessEvaluation &e) noexcept
{
    return Nonnegative(e.lower) && Positive(e.upper) && e.lower <= e.upper &&
           Nonnegative(e.histogramRange[0]) && Positive(e.histogramRange[1]) &&
           e.histogramRange[0] < e.histogramRange[1] && e.histogramBins > 0 &&
           e.histogramBins <= 4096 && Nonnegative(e.minRegionArea);
}
bool GetDisplayValid(const ThicknessDisplay &d) noexcept
{
    return static_cast<unsigned>(d.mode) <= 1 && Nonnegative(d.range[0]) && Positive(d.range[1]) &&
           d.range[0] < d.range[1] && Nonnegative(d.opacity) && d.opacity <= 1;
}
bool GetConfigValid(const ThicknessConfig &c) noexcept
{
    return c.maxWorkingBytes >= 4096 && c.maxSamples > 0 && c.deadlineMilliseconds > 0 &&
           c.deadlineMilliseconds <= 3600000 && c.stopTimeoutMilliseconds <= 5000;
}
std::optional<double> GetValue(const Field &field, const ThicknessPoint &modelPoint)
{
    if (!field.nodes || field.nodes->empty() || !Finite(modelPoint)) return {};
    const auto &g = field.geometry;
    ThicknessPoint index{}, fraction{};
    std::array<std::int64_t, 3> cell{};
    unsigned nearest = 0;
    for (unsigned a = 0; a < 3; ++a)
    {
        for (unsigned k = 0; k < 3; ++k)
            index[a] += g.direction[k * 3 + a] * (modelPoint[k] - g.origin[k]) / g.spacing[a];
        if (!std::isfinite(index[a]) || index[a] < g.extent[2 * a] || index[a] > g.extent[2 * a + 1])
            return {};
        cell[a] = static_cast<std::int64_t>(std::floor(index[a]));
        fraction[a] = index[a] - double(cell[a]);
        if (fraction[a] >= 0.5) nearest |= 1U << a;
    }
    std::array<std::optional<double>, 8> values{};
    for (unsigned corner = 0; corner < 8; ++corner)
    {
        std::array<int, 3> key{};
        bool inside = true;
        for (unsigned a = 0; a < 3; ++a)
        {
            const auto node = cell[a] + ((corner >> a) & 1U);
            if (node < g.extent[2 * a] || node > g.extent[2 * a + 1]) { inside = false; break; }
            key[a] = static_cast<int>(node);
        }
        if (!inside) continue;
        const auto found = std::lower_bound(field.nodes->begin(), field.nodes->end(), key,
            [](const ThicknessNode &node, const auto &target) { return node.index < target; });
        if (found != field.nodes->end() && found->index == key && found->validWeight > 0 &&
            Positive(found->thickness)) values[corner] = found->thickness;
    }
    if (!values[nearest]) return {};
    // 顺序是合同的一部分；缺失角点不作全局权重归一化。
    std::size_t count = 8;
    for (unsigned a = 0; a < 3; ++a)
    {
        for (std::size_t i = 0; i < count / 2; ++i)
        {
            const auto left = values[2 * i], right = values[2 * i + 1];
            values[i] = !left ? right : !right ? left
                        : std::optional<double>((1 - fraction[a]) * *left + fraction[a] * *right);
        }
        count /= 2;
    }
    return values[0];
}
Candidate BuildField(const Work &w) noexcept
{
    try
    {
        if (!w.source || !w.labels || !w.mesh || !GetParamsValid(w.archive.params) ||
            !GetConfigValid(w.archive.limits) || !GetEvaluationValid(w.archive.evaluation) ||
            w.archive.input.materialLabel == 0 || static_cast<unsigned>(w.archive.input.unit) < 1 ||
            static_cast<unsigned>(w.archive.input.unit) > 2)
            return {ThicknessStatus::InvalidInput, {}, {}, {}, "Invalid thickness work input."};
        return Kernel(w).Build();
    }
    catch (const Failure &f)
    {
        return {f.status, {}, {}, {}, f.message};
    }
    catch (const std::bad_alloc &)
    {
        return {ThicknessStatus::BudgetExceeded, {}, {}, {}, "Thickness allocation failed."};
    }
    catch (const std::exception &e)
    {
        return {ThicknessStatus::InternalError, {}, {}, {}, e.what()};
    }
    catch (...)
    {
        return {ThicknessStatus::InternalError, {}, {}, {}, "Unexpected thickness failure."};
    }
}
ThicknessPoint GetSampleCorner(const SurfaceMeshPayload &mesh, const ThicknessSample &sample,
                               std::size_t corner)
{
    // 先减局部参考再加回，避免大原点重心和的放大误差。
    const auto first = mesh.GetTriangles().at(sample.sourceTriangle * 3);
    const auto *origin = mesh.GetVertices().data() + first * 3;
    ThicknessPoint point{origin[0], origin[1], origin[2]};
    for (std::size_t k = 0; k < 3; ++k)
    {
        const auto id = mesh.GetTriangles().at(sample.sourceTriangle * 3 + k);
        const auto *vertex = mesh.GetVertices().data() + id * 3;
        for (std::size_t a = 0; a < 3; ++a)
            point[a] += (vertex[a] - origin[a]) * sample.barycentricCorners.at(corner)[k];
    }
    return point;
}
} // namespace ThicknessAlgorithm
