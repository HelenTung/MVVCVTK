#include <cstring>
#include "AlignmentMeshQuery.h"
#include "AlignmentMath.h"
#include <algorithm>
#include <stdexcept>
#if MVVCVTK_METROLOGY_HAS_CUDA
#include "AlignmentCudaQuery.h"
#include <type_traits>
#endif

namespace {
using namespace AlignmentMath;

// 三角形 Voronoi 区域求最近点，包含面内部、三条边及三个顶点。
Vec ClosestTriangle(const Vec &p, const Vec &a, const Vec &b, const Vec &c,
                    Vec &edge, bool &isVertex) {
    const auto ab = b - a, ac = c - a, ap = p - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0 && d2 <= 0) { isVertex = true; return a; }
    const auto bp = p - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0 && d4 <= d3) { isVertex = true; return b; }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { edge = ab; return a + (d1 / (d1 - d3)) * ab; }
    const auto cp = p - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0 && d5 <= d6) { isVertex = true; return c; }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { edge = ac; return a + (d2 / (d2 - d6)) * ac; }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 >= d3 && d5 >= d6) {
        edge = c - b;
        return b + ((d4 - d3) / ((d4 - d3) + (d5 - d6))) * (c - b);
    }
    const double denominator = va + vb + vc;
    if (!std::isfinite(denominator) || denominator <= 0)
        throw std::invalid_argument("Triangle distance is numerically singular.");
    return a + (vb / denominator) * ab + (vc / denominator) * ac;
}

double BoundsDistance(const AlignmentPoint &p, const std::array<double, 6> &bounds) {
    double squared = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double d = std::max({bounds[2 * axis] - p[axis],
                                   p[axis] - bounds[2 * axis + 1], 0.0});
        squared += d * d;
    }
    return squared;
}
} // namespace

AlignmentMeshQuery::AlignmentMeshQuery(std::shared_ptr<const SurfaceMeshPayload> mesh,
                                       const AlignmentWork &work,
                                       const std::vector<std::uint64_t> &triangles)
    : m_mesh(std::move(mesh)), m_cancelled(work.cancelled) {
    CheckWork();
    if (!m_mesh || !m_mesh->GetValid() || m_mesh->GetTriangleIndices().empty())
        throw std::invalid_argument("Reference requires a valid triangle surface.");
    const auto count = triangles.empty() ? m_mesh->GetTriangleIndices().size() / 3 : triangles.size();
    const auto limit = std::numeric_limits<std::size_t>::max();
    // 中位数划分、叶最多八个三角形，非根叶至少四个；节点上界为 N/2+1。
    const auto nodeCount = count / 2 + 1;
    if (nodeCount > limit / sizeof(Node) ||
        count > (limit - nodeCount * sizeof(Node)) / sizeof(std::uint64_t))
        throw std::length_error("Triangle index byte count overflows.");
    m_triangles.reserve(count);
    if (!triangles.empty()) {
        m_triangles.assign(triangles.begin(),triangles.end());
        std::size_t comparisons = 0;
        std::sort(m_triangles.begin(), m_triangles.end(), [&](auto a, auto b) {
            if (++comparisons % 256 == 0) CheckWork();
            return a < b;
        });
        if (std::adjacent_find(m_triangles.begin(), m_triangles.end()) != m_triangles.end())
            throw std::invalid_argument("Duplicate reference triangle selection.");
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            if (i % 256 == 0) CheckWork();
            m_triangles.push_back(static_cast<std::uint64_t>(i));
        }
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < m_triangles.size(); ++i) {
        if (i % 256 == 0) CheckWork();
        if (!AlignmentGeometryFit::GetFaceValid(*m_mesh, m_triangles[i])) continue;
        const auto t = GetTriangle(m_triangles[i]);
        const auto cross = (Vector(t[1]) - Vector(t[0])).cross(Vector(t[2]) - Vector(t[0]));
        if (!Finite(cross) || cross.dot(cross) <= 0 || !std::isfinite(cross.dot(cross)))
            throw std::invalid_argument("Reference contains a degenerate triangle.");
        m_triangles[kept++] = m_triangles[i];
    }
    m_triangles.resize(kept);
    if (m_triangles.empty()) return;
    m_nodes.reserve(m_triangles.size() / 2 + 1);
    (void)BuildNode(0, m_triangles.size());
}

void AlignmentMeshQuery::CheckWork() const {
    if ((m_cancelled && m_cancelled->load(std::memory_order_relaxed)))
        throw std::runtime_error("Triangle query cancelled.");
}

std::array<AlignmentPoint, 3> AlignmentMeshQuery::GetTriangle(std::uint64_t triangle) const {
    const auto &ids = m_mesh->GetTriangleIndices();
    if (triangle >= ids.size() / 3)
        throw std::invalid_argument("Reference triangle is out of bounds.");
    const auto &vertices = m_mesh->GetPointValues();
    std::array<AlignmentPoint, 3> result{};
    for (std::size_t j = 0; j < 3; ++j) {
        const auto index = static_cast<std::size_t>(ids[3 * static_cast<std::size_t>(triangle) + j]);
        result[j] = {vertices[3 * index], vertices[3 * index + 1], vertices[3 * index + 2]};
    }
    return result;
}

std::size_t AlignmentMeshQuery::BuildNode(std::size_t first, std::size_t count) {
    CheckWork();
    Node node;
    node.first = first;
    node.count = count;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        node.bounds[2 * axis] = std::numeric_limits<double>::infinity();
        node.bounds[2 * axis + 1] = -std::numeric_limits<double>::infinity();
    }
    for (std::size_t i = first; i < first + count; ++i) {
        if (i % 256 == 0) CheckWork();
        for (const auto &p : GetTriangle(m_triangles[i]))
            for (std::size_t axis = 0; axis < 3; ++axis) {
                node.bounds[2 * axis] = std::min(node.bounds[2 * axis], p[axis]);
                node.bounds[2 * axis + 1] = std::max(node.bounds[2 * axis + 1], p[axis]);
            }
    }
    const auto index = m_nodes.size();
    m_nodes.push_back(node);
    if (count <= 8) return index;
    std::size_t axis = 0;
    for (std::size_t j = 1; j < 3; ++j)
        if (node.bounds[2 * j + 1] - node.bounds[2 * j] >
            node.bounds[2 * axis + 1] - node.bounds[2 * axis]) axis = j;
    const auto middle = first + count / 2;
    std::size_t comparisons = 0;
    const auto &vertices=m_mesh->GetPointValues();
    const auto &triangles=m_mesh->GetTriangleIndices();
    // 索引在构造阶段已校验；排序只读当前轴三坐标，不反复物化完整三角形。
    const auto center=[&](std::uint64_t triangle) {
        const auto base=3*triangle;
        return vertices[3*triangles[base]+axis]/3 + vertices[3*triangles[base+1]+axis]/3
            + vertices[3*triangles[base+2]+axis]/3;
    };
    std::nth_element(m_triangles.begin() + first, m_triangles.begin() + middle,
                     m_triangles.begin() + first + count, [&](auto a, auto b) {
        if (++comparisons % 256 == 0) CheckWork();
        const double ca=center(a),cb=center(b);
        return ca == cb ? a < b : ca < cb;
    });
    const auto left = BuildNode(first, middle - first);
    const auto right = BuildNode(middle, first + count - middle);
    m_nodes[index].left = left;
    m_nodes[index].right = right;
    m_nodes[index].count = 0;
    return index;
}

AlignmentMeshHit AlignmentMeshQuery::GetClosest(const AlignmentPoint &point, bool isObservabilityNeeded) const {
    CheckWork();
    if (!Finite(Vector(point))) throw std::invalid_argument("Non-finite query position.");
    AlignmentMeshHit hit;
    if (m_nodes.empty()) return hit;
    // 中位数树深度至多为 size_t 位数；固定栈避免逐测点堆分配。
    std::array<std::size_t, std::numeric_limits<std::size_t>::digits + 1> stack{};
    std::size_t pending = 1, visited = 0;
    while (pending) {
        if (++visited % 256 == 0) CheckWork();
        const auto &node = m_nodes[stack[--pending]];
        if (BoundsDistance(point, node.bounds) > hit.distanceSquared) continue;
        if (node.count) {
            for (std::size_t i = node.first; i < node.first + node.count; ++i) {
                const auto id = m_triangles[i];
                const auto candidate=GetHit(point,id);
                if (!hit.isValid || candidate.distanceSquared < hit.distanceSquared ||
                    (candidate.distanceSquared == hit.distanceSquared && id < hit.triangle)) hit=candidate;
            }
        } else {
            auto near = node.left, far = node.right;
            if (BoundsDistance(point, m_nodes[near].bounds) >
                BoundsDistance(point, m_nodes[far].bounds)) std::swap(near, far);
            stack[pending++] = far;
            stack[pending++] = near;
        }
    }
    return isObservabilityNeeded && hit.isValid ? GetHit(point, hit.triangle, true) : hit;
}

AlignmentMeshHit AlignmentMeshQuery::GetHit(const AlignmentPoint &point, std::uint64_t triangle, bool isObservabilityNeeded) const {
    if(!Finite(Vector(point)))throw std::invalid_argument("Non-finite query position.");
    const auto t=GetTriangle(triangle);
    if (!AlignmentGeometryFit::GetFaceValid(*m_mesh, triangle)) return {};
    const auto a=Vector(t[0]),b=Vector(t[1]),c=Vector(t[2]);
    Vec edge{};
    bool isVertex = false;
    const auto closest=ClosestTriangle(Vector(point),a,b,c,edge,isVertex);
    const auto delta=Vector(point)-closest;const double squared=delta.dot(delta);
    if(!std::isfinite(squared))throw std::invalid_argument("Distance overflows.");
    const auto normal = Unit((b-a).cross(c-a));
    AlignmentMeshHit hit{Point(closest), Point(normal), triangle, squared, true};
    if (!isObservabilityNeeded) return hit;
    hit.observableDirections[0] = Point(normal);
    hit.observableCount = 1;
    // 最近曲面点可沿面滑动。仅严格处于边/顶点的外侧 Voronoi 区域时，
    // 才增加边界约束；零距离接触边界不被当成双侧固定点约束。
    const auto tangential = delta - delta.dot(normal) * normal;
    const double roundoff = 64 * std::numeric_limits<double>::epsilon() *
        std::max({cv::norm(a), cv::norm(b), cv::norm(c), cv::norm(Vector(point)),
                  cv::norm(b-a), cv::norm(c-a), cv::norm(delta)});
    if (cv::norm(tangential) > roundoff) {
        if (isVertex) {
            hit.observableDirections = {{{1,0,0}, {0,1,0}, {0,0,1}}};
            hit.observableCount = 3;
        } else if (cv::norm(edge) > 0) {
            hit.observableDirections[1] = Point(Unit(edge.cross(normal)));
            hit.observableCount = 2;
        }
    }
    return hit;
}
std::size_t AlignmentMeshQuery::GetWorkingBytes() const noexcept {
    return m_nodes.GetResidentBytes()+m_triangles.GetResidentBytes();
}
#if MVVCVTK_METROLOGY_HAS_CUDA
std::unique_ptr<AlignmentCudaQuery> AlignmentMeshQuery::BuildCudaQuery(std::size_t batchCapacity) const {
    CheckWork();
    static_assert(std::is_standard_layout_v<Node> && sizeof(Node) == sizeof(AlignmentCudaNode));
    static_assert(offsetof(Node, first) == offsetof(AlignmentCudaNode, first) &&
                  offsetof(Node, count) == offsetof(AlignmentCudaNode, count) &&
                  offsetof(Node, left) == offsetof(AlignmentCudaNode, left) &&
                  offsetof(Node, right) == offsetof(AlignmentCudaNode, right));
    const auto nodes=ConvertMeshArray<AlignmentCudaNode>(m_nodes.GetReadSnapshot(),1,[](const Node& node,std::size_t){
        AlignmentCudaNode converted{};std::memcpy(&converted,&node,sizeof(converted));return converted;
    });
    return std::make_unique<AlignmentCudaQuery>(nodes,m_triangles.GetReadSnapshot(),m_mesh->GetPointValues(),m_mesh->GetTriangleIndices(),batchCapacity,
        [cancel=m_cancelled]{if(cancel && cancel->load())throw std::runtime_error("Triangle query cancelled.");});
}
#endif
