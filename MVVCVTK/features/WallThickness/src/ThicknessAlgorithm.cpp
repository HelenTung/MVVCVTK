#include "ThicknessAlgorithm.h"
#include "ThicknessMath.h"
#include <vtkCellArray.h>
#include <vtkGenericCell.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkStaticCellLocator.h>
#include <algorithm>
#include <cmath>
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

class Kernel final
{
    const Work &m_work;
    const ThicknessParams &m_params;
    const GridGeometry3D &m_geometry;
    const SurfaceMeshPayload &m_mesh;
    const MeshAttribute *m_valid = nullptr, *m_normal = nullptr, *m_support = nullptr,
                        *m_residual = nullptr, *m_sigma = nullptr;
    vtkSmartPointer<vtkPolyData> m_poly;
    vtkSmartPointer<vtkStaticCellLocator> m_locator;
    vtkSmartPointer<vtkGenericCell> m_cell = vtkSmartPointer<vtkGenericCell>::New();
    std::vector<double> m_areas;
    unsigned m_n = 1;
    std::size_t m_sampleCount = 0;
    double m_epsilon = 0, m_minSpacing = 0;

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
        const auto i = Index(p);
        std::array<std::int64_t, 3> cell{};
        for (std::size_t a = 0; a < 3; ++a)
        {
            if (!std::isfinite(i[a]) || i[a] < double(m_geometry.extent[a * 2]) - 0.5 ||
                i[a] > double(m_geometry.extent[a * 2 + 1]) + 0.5)
                return -2;
            cell[a] = static_cast<std::int64_t>(std::floor(i[a] + 0.5));
        }
        return Material(cell);
    }
    void ValidateInput()
    {
        if (!m_work.source->GetValid() || !m_work.labels->GetValid() || !m_mesh.GetValid() ||
            m_mesh.GetVertices().empty() || m_mesh.GetTriangles().empty())
            throw Failure{ThicknessStatus::InvalidInput, "Invalid input payload."};
        const auto &g = m_work.source->GetGeometry();
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
            if (a.name == "measurement.valid")
                m_valid = &a;
            if (a.name == "measurement.normal")
                m_normal = &a;
            if (a.name == "measurement.support-ratio")
                m_support = &a;
            if (a.name == "measurement.fit-residual")
                m_residual = &a;
            if (a.name == "measurement.localization-sigma")
                m_sigma = &a;
            if (a.name == "measurement.boundary-complete")
                complete = &a;
        }
        if (!complete || complete->componentCount != 1 ||
            !std::all_of(complete->values.begin(), complete->values.end(),
                         [](double v) { return v == 1; }))
            throw Failure{ThicknessStatus::IncompleteBoundary,
                          "Mesh lacks complete-boundary provenance."};
        if (!m_valid || !m_normal || !m_support || !m_residual || !m_sigma ||
            m_valid->componentCount != 1 || m_normal->componentCount != 3 ||
            m_support->componentCount != 1 || m_residual->componentCount != 1 ||
            m_sigma->componentCount != 1)
            throw Failure{ThicknessStatus::InvalidInput,
                          "Measurement quality attributes required."};
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
            if (e.second.first != 2 || e.second.second != 0)
                throw Failure{ThicknessStatus::IncompleteBoundary,
                              "Mesh must be closed and consistently manifold."};
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
        m_poly = vtkSmartPointer<vtkPolyData>::New();
        m_poly->SetPoints(points);
        m_poly->SetPolys(cells);
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
                    if (Material(i) != 1)
                        continue;
                    hasMaterial = true;
                    for (std::size_t a = 0; a < 3; ++a)
                        for (int sign : {-1, 1})
                        {
                            auto neighbor = i;
                            neighbor[a] += sign;
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
    bool Quality(std::uint64_t triangle, const ThicknessPoint &bary, ThicknessPoint &normal,
                 bool &sharp) const
    {
        normal = {};
        sharp = false;
        std::array<ThicknessPoint, 3> ns{};
        for (std::size_t k = 0; k < 3; ++k)
        {
            const auto id = m_mesh.GetTriangles()[triangle * 3 + k];
            const auto *n = m_normal->values.data() + id * 3;
            ns[k] = Unit({n[0], n[1], n[2]});
            if (m_valid->values[id] != 1 || m_support->values[id] < m_params.minSupportRatio ||
                m_support->values[id] > 1 || m_residual->values[id] < 0 ||
                m_residual->values[id] > m_params.maxFitResidual || m_sigma->values[id] < 0 ||
                m_sigma->values[id] > m_params.maxLocalizationSigma || Length(ns[k]) < 0.5)
                return false;
            normal = Add(normal, Scale(ns[k], bary[k]));
        }
        for (std::size_t k = 0; k < 3; ++k)
            if (Dot(ns[k], ns[(k + 1) % 3]) < m_params.sharpNormalCosine)
                sharp = true;
        normal = Unit(normal);
        return Length(normal) > 0.5;
    }
    bool Inward(const ThicknessPoint &p, ThicknessPoint &normal, bool &outside) const
    {
        const double probe = m_params.maxBoundaryError + 8 * m_epsilon;
        const int plus = MaterialAt(Add(p, Scale(normal, probe))),
                  minus = MaterialAt(Sub(p, Scale(normal, probe)));
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
    bool Intersect(const ThicknessPoint &p, const ThicknessPoint &direction, Hit &hit)
    {
        const auto start = Add(p, Scale(direction, 2 * m_epsilon)),
                   end = Add(p, Scale(direction, m_params.maxDistance));
        if (!Finite(start) || !Finite(end))
            throw Failure{ThicknessStatus::InvalidInput, "Ray coordinate overflow."};
        double t = 0, point[3]{}, pc[3]{};
        int subId = 0;
        vtkIdType cellId = -1;
        if (!m_locator->IntersectWithLine(start.data(), end.data(), m_epsilon, t, point, pc, subId,
                                          cellId, m_cell) ||
            cellId < 0)
            return false;
        hit.point = {point[0], point[1], point[2]};
        hit.bary = {1 - pc[0] - pc[1], pc[0], pc[1]};
        hit.triangle = static_cast<std::uint64_t>(cellId);
        hit.distance = Length(Sub(hit.point, p));
        return std::isfinite(hit.distance) && hit.distance > 4 * m_epsilon &&
               hit.distance <= m_params.maxDistance + m_epsilon;
    }
    ThicknessValidity Path(const ThicknessPoint &p, const ThicknessPoint &q,
                           std::array<double, 2> &trim)
    {
        const double length = Length(Sub(q, p));
        if (length <= 2 * m_params.maxBoundaryError + 4 * m_epsilon)
            return ThicknessValidity::InvalidMaterialPath;
        const auto from = Index(p), delta = Sub(Index(q), from);
        if (!Finite(from) || !Finite(delta))
            throw Failure{ThicknessStatus::InvalidInput, "Index coordinate overflow."};
        const double begin = m_epsilon / length, end = 1 - begin;
        std::array<std::int64_t, 3> cell{};
        std::array<double, 3> next{}, step{};
        std::array<int, 3> sign{};
        for (std::size_t a = 0; a < 3; ++a)
        {
            const double initial = from[a] + begin * delta[a];
            if (initial < double(m_geometry.extent[a * 2]) - 1 ||
                initial > double(m_geometry.extent[a * 2 + 1]) + 1)
                return ThicknessValidity::TruncatedSupport;
            cell[a] = static_cast<std::int64_t>(std::floor(std::nextafter(
                initial + 0.5, delta[a] < 0 ? -std::numeric_limits<double>::infinity()
                                            : std::numeric_limits<double>::infinity())));
            sign[a] = delta[a] > 0 ? 1 : delta[a] < 0 ? -1 : 0;
            if (sign[a])
            {
                next[a] = (double(cell[a]) + 0.5 * sign[a] - from[a]) / delta[a];
                step[a] = 1 / std::abs(delta[a]);
            }
            else
                next[a] = step[a] = std::numeric_limits<double>::infinity();
        }
        bool hasMaterial = false, hasEnded = false;
        double first = 0, last = 0, t = begin;
        std::size_t count = 0;
        const auto limit = static_cast<std::size_t>(m_geometry.dimensions[0]) +
                           static_cast<std::size_t>(m_geometry.dimensions[1]) +
                           static_cast<std::size_t>(m_geometry.dimensions[2]) + 16;
        while (t < end)
        {
            if ((count++ & 63U) == 0)
                CheckWork(m_work);
            if (count > limit)
                return ThicknessValidity::TruncatedSupport;
            const double stop = std::min(end, *std::min_element(next.begin(), next.end()));
            if (stop > t)
            {
                const int label = Material(cell);
                if (label == -2)
                    return ThicknessValidity::TruncatedSupport;
                if (label == -1)
                    return ThicknessValidity::InvalidMaterialPath;
                if (label == 1)
                {
                    if (hasEnded)
                        return ThicknessValidity::InvalidMaterialPath;
                    if (!hasMaterial)
                        first = t;
                    hasMaterial = true;
                    last = stop;
                }
                else if (hasMaterial)
                    hasEnded = true;
            }
            if (stop >= end)
                break;
            bool moved = false;
            for (std::size_t a = 0; a < 3; ++a)
                if (next[a] <= stop + 8 * std::numeric_limits<double>::epsilon())
                {
                    cell[a] += sign[a];
                    next[a] += step[a];
                    moved = true;
                }
            if (!moved)
                return ThicknessValidity::InvalidMaterialPath;
            t = std::max(t, stop);
        }
        trim = {std::max(0.0, first * length - m_epsilon),
                std::max(0.0, (1 - last) * length - m_epsilon)};
        return hasMaterial && trim[0] <= m_params.maxBoundaryError + m_epsilon &&
                       trim[1] <= m_params.maxBoundaryError + m_epsilon
                   ? ThicknessValidity::Valid
                   : ThicknessValidity::InvalidMaterialPath;
    }
    void Measure(ThicknessSample &sample, const ThicknessPoint &bary, const ThicknessPoint &p)
    {
        ThicknessPoint normal{};
        bool sharp = false;
        if (!Quality(sample.sourceTriangle, bary, normal, sharp))
        {
            const auto i = sample.sourceTriangle * 3;
            normal = Unit(
                Cross(Sub(Vertex(m_mesh.GetTriangles()[i + 1]), Vertex(m_mesh.GetTriangles()[i])),
                      Sub(Vertex(m_mesh.GetTriangles()[i + 2]), Vertex(m_mesh.GetTriangles()[i]))));
            bool outside = false;
            Inward(p, normal, outside);
            sample.validity = outside ? ThicknessValidity::OutsideEvaluation
                                      : ThicknessValidity::LowSurfaceQuality;
            return;
        }
        bool outside = false;
        if (!Inward(p, normal, outside))
        {
            if (outside)
            {
                const auto i = sample.sourceTriangle * 3;
                auto geometric = Unit(Cross(
                    Sub(Vertex(m_mesh.GetTriangles()[i + 1]), Vertex(m_mesh.GetTriangles()[i])),
                    Sub(Vertex(m_mesh.GetTriangles()[i + 2]), Vertex(m_mesh.GetTriangles()[i]))));
                bool geometryOutside = false;
                Inward(p, geometric, geometryOutside);
                outside = geometryOutside;
            }
            sample.validity = outside ? ThicknessValidity::OutsideEvaluation
                                      : ThicknessValidity::InvalidMaterialPath;
            return;
        }
        if (sharp)
        {
            sample.validity = ThicknessValidity::SharpFeature;
            return;
        }
        std::size_t axis = 0;
        for (std::size_t k = 1; k < 3; ++k)
            if (std::abs(normal[k]) < std::abs(normal[axis]))
                axis = k;
        ThicknessPoint ref{};
        ref[axis] = 1;
        const auto u = Unit(Cross(normal, ref)), v = Cross(normal, u);
        bool hasValid = false, hasConflict = false;
        double minimum = 0, maximum = 0;
        sample.validity = ThicknessValidity::SearchLimitReached;
        for (std::uint32_t di = 0; di < m_params.directionCount; ++di)
        {
            CheckWork(m_work);
            auto direction = normal;
            if (di)
            {
                const double theta = m_params.coneAngleDegrees * pi / 180,
                             phi = 2 * pi * (di - 1) / (m_params.directionCount - 1);
                direction = Add(
                    Scale(normal, std::cos(theta)),
                    Scale(Add(Scale(u, std::cos(phi)), Scale(v, std::sin(phi))), std::sin(theta)));
            }
            Hit forward;
            if (!Intersect(p, direction, forward))
                continue;
            ThicknessPoint oppositeNormal{};
            bool oppositeSharp = false;
            if (!Quality(forward.triangle, forward.bary, oppositeNormal, oppositeSharp))
            {
                sample.validity = ThicknessValidity::LowSurfaceQuality;
                hasConflict = true;
                continue;
            }
            if (oppositeSharp)
            {
                sample.validity = ThicknessValidity::SharpFeature;
                hasConflict = true;
                continue;
            }
            if (!Inward(forward.point, oppositeNormal, outside) ||
                Dot(direction, oppositeNormal) > -m_params.minOppositeCosine)
            {
                sample.validity = ThicknessValidity::AmbiguousOpposite;
                hasConflict = true;
                continue;
            }
            std::array<double, 2> trim{};
            const auto path = Path(p, forward.point, trim);
            if (path != ThicknessValidity::Valid)
            {
                sample.validity = path;
                hasConflict = true;
                continue;
            }
            Hit reverse;
            if (!Intersect(forward.point, oppositeNormal, reverse) ||
                Length(Sub(reverse.point, p)) > m_params.reverseTolerance ||
                std::abs(reverse.distance - forward.distance) > m_params.reverseTolerance)
            {
                if (!hasValid)
                    sample.validity = ThicknessValidity::AmbiguousOpposite;
                continue;
            }
            ThicknessPoint reverseNormal{};
            bool reverseSharp = false;
            std::array<double, 2> reverseTrim{};
            if (!Quality(reverse.triangle, reverse.bary, reverseNormal, reverseSharp) ||
                reverseSharp ||
                Path(forward.point, reverse.point, reverseTrim) != ThicknessValidity::Valid)
            {
                sample.validity = ThicknessValidity::LowSurfaceQuality;
                hasConflict = true;
                continue;
            }
            if (!hasValid || forward.distance < minimum)
            {
                sample.thickness = forward.distance;
                sample.opposite = Add(forward.point, m_geometry.origin);
                sample.oppositeTriangle = forward.triangle;
                sample.directionIndex = di;
                sample.endpointTrim = trim;
                minimum = forward.distance;
            }
            maximum = hasValid ? std::max(maximum, forward.distance) : forward.distance;
            hasValid = true;
        }
        if (hasValid && !hasConflict)
            sample.validity = maximum - minimum > std::max(m_params.ambiguityAbsolute,
                                                           m_params.ambiguityRelative * minimum)
                                  ? ThicknessValidity::AmbiguousOpposite
                                  : ThicknessValidity::Valid;
        if (sample.validity != ThicknessValidity::Valid)
        {
            sample.thickness = 0;
            sample.opposite = {};
            sample.endpointTrim = {};
        }
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
                Measure(sample, bary, p);
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
        Field field{std::move(samples), std::move(neighbors), m_n};
        auto result = BuildEvaluation(field, m_work.archive.evaluation, m_params,
                                      m_work.archive.limits, m_work.cancelled, m_work.deadline);
        result.field = std::move(field);
        return result;
    }
};
} // namespace
bool GetParamsValid(const ThicknessParams &p) noexcept
{
    if (!Positive(p.maxDistance) || !Positive(p.sampleSpacing) || !Positive(p.reverseTolerance) ||
        p.reverseTolerance > p.maxDistance || !Nonnegative(p.maxFitResidual) ||
        !Nonnegative(p.maxLocalizationSigma) || !Positive(p.minSupportRatio) ||
        p.minSupportRatio > 1 || !Positive(p.minOppositeCosine) || p.minOppositeCosine > 1 ||
        !Positive(p.sharpNormalCosine) || p.sharpNormalCosine > 1 ||
        !Nonnegative(p.maxBoundaryError) || !Nonnegative(p.ambiguityAbsolute) ||
        !Nonnegative(p.ambiguityRelative) || p.ambiguityRelative > 1 ||
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
