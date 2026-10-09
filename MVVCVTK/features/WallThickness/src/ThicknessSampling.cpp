#include "ThicknessSampling.h"
#include "ThicknessMath.h"
#include <map>
#include <tuple>

namespace ThicknessAlgorithm {
namespace {
using namespace ThicknessMath;
using Edge = std::pair<std::size_t, std::size_t>;
Edge Key(std::size_t a, std::size_t b) { return {std::min(a, b), std::max(a, b)}; }
void Check(const Work& work) {
    if (work.cancelled && work.cancelled->load()) throw ThicknessStatus::Cancelled;
    if (std::chrono::steady_clock::now() >= work.deadline) throw ThicknessStatus::DeadlineExceeded;
}
}
std::vector<SamplingTriangle> BuildSampling(const Work& work, const std::size_t limit)
{
    const auto& mesh = *work.mesh;
    const auto& params = work.archive.params;
    const auto& origin = work.labels->GetGeometry().origin;
    std::vector<ThicknessPoint> points;
    points.reserve(mesh.GetVertices().size() / 3);
    for (std::size_t i = 0; i < mesh.GetVertices().size(); i += 3)
        points.push_back(Sub({mesh.GetVertices()[i], mesh.GetVertices()[i+1], mesh.GetVertices()[i+2]}, origin));
    std::vector<SamplingTriangle> triangles;
    const auto append = [&](auto& target, std::size_t source, const std::array<SamplingCorner,3>& corners) {
        const auto area = Length(Cross(Sub(points[corners[1].vertex], points[corners[0].vertex]),
            Sub(points[corners[2].vertex], points[corners[0].vertex])));
        if (area == 0) return;
        if (!std::isfinite(area)) throw ThicknessStatus::InvalidInput;
        if (target.size() >= limit) throw ThicknessStatus::BudgetExceeded;
        target.push_back(SamplingTriangle{source, corners});
    };
    std::map<std::tuple<std::size_t,std::size_t,unsigned>, std::pair<std::size_t,double>> intersections;
    for (std::size_t source = 0; source < mesh.GetTriangles().size()/3; ++source) {
        Check(work);
        std::vector<SamplingCorner> polygon(3);
        for (unsigned k = 0; k < 3; ++k) {
            polygon[k].vertex = static_cast<std::size_t>(mesh.GetTriangles()[source*3+k]);
            polygon[k].barycentric[k] = 1;
        }
        if (params.evaluationBounds) for (unsigned plane = 0; plane < 6 && !polygon.empty(); ++plane) {
            const auto axis = plane / 2;
            const double bound = (*params.evaluationBounds)[plane] - origin[axis];
            const auto inside = [&](const SamplingCorner& p) {
                return plane % 2 ? points[p.vertex][axis] <= bound : points[p.vertex][axis] >= bound;
            };
            std::vector<SamplingCorner> clipped;
            for (std::size_t i = 0; i < polygon.size(); ++i) {
                const auto& a = polygon[i]; const auto& b = polygon[(i+1)%polygon.size()];
                const bool inA = inside(a), inB = inside(b);
                if (inA) clipped.push_back(a);
                if (inA == inB) continue;
                const auto edge = Key(a.vertex, b.vertex);
                const auto key = std::make_tuple(edge.first, edge.second, plane);
                auto found = intersections.find(key);
                if (found == intersections.end()) {
                    const auto& lo = points[edge.first]; const auto& hi = points[edge.second];
                    const double t = (bound-lo[axis])/(hi[axis]-lo[axis]);
                    if (t == 0 || t == 1) {
                        found = intersections.emplace(key, std::make_pair(t == 0 ? edge.first : edge.second, t)).first;
                    } else {
                        auto p = Add(lo, Scale(Sub(hi,lo),t)); p[axis] = bound;
                        const auto id = points.size(); points.push_back(p);
                        found = intersections.emplace(key, std::make_pair(id,t)).first;
                    }
                }
                const double t = a.vertex == edge.first ? found->second.second : 1-found->second.second;
                SamplingCorner corner{found->second.first, Add(a.barycentric, Scale(Sub(b.barycentric,a.barycentric),t))};
                if (clipped.empty() || clipped.back().vertex != corner.vertex) clipped.push_back(corner);
            }
            if (clipped.size() > 1 && clipped.front().vertex == clipped.back().vertex) clipped.pop_back();
            polygon = std::move(clipped);
        }
        for (std::size_t k = 1; k+1 < polygon.size(); ++k)
            append(triangles, source, {polygon[0],polygon[k],polygon[k+1]});
    }
    intersections.clear();
    // 每轮先确定所有共享边的拆分，再同时重建两侧，避免 T 接点。
    for (;;) {
        Check(work);
        std::map<Edge,std::size_t> midpoints;
        for (const auto& triangle : triangles) {
            Check(work);
            for (unsigned k = 0; k < 3; ++k) {
                const auto edge = Key(triangle.corners[k].vertex,triangle.corners[(k+1)%3].vertex);
                if (Length(Sub(points[edge.first],points[edge.second])) <= params.sampleSpacing) continue;
                if (midpoints.count(edge)) continue;
                auto midpoint = Scale(Add(points[edge.first], points[edge.second]),0.5);
                if (midpoint == points[edge.first] || midpoint == points[edge.second]) throw ThicknessStatus::InvalidInput;
                const auto id = points.size(); points.push_back(midpoint); midpoints.emplace(edge,id);
            }
        }
        if (midpoints.empty()) break;
        std::vector<SamplingTriangle> next;
        for (const auto& triangle : triangles) {
            Check(work);
            const auto& c = triangle.corners;
            std::array<SamplingCorner,3> m{};
            unsigned mask = 0;
            for (unsigned k = 0; k < 3; ++k) {
                const auto found = midpoints.find(Key(c[k].vertex,c[(k+1)%3].vertex));
                if (found == midpoints.end()) continue;
                mask |= 1U << k;
                m[k] = {found->second,Scale(Add(c[k].barycentric,c[(k+1)%3].barycentric),0.5)};
            }
            const auto add = [&](std::array<SamplingCorner,3> corners) { append(next,triangle.source,corners); };
            if (!mask) add(c);
            else if (mask == 7) {
                add({c[0],m[0],m[2]}); add({m[0],c[1],m[1]});
                add({m[2],m[1],c[2]}); add({m[0],m[1],m[2]});
            }
            else if (mask == 1 || mask == 2 || mask == 4) {
                const unsigned k = mask == 1 ? 0 : mask == 2 ? 1 : 2;
                add({c[k],m[k],c[(k+2)%3]}); add({m[k],c[(k+1)%3],c[(k+2)%3]});
            }
            else {
                const unsigned k = !(mask&1) ? 0 : !(mask&2) ? 1 : 2;
                add({c[(k+2)%3],m[(k+2)%3],m[(k+1)%3]});
                add({c[k],c[(k+1)%3],m[(k+2)%3]});
                add({c[(k+1)%3],m[(k+1)%3],m[(k+2)%3]});
            }
        }
        triangles = std::move(next);
    }
    return triangles;
}
}
