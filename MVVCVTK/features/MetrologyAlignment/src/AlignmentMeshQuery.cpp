#include "AlignmentMeshQuery.h"
#include "AlignmentMath.h"
#include <algorithm>
#include <stdexcept>

namespace {
using namespace AlignmentMath;

// 三角形 Voronoi 区域求最近点，包含面内部、三条边及三个顶点。
Vec ClosestTriangle(const Vec &p, const Vec &a, const Vec &b, const Vec &c) {
    const auto ab = b - a, ac = c - a, ap = p - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const auto bp = p - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + (d1 / (d1 - d3)) * ab;
    const auto cp = p - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + (d2 / (d2 - d6)) * ac;
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 >= d3 && d5 >= d6)
        return b + ((d4 - d3) / ((d4 - d3) + (d5 - d6))) * (c - b);
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
    : m_mesh(std::move(mesh)), m_cancelled(work.cancelled), m_deadline(work.deadline) {
    CheckWork();
    if (!m_mesh || !m_mesh->GetValid() || m_mesh->GetTriangles().empty())
        throw std::invalid_argument("Reference requires a valid triangle surface.");
    const auto count = triangles.empty() ? m_mesh->GetTriangles().size() / 3 : triangles.size();
    const auto limit = work.config.workingBytes.value_or(std::numeric_limits<std::size_t>::max());
    // 中位数划分、叶最多八个三角形，非根叶至少四个；节点上界为 N/2+1。
    const auto nodeCount = count / 2 + 1;
    if (nodeCount > limit / sizeof(Node) ||
        count > (limit - nodeCount * sizeof(Node)) / sizeof(std::uint64_t))
        throw std::length_error("Triangle index exceeds the working budget.");
    m_triangles.reserve(count);
    if (!triangles.empty()) {
        m_triangles = triangles;
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
    for (std::size_t i = 0; i < m_triangles.size(); ++i) {
        if (i % 256 == 0) CheckWork();
        const auto t = GetTriangle(m_triangles[i]);
        const auto cross = (Vector(t[1]) - Vector(t[0])).cross(Vector(t[2]) - Vector(t[0]));
        if (!Finite(cross) || cross.dot(cross) <= 0 || !std::isfinite(cross.dot(cross)))
            throw std::invalid_argument("Reference contains a degenerate triangle.");
    }
    m_nodes.reserve(nodeCount);
    (void)BuildNode(0, count);
}

void AlignmentMeshQuery::CheckWork() const {
    if ((m_cancelled && m_cancelled->load(std::memory_order_relaxed)) ||
        std::chrono::steady_clock::now() >= m_deadline)
        throw std::runtime_error("Triangle query cancelled or deadline reached.");
}

std::array<AlignmentPoint, 3> AlignmentMeshQuery::GetTriangle(std::uint64_t triangle) const {
    const auto &ids = m_mesh->GetTriangles();
    if (triangle >= ids.size() / 3)
        throw std::invalid_argument("Reference triangle is out of bounds.");
    const auto &vertices = m_mesh->GetVertices();
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
    std::nth_element(m_triangles.begin() + first, m_triangles.begin() + middle,
                     m_triangles.begin() + first + count, [&](auto a, auto b) {
        if (++comparisons % 256 == 0) CheckWork();
        const auto ta = GetTriangle(a), tb = GetTriangle(b);
        const double ca = ta[0][axis] / 3 + ta[1][axis] / 3 + ta[2][axis] / 3;
        const double cb = tb[0][axis] / 3 + tb[1][axis] / 3 + tb[2][axis] / 3;
        return ca == cb ? a < b : ca < cb;
    });
    const auto left = BuildNode(first, middle - first);
    const auto right = BuildNode(middle, first + count - middle);
    m_nodes[index].left = left;
    m_nodes[index].right = right;
    m_nodes[index].count = 0;
    return index;
}

AlignmentMeshHit AlignmentMeshQuery::GetClosest(const AlignmentPoint &point) const {
    CheckWork();
    if (!Finite(Vector(point))) throw std::invalid_argument("Non-finite query position.");
    AlignmentMeshHit hit;
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
                const auto t = GetTriangle(id);
                const auto a = Vector(t[0]), b = Vector(t[1]), c = Vector(t[2]);
                const auto closest = ClosestTriangle(Vector(point), a, b, c);
                const auto delta = Vector(point) - closest;
                const double squared = delta.dot(delta);
                if (!std::isfinite(squared)) throw std::invalid_argument("Distance overflows.");
                if (!hit.isValid || squared < hit.distanceSquared ||
                    (squared == hit.distanceSquared && id < hit.triangle)) {
                    hit = {Point(closest), Point(Unit((b - a).cross(c - a))), id, squared, true};
                }
            }
        } else {
            auto near = node.left, far = node.right;
            if (BoundsDistance(point, m_nodes[near].bounds) >
                BoundsDistance(point, m_nodes[far].bounds)) std::swap(near, far);
            stack[pending++] = far;
            stack[pending++] = near;
        }
    }
    return hit;
}

std::size_t AlignmentMeshQuery::GetWorkingBytes() const noexcept {
    return m_nodes.capacity() * sizeof(Node) + m_triangles.capacity() * sizeof(std::uint64_t);
}
