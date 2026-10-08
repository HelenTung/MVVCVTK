#include "AlignmentDeviation.h"
#include <cmath>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
constexpr double epsilon = 1e-9; // 解析双精度计算容差，不代表 CT 精度。
void Check(bool value, const char *message) { if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; } }
AlignmentWork Work() {
    AlignmentWork work;
    work.deadline = std::chrono::steady_clock::time_point::max();
    work.cancelled = std::make_shared<std::atomic<bool>>(false);
    work.mesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{1,1,2,2,1,2,1,2,2},std::vector<std::uint64_t>{0,1,2});
    work.referenceMesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{0,0,0,10,0,0,0,10,0},std::vector<std::uint64_t>{0,1,2});
    return work;
}
AlignmentDeviationRequest Request() {
    AlignmentDeviationRequest request;
    request.alignmentResult.entityId.bytes[0] = 1; request.alignmentResult.generation = 1;
    request.reverseCoverage = false; request.maxDistance = 5;
    return request;
}
void Distances() {
    auto work = Work(); auto request = Request();
    auto result = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(result.status == AlignmentStatus::FullyDetermined && result.forward && !result.reverse,
          "full vertex field built against triangle interiors");
    if (!result.forward) return;
    const auto &stats = result.forward->statistics;
    Check(std::abs(stats.mean-2) < epsilon && std::abs(stats.rms-2) < epsilon &&
          std::abs(stats.absoluteP95-2) < epsilon && std::abs(stats.selectedArea-.5) < epsilon,
          "analytic triangle distance and area statistics");
    Check(result.verdict == AlignmentInspectionVerdict::NotEvaluated, "no tolerance means no pass or fail");
    request.metric = AlignmentDeviationMetric::NominalNormalProjection;
    request.tolerance = std::array<double,2>{-.1,.1};
    result = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(result.verdict == AlignmentInspectionVerdict::Fail && result.forward->statistics.outOfToleranceArea == .5,
          "positive projection outside tolerance remains visible");
    auto pose = alignmentIdentity; pose[11] = -4;
    result = AlignmentDeviation::Build(work,request,pose);
    Check(std::abs(result.forward->statistics.minimum+2) < epsilon &&
          std::abs(result.forward->samples[0].distance-2) < epsilon,
          "negative normal projection retains unsigned Euclidean distance");
    pose[3] = 20; pose[11] = -2; request.maxDistance = 30;
    result = AlignmentDeviation::Build(work,request,pose);
    Check(std::abs(result.forward->samples[0].value) < epsilon && result.forward->samples[0].distance > 10,
          "normal projection is explicitly distinct from signed Euclidean distance");
    request.maxDistance = 5;
    result = AlignmentDeviation::Build(work,request,pose);
    Check(result.forward->samples[0].state == AlignmentDeviationState::TooFar &&
          result.verdict == AlignmentInspectionVerdict::Indeterminate && !result.forward->statistics.hasValues,
          "distance gate prevents tangential projection from yielding false pass");
}
void CoverageAndQuality() {
    auto work = Work(); auto request = Request();
    const std::vector<double> one{0,0,0,1,0,0,0,1,0};
    const std::vector<std::uint64_t> triangle{0,1,2};
    work.mesh = std::make_shared<const SurfaceMeshPayload>(one,triangle);
    work.referenceMesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{0,0,0,1,0,0,0,1,0,100,0,0,101,0,0,100,1,0},
        std::vector<std::uint64_t>{0,1,2,3,4,5});
    request.reverseCoverage = true; request.maxDistance = .1; request.tolerance = std::array<double,2>{0,.01};
    auto result = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(result.status == AlignmentStatus::FullyDetermined && result.forward->statistics.evaluatedArea == .5 &&
          result.reverse && result.reverse->statistics.counts[4] == 3 && result.verdict == AlignmentInspectionVerdict::Indeterminate,
          "reverse coverage reveals entirely missing nominal patch despite perfect forward deviations");
    request.referenceTriangles = {0};
    result = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(result.verdict == AlignmentInspectionVerdict::Pass && result.reverse->statistics.counts[0] == 3 &&
          result.reverse->statistics.selectedArea == .5, "independent nominal region limits coverage evaluation");
    work.mesh = std::make_shared<const SurfaceMeshPayload>(one,triangle,
        std::vector<MeshAttribute>{{"measurement.valid",1,{0,1,1}}});
    result = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(result.forward->samples[0].state == AlignmentDeviationState::InvalidSource &&
          result.reverse->statistics.counts[3] == 3 && result.verdict == AlignmentInspectionVerdict::Indeterminate,
          "invalid measured vertex and reverse invalid target remain explicit");
    work.mesh = std::make_shared<const SurfaceMeshPayload>(one,std::vector<std::uint64_t>{0,2,1});
    request.minNormalCosine = .9;
    result = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(result.forward->statistics.counts[5] == 3 && result.reverse->statistics.counts[5] == 3,
          "opposite oriented surfaces are rejected by explicit normal gate in both directions");
}
void ValidationAndPeakBudget() {
    auto work = Work(); auto request = Request();
    work.config.workingBytes = 4096 + 256;
    Check(AlignmentDeviation::Build(work,request,alignmentIdentity).status == AlignmentStatus::FullyDetermined,
          "small budget covers one field plus query peak");
    request.reverseCoverage = true;
    const auto limited = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(limited.status == AlignmentStatus::BudgetExceeded && !limited.forward && !limited.reverse,
          "reverse field charges retained forward allocation and drops incomplete result");
    work.config.workingBytes.reset(); request = Request();
    request.metric = static_cast<AlignmentDeviationMetric>(9);
    Check(!AlignmentDeviation::GetRequestValid(request), "unknown deviation metric rejected");
    request = Request(); request.maxDistance = std::numeric_limits<double>::infinity();
    Check(!AlignmentDeviation::GetRequestValid(request), "nonfinite search distance rejected");
    request = Request(); request.tolerance = std::array<double,2>{1,0};
    Check(!AlignmentDeviation::GetRequestValid(request), "reversed tolerance interval rejected");
    request.tolerance = std::array<double,2>{-1,1};
    Check(!AlignmentDeviation::GetRequestValid(request), "negative unsigned tolerance rejected");
    request = Request(); request.minNormalCosine = std::numeric_limits<double>::quiet_NaN();
    Check(!AlignmentDeviation::GetRequestValid(request), "nonfinite normal gate rejected");
    request = Request(); request.maxDistance = 2;
    Check(AlignmentDeviation::Build(work,request,alignmentIdentity).forward->statistics.counts[1] == 3,
          "distance exactly at gate remains evaluable");
    request.maxDistance = std::nextafter(2.0,0.0);
    Check(AlignmentDeviation::Build(work,request,alignmentIdentity).forward->statistics.counts[4] == 3,
          "distance above gate stays explicitly unmatched");
}
void AreaWeightAndBudget() {
    auto work = Work(); auto request = Request();
    work.mesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{0,0,1,1,0,1,0,1,1, 3,0,3,6,0,3,3,1,3},
        std::vector<std::uint64_t>{0,1,2,3,4,5});
    const auto original = work.mesh->GetVertices();
    auto result = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(result.forward && std::abs(result.forward->statistics.mean-2.5) < epsilon &&
          std::abs(result.forward->statistics.rms-std::sqrt(7.0)) < epsilon &&
          result.forward->statistics.absoluteP95 == 3,
          "area weights prevent equal vertex counts from hiding larger high-deviation patch");
    Check(work.mesh->GetVertices() == original, "evaluation does not transform underlying measurement mesh");
    work.config.workingBytes = 1;
    Check(AlignmentDeviation::Build(work,request,alignmentIdentity).status == AlignmentStatus::BudgetExceeded,
          "field allocation obeys working budget");
    work.config.workingBytes.reset(); work.config.pointLimit = 2;
    Check(AlignmentDeviation::Build(work,request,alignmentIdentity).status == AlignmentStatus::BudgetExceeded,
          "dense evaluation never substitutes fewer samples to meet limit");
    work.config.pointLimit = 0; request.sourceTriangles = {0,0};
    Check(AlignmentDeviation::Build(work,request,alignmentIdentity).status == AlignmentStatus::InvalidInput,
          "duplicate selected triangles cannot double-count area");
    request.sourceTriangles = {2};
    Check(AlignmentDeviation::Build(work,request,alignmentIdentity).status == AlignmentStatus::InvalidInput,
          "triangle selection validates pinned index bounds");
    request.sourceTriangles.clear(); work.cancelled->store(true);
    result = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(result.status == AlignmentStatus::Cancelled && !result.forward && !result.reverse,
          "cancelled evaluation returns no partial field");
    work.cancelled->store(false); work.deadline = std::chrono::steady_clock::now();
    Check(AlignmentDeviation::Build(work,request,alignmentIdentity).status == AlignmentStatus::DeadlineExceeded,
          "expired evaluation deadline is explicit");
}
} // namespace
int main() {
    try { Distances(); CoverageAndQuality(); AreaWeightAndBudget(); ValidationAndPeakBudget(); }
    catch (const std::exception &e) { ++failures; std::cerr << e.what() << '\n'; }
    return failures ? 1 : 0;
}
