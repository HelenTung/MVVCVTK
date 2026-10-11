#include "AlignmentPrealign.h"
#include "AlignmentMath.h"
#include <numeric>
#include <random>
#include <stdexcept>

namespace {
using namespace AlignmentMath;
using Neighbor = std::pair<double, std::size_t>;
constexpr auto missing = std::numeric_limits<std::size_t>::max();
const double pi = std::acos(-1.0);
void Require(bool value, const char *message) { if (!value) throw std::invalid_argument(message); }
void CheckWork(const AlignmentWork &work) {
    if (AlignmentGeometryFit::GetWorkStatus(work) != AlignmentStatus::FullyDetermined)
        throw std::runtime_error("Automatic prealignment cancelled.");
}

// 三维邻域与 33 维描述子共用精确索引；只保存行号/包围盒，不复制输入数组。
template<std::size_t Dimension> class PointIndex final {
    using Row = std::array<double, Dimension>;
    struct Node final {
        std::array<double, 2 * Dimension> bounds{};
        std::size_t first = 0, count = 0, left = 0, right = 0;
    };
  public:
    PointIndex(const std::vector<Row> &points, const AlignmentWork &work) : m_points(points), m_work(work) {
        Require(!points.empty(), "Empty search cloud.");
        m_order.resize(points.size());
        std::iota(m_order.begin(), m_order.end(), 0);
        m_nodes.reserve(points.size() / 4 + 1); // 中位数划分，叶最多16行，非根叶至少8行。
        Build(0, points.size());
    }
    void Nearest(const Row &point, std::size_t limit, double radiusSquared,
                 std::vector<Neighbor> &result, std::size_t exclude = missing) const {
        result.clear();
        if (!limit) return;
        std::array<std::size_t, std::numeric_limits<std::size_t>::digits + 1> stack{};
        std::size_t pending = 1, visited = 0;
        while (pending) {
            if (++visited % 256 == 0) CheckWork(m_work);
            const auto &node = m_nodes[stack[--pending]];
            const double distanceLimit = result.size() == limit ? std::min(radiusSquared, result.front().first) : radiusSquared;
            if (BoundsDistance(point, node.bounds) > distanceLimit) continue;
            if (node.count) {
                for (std::size_t i = node.first; i < node.first + node.count; ++i) {
                    const auto id = m_order[i];
                    if (id == exclude) continue;
                    double squared = 0;
                    for (std::size_t j = 0; j < Dimension; ++j) {
                        const double d = point[j] - m_points[id][j]; squared += d * d;
                    }
                    const Neighbor candidate{squared, id};
                    if (!std::isfinite(squared) || squared > radiusSquared) continue;
                    if (result.size() < limit) { result.push_back(candidate); std::push_heap(result.begin(), result.end()); }
                    else if (candidate < result.front()) {
                        std::pop_heap(result.begin(), result.end()); result.back() = candidate;
                        std::push_heap(result.begin(), result.end());
                    }
                }
            } else {
                auto near = node.left, far = node.right;
                if (BoundsDistance(point, m_nodes[near].bounds) > BoundsDistance(point, m_nodes[far].bounds))
                    std::swap(near, far);
                stack[pending++] = far; stack[pending++] = near;
            }
        }
        std::sort_heap(result.begin(), result.end());
    }
  private:
    static double BoundsDistance(const Row &point, const std::array<double, 2 * Dimension> &bounds) {
        double sum = 0;
        for (std::size_t j = 0; j < Dimension; ++j) {
            const double d = std::max({bounds[2 * j] - point[j], point[j] - bounds[2 * j + 1], 0.0});
            sum += d * d;
        }
        return sum;
    }
    std::size_t Build(std::size_t first, std::size_t count) {
        CheckWork(m_work);
        Node node; node.first = first; node.count = count;
        for (std::size_t j = 0; j < Dimension; ++j) {
            node.bounds[2 * j] = std::numeric_limits<double>::infinity();
            node.bounds[2 * j + 1] = -std::numeric_limits<double>::infinity();
        }
        for (std::size_t i = first; i < first + count; ++i) {
            if (i % 256 == 0) CheckWork(m_work);
            for (std::size_t j = 0; j < Dimension; ++j) {
                const auto value = m_points[m_order[i]][j];
                Require(std::isfinite(value), "Non-finite search coordinate.");
                node.bounds[2 * j] = std::min(node.bounds[2 * j], value);
                node.bounds[2 * j + 1] = std::max(node.bounds[2 * j + 1], value);
            }
        }
        const auto id = m_nodes.size(); m_nodes.push_back(node);
        if (count <= 16) return id;
        std::size_t axis = 0;
        for (std::size_t j = 1; j < Dimension; ++j)
            if (node.bounds[2 * j + 1] - node.bounds[2 * j] > node.bounds[2 * axis + 1] - node.bounds[2 * axis]) axis = j;
        const auto middle = first + count / 2;
        std::size_t comparisons = 0;
        std::nth_element(m_order.begin() + first, m_order.begin() + middle, m_order.begin() + first + count,
            [&](auto a, auto b) {
                if (++comparisons % 256 == 0) CheckWork(m_work);
                return m_points[a][axis] == m_points[b][axis] ? a < b : m_points[a][axis] < m_points[b][axis];
            });
        const auto left = Build(first, middle - first), right = Build(middle, first + count - middle);
        m_nodes[id].left = left; m_nodes[id].right = right; m_nodes[id].count = 0;
        return id;
    }
    const std::vector<Row> &m_points;
    const AlignmentWork &m_work;
    std::vector<std::size_t> m_order;
    std::vector<Node> m_nodes;
};

void CheckBudget(const AlignmentWork &work, std::size_t source, std::size_t reference, bool withMatches = true) {
    const auto &p = *work.recipe.prealignment;
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (source > std::numeric_limits<std::size_t>::max() - reference)
        throw std::length_error("Prealignment sample count overflows.");
    const auto count = source + reference;
    // 两组点/法向、SPFH/FPFH、有效性、两种索引上界，以及有界匹配表。
    constexpr std::size_t pointBytes = 2 * sizeof(AlignmentPoint) + 2 * sizeof(AlignmentDescriptor) +
        2 + 5 * sizeof(std::size_t) + (2 * 33 * sizeof(double) + 4 * sizeof(std::size_t)) / 4 +
        (2 * 3 * sizeof(double) + 4 * sizeof(std::size_t)) / 4;
    std::size_t bytes = 16384; // 固定小矩阵、随机数状态与遍历栈；非进程 RSS 承诺。
    const auto charge = [&](std::size_t elements, std::size_t width) {
        if (bytes > limit || elements > (limit - bytes) / width)
            throw std::length_error("Prealignment size accounting overflows; increase sample spacing.");
        bytes += elements * width;
    };
    charge(count, pointBytes);
    if (withMatches) charge(source, p.matchCount * sizeof(std::size_t));
    charge(p.neighborLimit + (withMatches ? p.matchCount : 0), 2 * sizeof(Neighbor));
    charge(p.sourceTriangles.size(), sizeof(std::uint64_t));
    charge(p.referenceTriangles.size(), sizeof(std::uint64_t));
}
struct Face final { std::array<Vec, 3> points{}; std::array<std::size_t, 3> ids{}; Vec normal{}; double area = 0; };
Face GetFace(const SurfaceMeshPayload &mesh, std::uint64_t id) {
    Require(id < mesh.GetTriangles().size() / 3, "Prealignment triangle selection is out of bounds.");
    Face face;
    const auto &vertices = mesh.GetVertices();
    for (std::size_t j = 0; j < 3; ++j) {
        face.ids[j] = static_cast<std::size_t>(mesh.GetTriangles()[3 * id + j]);
        const auto offset = 3 * face.ids[j];
        face.points[j] = {vertices[offset], vertices[offset + 1], vertices[offset + 2]};
    }
    const auto cross = (face.points[1] - face.points[0]).cross(face.points[2] - face.points[0]);
    const auto length = cv::norm(cross);
    Require(std::isfinite(length) && length > 0, "Prealignment contains a degenerate triangle.");
    face.area = length / 2; face.normal = cross / length;
    return face;
}
bool GetFaceUsable(const AlignmentWork &work, const Face &face, bool source) {
    return !source || (AlignmentGeometryFit::GetPointUsable(work, face.ids[0]) &&
        AlignmentGeometryFit::GetPointUsable(work, face.ids[1]) && AlignmentGeometryFit::GetPointUsable(work, face.ids[2]));
}
struct SamplePlan final {
    std::vector<std::uint64_t> selected;
    double area = 0, rejectedArea = 0;
    std::size_t count = 0, rejected = 0;
};
SamplePlan PlanSamples(const SurfaceMeshPayload &mesh, const std::vector<std::uint64_t> &selection,
                       const AlignmentWork &work, bool source) {
    CheckWork(work);
    Require(mesh.GetValid() && !mesh.GetTriangles().empty(), "Prealignment requires a triangle mesh.");
    SamplePlan plan; plan.selected = selection;
    std::size_t comparisons = 0;
    std::sort(plan.selected.begin(), plan.selected.end(), [&](auto a, auto b) {
        if (++comparisons % 256 == 0) CheckWork(work); return a < b;
    });
    Require(std::adjacent_find(plan.selected.begin(), plan.selected.end()) == plan.selected.end(),
            "Duplicate prealignment triangle selection.");
    const auto count = plan.selected.empty() ? mesh.GetTriangles().size() / 3 : plan.selected.size();
    double correction = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (i % 256 == 0) CheckWork(work);
        const auto face = GetFace(mesh, plan.selected.empty() ? i : plan.selected[i]);
        if (!GetFaceUsable(work, face, source)) {
            ++plan.rejected; plan.rejectedArea += face.area; continue;
        }
        const double adjusted = face.area - correction, next = plan.area + adjusted;
        correction = (next - plan.area) - adjusted; plan.area = next;
    }
    const auto spacing = work.recipe.prealignment->sampleSpacing;
    const double desired = std::ceil(plan.area / (spacing * spacing));
    if (!std::isfinite(desired) || !std::isfinite(plan.area + plan.rejectedArea) ||
        desired > static_cast<double>(std::numeric_limits<std::size_t>::max() / 1024))
        throw std::length_error("Prealignment sampling density is too high.");
    plan.count = static_cast<std::size_t>(desired);
    return plan;
}
double RadicalInverse(std::size_t value, std::size_t base) {
    double result = 0, weight = 1;
    while (value) { weight /= static_cast<double>(base); result += weight * static_cast<double>(value % base); value /= base; }
    return result;
}
AlignmentFeatureCloud SampleMesh(const SurfaceMeshPayload &mesh, const SamplePlan &plan,
                                 const AlignmentWork &work, bool source) {
    AlignmentFeatureCloud cloud;
    cloud.points.reserve(plan.count); cloud.normals.reserve(plan.count);
    const auto count = plan.selected.empty() ? mesh.GetTriangles().size() / 3 : plan.selected.size();
    double cumulative = 0, correction = 0;
    std::size_t sample = 0;
    for (std::size_t i = 0; i < count && sample < plan.count; ++i) {
        if (i % 256 == 0) CheckWork(work);
        const auto face = GetFace(mesh, plan.selected.empty() ? i : plan.selected[i]);
        if (!GetFaceUsable(work, face, source)) continue;
        const double adjusted = face.area - correction, next = cumulative + adjusted;
        correction = (next - cumulative) - adjusted; cumulative = next;
        while (sample < plan.count && (static_cast<double>(sample) + 0.5) * (plan.area / plan.count) < cumulative) {
            if (sample % 256 == 0) CheckWork(work);
            const double root = std::sqrt(RadicalInverse(sample + 1, 2)), v = RadicalInverse(sample + 1, 3);
            cloud.points.push_back(Point((1 - root) * face.points[0] + root * (1 - v) * face.points[1] + root * v * face.points[2]));
            cloud.normals.push_back(Point(face.normal));
            ++sample;
        }
    }
    Require(sample == plan.count, "Surface area sampling did not cover the requested density.");
    return cloud;
}
void EstimateNormals(AlignmentFeatureCloud &cloud, const PointIndex<3> &index, const AlignmentWork &work) {
    const auto &p = *work.recipe.prealignment;
    std::vector<Neighbor> neighbors; neighbors.reserve(p.neighborLimit);
    for (std::size_t i = 0; i < cloud.points.size(); ++i) {
        CheckWork(work);
        const auto hint = Vector(cloud.normals[i]); cloud.normals[i] = {};
        if (!Finite(hint) || std::abs(cv::norm(hint) - 1) > 1e-8) continue;
        index.Nearest(cloud.points[i], p.neighborLimit, p.normalRadius * p.normalRadius, neighbors, i);
        if (neighbors.size() < 3) continue;
        const auto origin = Vector(cloud.points[i]);
        Vec center{};
        for (const auto &neighbor : neighbors) center += (Vector(cloud.points[neighbor.second]) - origin) / p.normalRadius;
        center /= static_cast<double>(neighbors.size() + 1);
        cv::Matx33d covariance = cv::Matx33d::zeros();
        const auto add = [&](const Vec &point) {
            const auto q = point - center;
            for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) covariance(row, col) += q[row] * q[col];
        };
        add({});
        for (const auto &neighbor : neighbors) add((Vector(cloud.points[neighbor.second]) - origin) / p.normalRadius);
        Vec eigenvalues; cv::Matx33d eigenvectors;
        if (!cv::eigen(covariance, eigenvalues, eigenvectors) || eigenvalues[0] <= 0 ||
            eigenvalues[1] <= work.recipe.rankTolerance * eigenvalues[0] ||
            eigenvalues[1] - eigenvalues[2] <= work.recipe.rankTolerance * eigenvalues[0]) continue;
        auto n = Vec(eigenvectors(2, 0), eigenvectors(2, 1), eigenvectors(2, 2));
        const double direction = n.dot(hint);
        if (std::abs(direction) <= work.recipe.rankTolerance) continue;
        if (direction < 0) n = -n;
        cloud.normals[i] = Point(Unit(n));
    }
}
std::optional<std::array<double, 3>> PairAngles(const AlignmentPoint &a, const AlignmentPoint &na,
                                               const AlignmentPoint &b, const AlignmentPoint &nb,
                                               double tolerance) {
    auto d = Vector(b) - Vector(a), u = Vector(na), other = Vector(nb);
    const double length = cv::norm(d);
    if (!Finite(u) || !Finite(other) || !std::isfinite(length) || length <= 0 || std::abs(cv::norm(u) - 1) > 1e-8 ||
        std::abs(cv::norm(other) - 1) > 1e-8) return {};
    d /= length;
    if (std::abs(u.dot(d)) < std::abs(other.dot(d))) { std::swap(u, other); d = -d; }
    auto v = d.cross(u);
    const double width = cv::norm(v);
    if (width <= tolerance) return {};
    v /= width;
    const auto w = u.cross(v);
    return std::array<double, 3>{std::atan2(w.dot(other), u.dot(other)),
                                std::clamp(v.dot(other), -1.0, 1.0), std::clamp(u.dot(d), -1.0, 1.0)};
}
std::optional<AlignmentMatrix> FitTriple(const AlignmentFeatureCloud &source, const AlignmentFeatureCloud &target,
                                        const std::array<std::size_t, 3> &a, const std::array<std::size_t, 3> &b,
                                        const AlignmentWork &work) {
    const auto &p = *work.recipe.prealignment;
    for (std::size_t i = 0; i < 3; ++i) for (std::size_t j = i + 1; j < 3; ++j) {
        const double x = cv::norm(Vector(source.points[a[i]]) - Vector(source.points[a[j]]));
        const double y = cv::norm(Vector(target.points[b[i]]) - Vector(target.points[b[j]]));
        if (x <= 0 || y <= 0 || x < p.edgeLengthRatio * y || y < p.edgeLengthRatio * x) return {};
    }
    Vec sourceCenter{}, targetCenter{};
    for (std::size_t i = 0; i < 3; ++i) { sourceCenter += Vector(source.points[a[i]]) / 3; targetCenter += Vector(target.points[b[i]]) / 3; }
    cv::Matx33d h = cv::Matx33d::zeros();
    for (std::size_t i = 0; i < 3; ++i) {
        const auto x = (Vector(source.points[a[i]]) - sourceCenter) / p.sampleSpacing;
        const auto y = (Vector(target.points[b[i]]) - targetCenter) / p.sampleSpacing;
        for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) h(row, col) += x[row] * y[col];
    }
    cv::SVD svd(cv::Mat(h), cv::SVD::FULL_UV);
    if (svd.w.at<double>(0) <= 0 || svd.w.at<double>(1) <= work.recipe.rankTolerance * svd.w.at<double>(0)) return {};
    cv::Mat v = svd.vt.t(), rotation = v * svd.u.t();
    if (cv::determinant(rotation) < 0) { v.col(2) *= -1; rotation = v * svd.u.t(); }
    AlignmentMatrix pose = alignmentIdentity;
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) pose[4 * row + col] = rotation.at<double>(row, col);
    const auto t = targetCenter - Rotate(pose, sourceCenter);
    pose[3] = t[0]; pose[7] = t[1]; pose[11] = t[2];
    if (!Rigid(pose)) return {};
    for (std::size_t i = 0; i < 3; ++i)
        if (cv::norm(Transform(pose, Vector(source.points[a[i]])) - Vector(target.points[b[i]])) > p.maxPairDistance) return {};
    return pose;
}
} // namespace

bool AlignmentPrealign::GetParamsValid(const AlignmentPrealignParams &p) {
    const auto length = [](double value) { return std::isfinite(value) && value > 0 &&
        std::isfinite(value * value) && value * value > 0; };
    return length(p.sampleSpacing) && length(p.normalRadius) && length(p.featureRadius) && length(p.maxPairDistance) &&
        std::isfinite(p.minCoverage) && p.minCoverage > 0 && p.minCoverage <= 1 &&
        std::isfinite(p.edgeLengthRatio) && p.edgeLengthRatio > 0 && p.edgeLengthRatio <= 1 &&
        std::isfinite(p.candidateSeparation) && p.candidateSeparation >= 0 &&
        p.neighborLimit >= 3 && p.neighborLimit <= 4096 && p.matchCount > 0 && p.matchCount <= 64 &&
        p.iterationLimit > 0 && p.candidateLimit > 0 && p.candidateLimit <= 8;
}

void AlignmentPrealign::BuildFeatures(AlignmentFeatureCloud &cloud, const AlignmentWork &work, bool estimateNormals) {
    CheckWork(work);
    Require(work.recipe.prealignment && GetParamsValid(*work.recipe.prealignment), "Invalid FPFH parameters.");
    Require(cloud.points.size() == cloud.normals.size(), "FPFH normal count mismatch.");
    CheckBudget(work, cloud.points.size(), 0, false);
    cloud.descriptors.assign(cloud.points.size(), {}); cloud.valid.assign(cloud.points.size(), 0);
    if (cloud.points.empty()) return;
    const auto &p = *work.recipe.prealignment;
    PointIndex<3> index(cloud.points, work);
    if (estimateNormals) EstimateNormals(cloud, index, work);
    std::vector<AlignmentDescriptor> spfh(cloud.points.size());
    std::vector<Neighbor> neighbors; neighbors.reserve(p.neighborLimit);
    // Darboux 三角量顺序为 theta/alpha/phi，各11箱。退化点对不伪造零角度样本。
    for (std::size_t i = 0; i < cloud.points.size(); ++i) {
        CheckWork(work);
        index.Nearest(cloud.points[i], p.neighborLimit, p.featureRadius * p.featureRadius, neighbors, i);
        std::size_t pairs = 0;
        for (const auto &neighbor : neighbors) {
            const auto j = neighbor.second;
            const auto angles = PairAngles(cloud.points[i], cloud.normals[i], cloud.points[j], cloud.normals[j], work.recipe.rankTolerance);
            if (!angles) continue;
            for (std::size_t group = 0; group < 3; ++group) {
                const double value = group == 0 ? ((*angles)[group] + pi) / (2 * pi) : ((*angles)[group] + 1) / 2;
                const auto bin = static_cast<std::size_t>(std::clamp(static_cast<int>(std::floor(11 * value)), 0, 10));
                spfh[i][group * 11 + bin] += 1;
            }
            ++pairs;
        }
        if (!pairs) continue;
        for (auto &value : spfh[i]) value *= 100.0 / static_cast<double>(pairs);
        cloud.valid[i] = 1;
    }
    // 公式变体固定为自身 SPFH + 平方距离倒数加权邻居 SPFH；每邻居分组归一至100。
    // 与 Open3D v0.19.0 的组合方式一致，额外剔除退化点对；邻域按需重查，不保存 N*k 边表。
    const auto spfhValid = cloud.valid;
    for (std::size_t i = 0; i < cloud.points.size(); ++i) {
        CheckWork(work);
        if (!spfhValid[i]) continue;
        index.Nearest(cloud.points[i], p.neighborLimit, p.featureRadius * p.featureRadius, neighbors, i);
        auto &descriptor = cloud.descriptors[i];
        for (const auto &neighbor : neighbors)
            if (neighbor.first > 0 && spfhValid[neighbor.second])
                for (std::size_t bin = 0; bin < 33; ++bin) descriptor[bin] += spfh[neighbor.second][bin] / neighbor.first;
        for (std::size_t group = 0; group < 3; ++group) {
            const auto first = descriptor.begin() + group * 11;
            const double sum = std::accumulate(first, first + 11, 0.0);
            for (std::size_t bin = group * 11; bin < (group + 1) * 11; ++bin)
                descriptor[bin] = spfh[i][bin] + (sum > 0 ? 100 * descriptor[bin] / sum : 0);
        }
        if (!std::all_of(descriptor.begin(), descriptor.end(), [](double value) { return std::isfinite(value); })) {
            descriptor = {}; cloud.valid[i] = 0;
        }
    }
}

AlignmentPrealignResult AlignmentPrealign::BuildCandidates(const AlignmentWork &work) {
    CheckWork(work);
    Require(work.recipe.prealignment && GetParamsValid(*work.recipe.prealignment), "Invalid automatic prealignment parameters.");
    const auto &p = *work.recipe.prealignment;
    Require(work.mesh && work.referenceMesh, "Automatic prealignment needs two surfaces.");
    Require(p.sourceTriangles.empty() || work.recipe.exactMesh == work.input.mesh, "Prealignment source selection is stale.");
    CheckBudget(work, 0, 0);
    const auto sourcePlan = PlanSamples(*work.mesh, p.sourceTriangles, work, true);
    const auto targetPlan = PlanSamples(*work.referenceMesh, p.referenceTriangles, work, false);
    CheckBudget(work, sourcePlan.count, targetPlan.count);
    auto source = SampleMesh(*work.mesh, sourcePlan, work, true), target = SampleMesh(*work.referenceMesh, targetPlan, work, false);
    BuildFeatures(source, work); BuildFeatures(target, work);
    AlignmentPrealignResult result;
    auto &stats = result.statistics;
    stats.sourceSamples = source.points.size(); stats.referenceSamples = target.points.size();
    stats.sourceRejectedTriangles = sourcePlan.rejected;
    stats.sourceSelectedArea = sourcePlan.area + sourcePlan.rejectedArea;
    stats.sourceRejectedArea = sourcePlan.rejectedArea;
    std::vector<std::size_t> sourceIds, referenceIds;
    std::vector<AlignmentDescriptor> referenceFeatures;
    sourceIds.reserve(source.valid.size()); referenceIds.reserve(target.valid.size());
    referenceFeatures.reserve(target.valid.size());
    for (std::size_t i = 0; i < source.valid.size(); ++i) if (source.valid[i]) sourceIds.push_back(i);
    for (std::size_t i = 0; i < target.valid.size(); ++i) if (target.valid[i]) {
        referenceIds.push_back(i); referenceFeatures.push_back(target.descriptors[i]);
    }
    stats.sourceFeatures = sourceIds.size(); stats.referenceFeatures = referenceIds.size();
    if (sourceIds.size() < 3 || referenceIds.size() < 3) return result;
    const auto matchesPerPoint = std::min(p.matchCount, referenceIds.size());
    std::vector<std::size_t> matches(sourceIds.size() * matchesPerPoint);
    {
        PointIndex<33> featureIndex(referenceFeatures, work);
        std::vector<Neighbor> neighbors; neighbors.reserve(matchesPerPoint);
        for (std::size_t i = 0; i < sourceIds.size(); ++i) {
            CheckWork(work);
            featureIndex.Nearest(source.descriptors[sourceIds[i]], matchesPerPoint,
                                  std::numeric_limits<double>::infinity(), neighbors);
            for (std::size_t j = 0; j < matchesPerPoint; ++j) matches[i * matchesPerPoint + j] = referenceIds[neighbors[j].second];
        }
    }
    // 描述子只用于建立候选对应；几何覆盖评分独立使用全部工作样本。
    std::vector<AlignmentDescriptor>().swap(referenceFeatures);
    std::vector<AlignmentDescriptor>().swap(source.descriptors);
    std::vector<AlignmentDescriptor>().swap(target.descriptors);
    std::vector<AlignmentPoint>().swap(source.normals);
    std::vector<AlignmentPoint>().swap(target.normals);
    std::vector<std::uint8_t>().swap(source.valid);
    std::vector<std::uint8_t>().swap(target.valid);
    std::vector<std::size_t>().swap(referenceIds);
    PointIndex<3> targetIndex(target.points, work);
    struct Candidate final { AlignmentMatrix pose; std::size_t inliers = 0; double squared = 0; };
    std::vector<Candidate> candidates;
    std::vector<Neighbor> nearest; nearest.reserve(1);
    Vec center{};
    for (const auto &point : source.points) center += Vector(point) / static_cast<double>(source.points.size());
    double radius = 0;
    for (const auto &point : source.points) radius = std::max(radius, cv::norm(Vector(point) - center));
    const double separation = p.candidateSeparation > 0 ? p.candidateSeparation : p.maxPairDistance;
    const auto better = [](const Candidate &a, const Candidate &b) { return a.inliers != b.inliers ? a.inliers > b.inliers : a.squared < b.squared; };
    std::mt19937_64 random(p.randomSeed);
    for (std::size_t trial = 0; trial < p.iterationLimit; ++trial) {
        CheckWork(work); ++stats.trials;
        std::array<std::size_t, 3> a{}, b{};
        for (std::size_t j = 0; j < 3; ++j) {
            const auto row = static_cast<std::size_t>(random() % sourceIds.size());
            a[j] = sourceIds[row]; b[j] = matches[row * matchesPerPoint + random() % matchesPerPoint];
        }
        if (a[0] == a[1] || a[0] == a[2] || a[1] == a[2] || b[0] == b[1] || b[0] == b[2] || b[1] == b[2]) continue;
        const auto pose = FitTriple(source, target, a, b, work);
        if (!pose) continue;
        Candidate candidate{*pose};
        for (std::size_t i = 0; i < source.points.size(); ++i) {
            if (i % 128 == 0) CheckWork(work);
            targetIndex.Nearest(Point(Transform(*pose, Vector(source.points[i]))), 1,
                                p.maxPairDistance * p.maxPairDistance, nearest);
            if (!nearest.empty()) { ++candidate.inliers; candidate.squared += nearest.front().first; }
        }
        const double coverage = static_cast<double>(candidate.inliers) / static_cast<double>(source.points.size());
        stats.bestCoverage = std::max(stats.bestCoverage, coverage);
        if (coverage < p.minCoverage) continue;
        const auto existing = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate &other) {
            return cv::norm(Transform(other.pose, center) - Transform(candidate.pose, center)) +
                   radius * GetRotationAngle(Multiply(candidate.pose, Inverse(other.pose))) <= separation;
        });
        if (existing != candidates.end()) { if (better(candidate, *existing)) *existing = candidate; }
        else if (candidates.size() < p.candidateLimit) candidates.push_back(candidate);
        else { stats.isCandidateLimitReached = true; if (better(candidate, candidates.back())) candidates.back() = candidate; }
        std::sort(candidates.begin(), candidates.end(), better);
    }
    for (const auto &candidate : candidates) result.poses.push_back(candidate.pose);
    stats.candidates = result.poses.size();
    return result;
}
