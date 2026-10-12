#include "AlignmentDeviation.h"
#include <opencv2/core.hpp>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
AlignmentComputeBackend backend=AlignmentComputeBackend::Cpu;
constexpr double epsilon = 1e-9; // 解析双精度计算容差，不代表 CT 精度。
void Check(bool value, const char *message) { if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; } }
AlignmentWork Work() {
    AlignmentWork work;

    work.cancelled = std::make_shared<std::atomic<bool>>(false);
    work.mesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{1,1,2,2,1,2,1,2,2},std::vector<std::uint64_t>{0,1,2});
    work.referenceMesh = std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{0,0,0,10,0,0,0,10,0},std::vector<std::uint64_t>{0,1,2});
    return work;
}
AlignmentDeviationRequest Request() {
    AlignmentDeviationRequest request;
    request.computeBackend=backend;
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
void CellQualityCoverage() {
    auto work = Work(); auto request = Request();
    const std::vector<double> vertices{0,0,0,1,0,0,0,1,0};
    const std::vector<std::uint64_t> triangles{0,1,2};
    const std::vector<MeshAttribute> quality{{"measurement.valid",1,{1,1,1}},
        {"measurement.support-ratio",1,{1,1,1}}, {"measurement.localization-sigma",1,{0,0,0}},
        {"measurement.fit-residual",1,{0,0,0}}, {"measurement.normal",3,{0,0,1,0,0,1,0,0,1}}};
    work.mesh = std::make_shared<const SurfaceMeshPayload>(vertices,triangles,quality,"RAS",
        std::vector<MeshAttribute>{{"measurement.valid",1,{0}}});
    work.referenceMesh = std::make_shared<const SurfaceMeshPayload>(vertices,triangles);
    request.requiresMeasurementQuality = true; request.reverseCoverage = true;
    request.minNormalCosine = .9; request.tolerance = std::array<double,2>{0,.01};
    const auto result = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(result.status == AlignmentStatus::FullyDetermined && result.forward && result.reverse,
          "invalid cell still yields an explicit unevaluated coverage field");
    if (!result.forward || !result.reverse) return;
    Check(result.forward->statistics.counts[2] == 3 && result.forward->statistics.selectedArea == .5 &&
          result.forward->statistics.evaluatedArea == 0 && result.reverse->statistics.counts[3] == 3 &&
          result.reverse->statistics.selectedArea == .5 && result.verdict == AlignmentInspectionVerdict::Indeterminate,
          "valid vertices on invalid cell cannot produce a zero-deviation Pass in either coverage direction");
    work.mesh = std::make_shared<const SurfaceMeshPayload>(std::vector<double>{0,0,0,1,0,0,0,1,0,1,1,0},
        std::vector<std::uint64_t>{0,1,2,1,3,2},std::vector<MeshAttribute>{},"RAS",
        std::vector<MeshAttribute>{{"measurement.valid",1,{1,0}}});
    request.requiresMeasurementQuality = false; request.minNormalCosine.reset(); request.reverseCoverage = false;
    const auto mixed = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(mixed.forward && std::abs(mixed.forward->statistics.selectedArea-1) < epsilon &&
          std::abs(mixed.forward->statistics.evaluatedArea-1.0/6) < epsilon &&
          mixed.verdict == AlignmentInspectionVerdict::Indeterminate,
          "shared invalid-face vertices are conservative without deleting selected area");
    request.sourceTriangles = {0};
    const auto selected = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(selected.forward && std::abs(selected.forward->statistics.evaluatedArea-.5) < epsilon &&
          selected.verdict == AlignmentInspectionVerdict::Pass,
          "unselected invalid incident faces do not contaminate a valid local evaluation");
}
void ValidationAndPeakBudget() {
    auto work = Work(); auto request = Request();
    request = Request();
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
    request=Request();request.computeBackend=static_cast<AlignmentComputeBackend>(9);
    Check(!AlignmentDeviation::GetRequestValid(request),"unknown compute backend rejected");
    request=Request();auto overflowPose=alignmentIdentity;overflowPose[3]=1e200;
    const auto overflow=AlignmentDeviation::Build(work,request,overflowPose);
    Check(overflow.status==AlignmentStatus::InvalidInput && !overflow.forward && !overflow.reverse,"distance overflow never publishes a partial CPU or CUDA field");
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
    request.sourceTriangles = {0,0};
    Check(AlignmentDeviation::Build(work,request,alignmentIdentity).status == AlignmentStatus::InvalidInput,
          "duplicate selected triangles cannot double-count area");
    request.sourceTriangles = {2};
    Check(AlignmentDeviation::Build(work,request,alignmentIdentity).status == AlignmentStatus::InvalidInput,
          "triangle selection validates pinned index bounds");
    request.sourceTriangles.clear(); work.cancelled->store(true);
    result = AlignmentDeviation::Build(work,request,alignmentIdentity);
    Check(result.status == AlignmentStatus::Cancelled && !result.forward && !result.reverse,
          "cancelled evaluation returns no partial field");
    work.cancelled->store(false);
}
void BackendEquivalence() {
    auto work=Work();auto request=Request();request.computeBackend=AlignmentComputeBackend::Cpu;
    std::vector<double> vertices;std::vector<std::uint64_t> triangles;
    constexpr std::size_t side=101;
    for(std::size_t y=0;y<side;++y)for(std::size_t x=0;x<side;++x)
        vertices.insert(vertices.end(),{double(x)*.1,double(y)*.1,.01*std::sin(double(x)*.1)*std::cos(double(y)*.1)});
    for(std::size_t y=0;y+1<side;++y)for(std::size_t x=0;x+1<side;++x) {
        const auto i=y*side+x;triangles.insert(triangles.end(),{i,i+1,i+side,i+1,i+side+1,i+side});
    }
    work.referenceMesh=std::make_shared<const SurfaceMeshPayload>(vertices,triangles);
    for(std::size_t i=2;i<vertices.size();i+=3)vertices[i]+=.2;
    work.mesh=std::make_shared<const SurfaceMeshPayload>(std::move(vertices),triangles);
    const int previousThreads=cv::getNumThreads();cv::setNumThreads(1);
    const auto serial=AlignmentDeviation::Build(work,request,alignmentIdentity);
    cv::setNumThreads(4);const auto parallel=AlignmentDeviation::Build(work,request,alignmentIdentity);
    cv::setNumThreads(previousThreads);
    Check(serial.forward && parallel.forward,"serial and parallel fields exist");
    if(!serial.forward || !parallel.forward)return;
    auto candidate=parallel;
    if(backend==AlignmentComputeBackend::Cuda) {
        request.computeBackend=backend;candidate=AlignmentDeviation::Build(work,request,alignmentIdentity);
        Check(candidate.status==AlignmentStatus::FullyDetermined && candidate.forward,"CUDA multi-batch field completes");
        if(!candidate.forward)return;
    }
    for(std::size_t i=0;i<serial.forward->samples.size();++i) {
        const auto &a=serial.forward->samples[i],&b=parallel.forward->samples[i],&g=candidate.forward->samples[i];
        Check(a.value==b.value && a.closestTriangle==b.closestTriangle && a.state==b.state,"CPU scheduling preserves exact samples");
        Check(std::abs(a.distance-g.distance)<epsilon && std::abs(a.value-g.value)<epsilon &&
              a.closestTriangle==g.closestTriangle && a.state==g.state && a.area==g.area,"CUDA preserves distance, tie choice, state and source area");
    }
    Check(serial.forward->statistics.mean==parallel.forward->statistics.mean &&
          std::abs(serial.forward->statistics.mean-candidate.forward->statistics.mean)<epsilon,"ordered reduction preserves field statistics");
}
} // namespace
int main(int argc,char **argv) {
    if(argc>1 && std::string(argv[1])=="--cuda-unavailable") {
        auto request=Request();request.computeBackend=AlignmentComputeBackend::Cuda;
        const auto result=AlignmentDeviation::Build(Work(),request,alignmentIdentity);
        Check(result.status==AlignmentStatus::Unavailable && !result.forward && !result.reverse,"CPU-only build rejects explicit CUDA without falling back");
        request.sourceTriangles={2};
        Check(AlignmentDeviation::Build(Work(),request,alignmentIdentity).status==AlignmentStatus::InvalidInput,
              "invalid selection is rejected before unsupported backend, preserving validation precedence");
        return failures ? 1 : 0;
    }
    if(argc>1 && std::string(argv[1])=="--cuda")backend=AlignmentComputeBackend::Cuda;
    try { Distances(); CoverageAndQuality(); CellQualityCoverage(); AreaWeightAndBudget(); ValidationAndPeakBudget(); BackendEquivalence(); }
    catch (const std::exception &e) { ++failures; std::cerr << e.what() << '\n'; }
    return failures ? 1 : 0;
}
