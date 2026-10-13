#include "AlignmentDeviation.h"
#include "AlignmentMath.h"
#include "AlignmentMeshQuery.h"
#include "AlignmentCudaQuery.h"
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace {
using namespace AlignmentMath;
void Require(bool value, const char *message) { if (!value) throw std::invalid_argument(message); }
void Check(const AlignmentWork &work) {
    if (AlignmentGeometryFit::GetWorkStatus(work) != AlignmentStatus::FullyDetermined)
        throw std::runtime_error("Surface evaluation cancelled.");
}
std::size_t Charge(std::size_t available, std::size_t count, std::size_t bytes) {
    if (count > available / bytes) throw std::length_error("Surface evaluation size accounting overflows.");
    return available - count * bytes;
}
Vec Vertex(const SurfaceMeshPayload &mesh, std::uint64_t id) {
    const auto &v = mesh.GetVertices(); return {v[3*id],v[3*id+1],v[3*id+2]};
}
void Prepare(const AlignmentWork &work, const std::vector<std::uint64_t> &selection,
             AlignmentDeviationField &field, std::vector<AlignmentPoint> &normals, std::size_t available,
             const RoiReadSnapshot& roi,const AlignmentMatrix& meshToRoi) {
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
        const bool valid = AlignmentGeometryFit::GetFaceValid(mesh, triangle);
        Require(std::isfinite(area) && (area > 0 || !valid), "Evaluation contains a degenerate triangle.");
        if (roi) {
            const std::array<AlignmentPoint,3> roiTriangle{Point(Transform(meshToRoi,Vertex(mesh,a))),Point(Transform(meshToRoi,Vertex(mesh,b))),Point(Transform(meshToRoi,Vertex(mesh,c)))};
            const auto selected=roi->GetTriangleSelection(roiTriangle,0,0,[&]{return AlignmentGeometryFit::GetWorkStatus(work)!=AlignmentStatus::FullyDetermined;});
            Require(selected.error==RoiError::None,"Deviation ROI triangle selection failed.");field.roiBoundaryTolerance=std::max(field.roiBoundaryTolerance,selected.boundaryTolerance);
            const auto inverse=Inverse(meshToRoi);
            for (const auto& piece:selected.triangles) {
                const AlignmentPoint center{(piece[0][0]+piece[1][0]+piece[2][0])/3,(piece[0][1]+piece[1][1]+piece[2][1])/3,(piece[0][2]+piece[1][2]+piece[2][2])/3};
                if(!roi->GetContains(center))continue;AlignmentDeviationSample sample;sample.parentTriangle=triangle;sample.sourcePosition=Point(Transform(inverse,Vector(center)));
                std::array<AlignmentPoint,3> positions{};for(int j=0;j<3;++j)positions[j]=Point(Transform(inverse,Vector(piece[j])));sample.sourceTriangle=positions;
                const auto u=Vector(positions[1])-Vector(positions[0]),v=Vector(positions[2])-Vector(positions[0]);sample.area=cv::norm(u.cross(v))*.5;
                if(!(sample.area>0))continue;if(!valid)sample.state=AlignmentDeviationState::InvalidSource;
                const auto ab=Vertex(mesh,b)-Vertex(mesh,a),ac=Vertex(mesh,c)-Vertex(mesh,a),q=Vector(*sample.sourcePosition)-Vertex(mesh,a);
                const double aa=ab.dot(ab),bb=ac.dot(ac),d=ab.dot(ac),det=aa*bb-d*d;
                if(det>0){const double vv=(bb*ab.dot(q)-d*ac.dot(q))/det,ww=(aa*ac.dot(q)-d*ab.dot(q))/det;sample.barycentric={1-vv-ww,vv,ww};}
                field.samples.push_back(std::move(sample));if(!normals.empty())normals.push_back(Point(cross));
            }continue;
        }
        for (auto id : {a,b,c}) {
            field.samples[id].area += area;
            // 公开场按原顶点存储，不能拆成不同面上的两个样本；关联无效选中面
            // 的顶点保守标为无效，保留其全部选中面积，避免缩小分母后误报完整合格。
            if (!valid) field.samples[id].state = AlignmentDeviationState::InvalidSource;
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
    Require(std::isfinite(s.selectedArea) && s.selectedArea >= 0, "Invalid selected surface area.");
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
    auto available = Charge(std::numeric_limits<std::size_t>::max(), retainedBytes, 1);
    const auto baseCount = work.mesh->GetVertices().size()/3;
    available = Charge(available, baseCount, sizeof(AlignmentDeviationSample));
    const bool needsNormals = request.minNormalCosine.has_value();
    if (needsNormals) available = Charge(available, baseCount, sizeof(AlignmentPoint));
    auto field = std::make_shared<AlignmentDeviationField>(); field->samples.resize(baseCount);field->originalVertexCount=baseCount;
    std::vector<AlignmentPoint> normals;
    if (needsNormals) normals.resize(baseCount);
    RoiReadSnapshot roi;if(request.evaluationRoi){const auto found=work.rois.find(*request.evaluationRoi);Require(found!=work.rois.end(),"Deviation ROI read view is missing.");roi=found->second;}
    Prepare(work, selection, *field, normals, available,roi,reverse ? Inverse(pose):alignmentIdentity);
    const auto count=field->samples.size();
    {
        AlignmentMeshQuery query(work.referenceMesh, work, targetSelection);
        const auto transform = reverse ? Inverse(pose) : pose;
        const auto position = [&](std::size_t i) -> std::optional<AlignmentPoint> {
            auto &sample = field->samples[i];
            if (sample.area <= 0) return {};
            Require(std::isfinite(sample.area), "Sample area overflows.");
            if (sample.state == AlignmentDeviationState::InvalidSource) return {};
            if (!reverse && ((sample.parentTriangle && !AlignmentGeometryFit::GetFaceValid(*measuredWork.mesh,*sample.parentTriangle)) || (!sample.parentTriangle && !AlignmentGeometryFit::GetPointUsable(measuredWork, i)))) {
                sample.state = AlignmentDeviationState::InvalidSource; return {};
            }
            if (query.GetIsEmpty()) { sample.state = AlignmentDeviationState::InvalidTarget; return {}; }
            const auto point = Transform(transform,sample.sourcePosition ? Vector(*sample.sourcePosition):Vertex(*work.mesh,i));
            Require(Finite(point), "Non-finite query position.");
            return Point(point);
        };
        const auto setSample = [&](std::size_t i, const AlignmentPoint &position, const AlignmentMeshHit &hit) {
            auto &sample=field->samples[i];const auto point=Vector(position);
            if (!hit.isValid) { sample.state = AlignmentDeviationState::InvalidTarget; return; }
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
            if (sample.state != AlignmentDeviationState::Valid) return;
            if (sample.distance > request.maxDistance) { sample.state = AlignmentDeviationState::TooFar; return; }
            if (needsNormals) {
                const auto normal = Vector(normals[i]); const double length = cv::norm(normal);
                if (!Finite(normal) || !std::isfinite(length) || length <= 0)
                    sample.state = AlignmentDeviationState::NormalUnavailable;
                else if (Rotate(transform, normal/length).dot(Vector(hit.normal)) < *request.minNormalCosine)
                    sample.state = AlignmentDeviationState::NormalMismatch;
            }
        };
#if !MVVCVTK_METROLOGY_HAS_CUDA
        if (request.computeBackend == AlignmentComputeBackend::Cuda)
            throw AlignmentCudaUnavailable("This repository build has no CUDA deviation backend.");
#endif
        if (request.computeBackend == AlignmentComputeBackend::Cuda && !query.GetIsEmpty()) {
#if MVVCVTK_METROLOGY_HAS_CUDA
            static_assert(sizeof(AlignmentPoint)==3*sizeof(double));
            constexpr std::size_t batchCapacity=8192;
            auto gpu=query.BuildCudaQuery(batchCapacity);
            std::vector<AlignmentPoint> positions;positions.reserve(batchCapacity);
            std::vector<std::size_t> indices;indices.reserve(batchCapacity);
            std::vector<AlignmentCudaHit> hits(batchCapacity);
            const auto flush=[&] {
                if(positions.empty())return;
                Check(work);
                gpu->Query(positions.front().data(),positions.size(),hits.data());
                Check(work);
                cv::parallel_for_(cv::Range(0,static_cast<int>(positions.size())),[&](const cv::Range &range) {
                  for(int offset=range.start;offset<range.end;++offset) {
                    if((offset-range.start)%256==0)Check(work);
                    const auto j=static_cast<std::size_t>(offset);
                    const auto &hit=hits[j];
                    if(hit.error==2)throw std::invalid_argument("CUDA triangle distance overflows or is numerically singular.");
                    if(hit.error==3)throw std::runtime_error("CUDA triangle query traversal stack overflows.");
                    Require(hit.error==0,"CUDA surface correspondence is unavailable.");
                    // GPU 只筛候选；CPU 保留原测量公式。近等距（含共享边/顶点）完整验证面归属。
                    const auto canonical=hit.requiresVerification ? query.GetClosest(positions[j])
                        : query.GetHit(positions[j],hit.triangle);
                    setSample(indices[j],positions[j],canonical);
                  }
                });
                positions.clear();indices.clear();
            };
            for(std::size_t i=0;i<count;++i) {
                if(i%256==0)Check(work);
                if(const auto point=position(i)) { positions.push_back(*point);indices.push_back(i); }
                if(positions.size()==batchCapacity)flush();
            }
            flush();
#else
            throw AlignmentCudaUnavailable("This repository build has no CUDA deviation backend.");
#endif
        } else {
            // 索引与几何只读，每个任务只写自己的样本；串行统计保持归约顺序及数值判据。
            // int 仅是 OpenCV 调度索引，超出时分段继续，不成为输入数量上限。
            for(std::size_t base=0;base<count;) {
                const auto size=std::min(count-base,static_cast<std::size_t>(std::numeric_limits<int>::max()));
                cv::parallel_for_(cv::Range(0,static_cast<int>(size)),[&](const cv::Range &range) {
                    for(int offset=range.start;offset<range.end;++offset) {
                        const auto i=base+static_cast<std::size_t>(offset);
                        if((offset-range.start)%256==0)Check(work);
                        if(const auto point=position(i))setSample(i,*point,query.GetClosest(*point));
                    }
                });
                base+=size;
            }
        }
    }
    // 查询索引释放后再申请分位数排序工作区，两个大临时工作集不重叠。
    Statistics(work, *field, reverse ? std::nullopt : request.tolerance, available);
    return field;
}
bool Complete(const AlignmentDeviationField &field) {
    if(!field.statistics.hasValues)return false;
    for (std::size_t i = 2; i < field.statistics.counts.size(); ++i)
        if (field.statistics.counts[i]) return false;
    return field.statistics.hasValues;
}
} // namespace

bool AlignmentDeviation::GetRequestValid(const AlignmentDeviationRequest &r) {
    return GetDataRevisionRefValid(r.alignmentResult) && static_cast<unsigned>(r.metric) <= 1 &&
        static_cast<unsigned>(r.computeBackend) <= 1 &&
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
        work.cancelled = input.cancelled;
        work.rois=input.rois;work.input=input.input;
        // 固定元数据、共享指针控制块及局部对象预留；不将输入网格或进程 RSS 计作新增数组。
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
    } catch (const AlignmentCudaUnavailable &e) { output.status = AlignmentStatus::Unavailable; output.message = e.what(); }
      catch (const std::length_error &e) { output.status = AlignmentStatus::ResourceExhausted; output.message = e.what(); }
      catch (const std::bad_alloc &) { output.status = AlignmentStatus::ResourceExhausted; }
      catch (const std::invalid_argument &e) { output.status = AlignmentStatus::InvalidInput; output.message = e.what(); }
      catch (const std::exception &e) {
        const auto status = AlignmentGeometryFit::GetWorkStatus(input);
        output.status = status == AlignmentStatus::FullyDetermined ? AlignmentStatus::InternalError : status;
        output.message = e.what();
    }
    if (output.status != AlignmentStatus::FullyDetermined) { output.forward.reset(); output.reverse.reset(); }
    return output;
}
