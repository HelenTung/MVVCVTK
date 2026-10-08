#include "AlignmentDeviation.h"
#include "AlignmentMath.h"
#include "AlignmentMeshQuery.h"
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace {
using namespace AlignmentMath;
void Require(bool value, const char *message) { if (!value) throw std::invalid_argument(message); }
void Check(const AlignmentWork &work) {
    if (AlignmentGeometryFit::GetWorkStatus(work) != AlignmentStatus::FullyDetermined)
        throw std::runtime_error("Surface evaluation cancelled or deadline reached.");
}
std::size_t Charge(std::size_t available, std::size_t count, std::size_t bytes) {
    if (count > available / bytes) throw std::length_error("Surface evaluation exceeds working budget.");
    return available - count * bytes;
}
Vec Vertex(const SurfaceMeshPayload &mesh, std::uint64_t id) {
    const auto &v = mesh.GetVertices(); return {v[3*id],v[3*id+1],v[3*id+2]};
}
void Prepare(const AlignmentWork &work, const std::vector<std::uint64_t> &selection,
             AlignmentDeviationField &field, std::vector<AlignmentPoint> &normals, std::size_t available) {
    const auto &mesh = *work.mesh;
    const auto &triangles = mesh.GetTriangles();
    const auto count = selection.empty() ? triangles.size()/3 : selection.size();
    std::vector<std::uint64_t> sorted;
    if (!selection.empty()) {
        (void)Charge(available, selection.size(), sizeof(std::uint64_t));
        sorted = selection;
        std::size_t comparisons = 0;
        std::sort(sorted.begin(), sorted.end(), [&](auto a, auto b) {
            if (++comparisons % 256 == 0) Check(work); return a < b;
        });
        Require(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "Duplicate evaluation triangle.");
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (i % 256 == 0) Check(work);
        const auto triangle = sorted.empty() ? i : sorted[i];
        Require(triangle < triangles.size()/3, "Evaluation triangle is out of bounds.");
        const auto a = triangles[3*triangle], b = triangles[3*triangle+1], c = triangles[3*triangle+2];
        const auto cross = (Vertex(mesh,b)-Vertex(mesh,a)).cross(Vertex(mesh,c)-Vertex(mesh,a));
        const double area = cv::norm(cross)/6;
        Require(std::isfinite(area) && area > 0, "Evaluation contains a degenerate triangle.");
        for (auto id : {a,b,c}) {
            field.samples[id].area += area;
            if (!normals.empty()) normals[id] = Point(Vector(normals[id]) + cross);
        }
    }
}
void Statistics(const AlignmentWork &work, AlignmentDeviationField &field,
                const std::optional<std::array<double,2>> &tolerance, std::size_t available) {
    auto &s = field.statistics;
    std::size_t validCount = 0;
    for (std::size_t i = 0; i < field.samples.size(); ++i) {
        if (i % 256 == 0) Check(work);
        const auto &sample = field.samples[i];
        const auto state = static_cast<std::size_t>(sample.state);
        ++s.counts[state]; s.areas[state] += sample.area; s.selectedArea += sample.area;
        if (sample.state != AlignmentDeviationState::Valid) continue;
        ++validCount; s.evaluatedArea += sample.area;
        if (!s.hasValues) { s.minimum = s.maximum = sample.value; s.hasValues = true; }
        s.minimum = std::min(s.minimum, sample.value); s.maximum = std::max(s.maximum, sample.value);
        if (tolerance && (sample.value < (*tolerance)[0] || sample.value > (*tolerance)[1]))
            s.outOfToleranceArea += sample.area;
    }
    Require(std::isfinite(s.selectedArea) && s.selectedArea > 0, "Invalid selected surface area.");
    if (!validCount) return;
    (void)Charge(available, validCount, sizeof(std::size_t));
    std::vector<std::size_t> order; order.reserve(validCount);
    double square = 0;
    for (std::size_t i = 0; i < field.samples.size(); ++i) {
        if (i % 256 == 0) Check(work);
        const auto &sample = field.samples[i];
        if (sample.state != AlignmentDeviationState::Valid) continue;
        order.push_back(i);
        const double weight = sample.area/s.evaluatedArea;
        s.mean += weight*sample.value; square += weight*sample.value*sample.value;
    }
    s.rms = std::sqrt(square);
    Require(std::isfinite(s.mean) && std::isfinite(s.rms), "Deviation statistics overflow.");
    std::size_t comparisons = 0;
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        if (++comparisons % 256 == 0) Check(work);
        const double x = std::abs(field.samples[a].value), y = std::abs(field.samples[b].value);
        return x == y ? a < b : x < y;
    });
    double area = 0;
    std::size_t accumulated = 0;
    for (auto i : order) {
        if (++accumulated % 256 == 0) Check(work);
        area += field.samples[i].area; s.absoluteP95 = std::abs(field.samples[i].value);
        if (area >= .95*s.evaluatedArea) break;
    }
}
std::shared_ptr<const AlignmentDeviationField> Evaluate(AlignmentWork work,
    const AlignmentDeviationRequest &request, const AlignmentMatrix &pose, bool reverse,
    std::size_t retainedBytes) {
    const auto measuredWork = work;
    if (reverse) std::swap(work.mesh, work.referenceMesh);
    const auto &selection = reverse ? request.referenceTriangles : request.sourceTriangles;
    const auto &targetSelection = reverse ? request.sourceTriangles : request.referenceTriangles;
    auto available = Charge(work.config.workingBytes.value_or(std::numeric_limits<std::size_t>::max()), retainedBytes, 1);
    const auto count = work.mesh->GetVertices().size()/3;
    available = Charge(available, count, sizeof(AlignmentDeviationSample));
    const bool needsNormals = request.minNormalCosine.has_value();
    if (needsNormals) available = Charge(available, count, sizeof(AlignmentPoint));
    auto field = std::make_shared<AlignmentDeviationField>(); field->samples.resize(count);
    std::vector<AlignmentPoint> normals;
    if (needsNormals) normals.resize(count);
    Prepare(work, selection, *field, normals, available);
    std::size_t selected = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (i % 256 == 0) Check(work);
        if (field->samples[i].area > 0) ++selected;
    }
    if (work.config.pointLimit && selected > work.config.pointLimit)
        throw std::length_error("Evaluation selection exceeds point limit.");
    work.config.workingBytes = available;
    {
        AlignmentMeshQuery query(work.referenceMesh, work, targetSelection);
        const auto transform = reverse ? Inverse(pose) : pose;
        for (std::size_t i = 0; i < count; ++i) {
            if (i % 256 == 0) Check(work);
            auto &sample = field->samples[i];
            if (sample.area <= 0) continue;
            Require(std::isfinite(sample.area), "Sample area overflows.");
            if (!reverse && !AlignmentGeometryFit::GetPointUsable(measuredWork, i)) {
                sample.state = AlignmentDeviationState::InvalidSource; continue;
            }
            const auto point = Transform(transform, Vertex(*work.mesh,i));
            const auto hit = query.GetClosest(Point(point));
            Require(hit.isValid, "Surface correspondence is unavailable.");
            sample.closestTriangle = hit.triangle; sample.distance = std::sqrt(hit.distanceSquared);
            sample.value = !reverse && request.metric == AlignmentDeviationMetric::NominalNormalProjection
                ? (point-Vector(hit.point)).dot(Vector(hit.normal)) : sample.distance;
            sample.state = AlignmentDeviationState::Valid;
            if (reverse) {
                const auto &ids = work.referenceMesh->GetTriangles();
                for (std::size_t j = 0; j < 3; ++j)
                    if (!AlignmentGeometryFit::GetPointUsable(measuredWork, static_cast<std::size_t>(ids[3*hit.triangle+j])))
                        sample.state = AlignmentDeviationState::InvalidTarget;
            }
            if (sample.state != AlignmentDeviationState::Valid) continue;
            if (sample.distance > request.maxDistance) { sample.state = AlignmentDeviationState::TooFar; continue; }
            if (needsNormals) {
                const auto normal = Vector(normals[i]); const double length = cv::norm(normal);
                if (!Finite(normal) || !std::isfinite(length) || length <= 0)
                    sample.state = AlignmentDeviationState::NormalUnavailable;
                else if (Rotate(transform, normal/length).dot(Vector(hit.normal)) < *request.minNormalCosine)
                    sample.state = AlignmentDeviationState::NormalMismatch;
            }
        }
    }
    // 查询索引释放后再申请分位数排序工作区，两个大临时工作集不重叠。
    Statistics(work, *field, reverse ? std::nullopt : request.tolerance, available);
    return field;
}
bool Complete(const AlignmentDeviationField &field) {
    for (std::size_t i = 2; i < field.statistics.counts.size(); ++i)
        if (field.statistics.counts[i]) return false;
    return field.statistics.hasValues;
}
} // namespace

bool AlignmentDeviation::GetRequestValid(const AlignmentDeviationRequest &r) {
    return GetDataRevisionRefValid(r.alignmentResult) && static_cast<unsigned>(r.metric) <= 1 &&
        std::isfinite(r.maxDistance) && r.maxDistance > 0 &&
        (!r.minNormalCosine || (std::isfinite(*r.minNormalCosine) && *r.minNormalCosine >= 0 && *r.minNormalCosine <= 1)) &&
        (!r.tolerance || (std::isfinite((*r.tolerance)[0]) && std::isfinite((*r.tolerance)[1]) &&
                         (*r.tolerance)[0] <= (*r.tolerance)[1] &&
                         (r.metric != AlignmentDeviationMetric::UnsignedDistance || (*r.tolerance)[0] >= 0))) &&
        std::isfinite(r.minSupportRatio) && r.minSupportRatio >= 0 && r.minSupportRatio <= 1 &&
        std::isfinite(r.maxLocalizationSigma) && r.maxLocalizationSigma > 0 &&
        std::isfinite(r.maxSurfaceFitResidual) && r.maxSurfaceFitResidual > 0;
}
AlignmentDeviationData AlignmentDeviation::Build(const AlignmentWork &input,
    const AlignmentDeviationRequest &request, const AlignmentMatrix &pose) {
    AlignmentDeviationData output;
    try {
        Check(input);
        Require(GetRequestValid(request) && Rigid(pose), "Invalid deviation request or rigid transform.");
        Require(input.mesh && input.referenceMesh && input.mesh->GetValid() && input.referenceMesh->GetValid() &&
            input.mesh->GetCoordinateFrame() == input.referenceMesh->GetCoordinateFrame(), "Invalid evaluation surfaces.");
        AlignmentWork work;
        work.mesh = input.mesh; work.referenceMesh = input.referenceMesh; work.config = input.config;
        work.cancelled = input.cancelled; work.deadline = input.deadline;
        // 固定元数据、共享指针控制块及局部对象预留；不将输入网格或进程 RSS 计作新增数组。
        work.config.workingBytes = Charge(input.config.workingBytes.value_or(std::numeric_limits<std::size_t>::max()), 4096, 1);
        work.recipe.requiresMeasurementQuality = request.requiresMeasurementQuality;
        work.recipe.minSupportRatio = request.minSupportRatio;
        work.recipe.maxLocalizationSigma = request.maxLocalizationSigma;
        work.recipe.maxSurfaceFitResidual = request.maxSurfaceFitResidual;
        output.forward = Evaluate(work, request, pose, false, 0);
        if (request.reverseCoverage) output.reverse = Evaluate(work, request, pose, true,
            output.forward->samples.capacity()*sizeof(AlignmentDeviationSample));
        if (request.tolerance) {
            output.verdict = output.forward->statistics.outOfToleranceArea > 0 ? AlignmentInspectionVerdict::Fail
                : Complete(*output.forward) && (!output.reverse || Complete(*output.reverse))
                    ? AlignmentInspectionVerdict::Pass : AlignmentInspectionVerdict::Indeterminate;
        }
        output.status = AlignmentStatus::FullyDetermined;
    } catch (const std::length_error &e) { output.status = AlignmentStatus::BudgetExceeded; output.message = e.what(); }
      catch (const std::bad_alloc &) { output.status = AlignmentStatus::BudgetExceeded; }
      catch (const std::invalid_argument &e) { output.status = AlignmentStatus::InvalidInput; output.message = e.what(); }
      catch (const std::exception &e) {
        const auto status = AlignmentGeometryFit::GetWorkStatus(input);
        output.status = status == AlignmentStatus::FullyDetermined ? AlignmentStatus::InternalError : status;
        output.message = e.what();
    }
    if (output.status != AlignmentStatus::FullyDetermined) { output.forward.reset(); output.reverse.reset(); }
    return output;
}
