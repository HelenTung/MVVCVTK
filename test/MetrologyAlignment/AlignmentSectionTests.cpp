// 解析多边形管的截线、固定截面圆和失败路径；不将弦误差计入算法误差。
#include "AlignmentSection.h"
#include "AlignmentInspection.h"
#include "AlignmentMath.h"
#include <iostream>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace {
using namespace AlignmentMath;
int failures=0;
void Check(bool condition,const char *message) { if (!condition) { ++failures; std::cerr<<"FAILED: "<<message<<'\n'; } }
DataRevisionRef Ref() { DataRevisionRef r; r.entityId.bytes[0]=1; r.generation=1; return r; }
struct Fixture final {
    static constexpr std::size_t sides=64;
    std::vector<double> vertices;
    std::vector<std::uint64_t> triangles;
    AlignmentWork work;
    InspectionSection section;
    Fixture() {
        work.input.mesh=Ref(); work.deadline=std::chrono::steady_clock::time_point::max();
        work.cancelled=std::make_shared<std::atomic<bool>>(false);
        section.pinnedMesh=work.input.mesh; section.planePoint={0,0,.25};
        section.sampleSpacing=2*sides*5*std::sin(std::acos(-1.0)/sides)/256*(1+1e-12);
        Tube(5);
    }
    void Tube(double radius) {
        const auto start=vertices.size()/3;
        for (int z=-1;z<=1;++z) for (std::size_t i=0;i<sides;++i) {
            const auto angle=2*std::acos(-1.0)*i/sides;
            vertices.insert(vertices.end(),{radius*std::cos(angle),radius*std::sin(angle),static_cast<double>(z)});
        }
        for (std::size_t layer=0;layer<2;++layer) for (std::size_t i=0;i<sides;++i) {
            const auto a=start+layer*sides+i,b=start+layer*sides+(i+1)%sides,c=a+sides,d=b+sides;
            triangles.insert(triangles.end(),{a,b,c,b,d,c});
        }
    }
    void Publish(std::optional<std::size_t> invalidFace={}) {
        std::vector<MeshAttribute> cells;
        if (invalidFace) {
            cells.push_back({"measurement.valid",1,std::vector<double>(triangles.size()/3,1)});
            cells[0].values.at(*invalidFace)=0;
        }
        work.mesh=std::make_shared<const SurfaceMeshPayload>(vertices,triangles,std::vector<MeshAttribute>{},"RAS",std::move(cells));
    }
    AlignmentSectionData Cut(const AlignmentMatrix &pose=alignmentIdentity) { return AlignmentSection::Build(work,section,pose); }
    InspectionPlan Plan() const {
        InspectionPlan plan; plan.id="section-fixture"; plan.alignmentResult=Ref();
        InspectionGeometrySpec spec; spec.fit.id="section-circle"; spec.fit.kind=AlignmentGeometryKind::Circle;
        spec.fit.maxFitRms=1; spec.section=section; plan.geometries.push_back(spec);
        InspectionItem item; item.id="diameter"; item.measured.metric=InspectionMetric::Diameter; plan.items.push_back(item);
        return plan;
    }
};
template<class Exception,class Call> void Reject(Call call,const char *message) {
    bool rejected=false; try { call(); } catch (const Exception &) { rejected=true; }
    Check(rejected,message);
}
void GeometryAndTrace() {
    Fixture f; f.Publish();
    for (const double z:{.25,0.0}) {
        f.section.planePoint[2]=z;
        const auto cut=f.Cut();
        const auto &trace=*cut.trace;
        const double perimeter=2*Fixture::sides*5*std::sin(std::acos(-1.0)/Fixture::sides);
        Check(trace.components==1 && trace.closedLoops==1 && std::abs(trace.perimeter-perimeter)<1e-10,
            "crossing faces and shared on-plane edges preserve analytic polygon perimeter");
        Check(cut.samples.points.size()==256 && std::abs(trace.sampleSpacing-perimeter/256)<1e-12,
            "uniform closed arclength sampling has no duplicated seam");
        for (std::size_t i=0;i<trace.samples.size();++i) {
            const auto &s=trace.samples[i]; Vec p{};
            for (std::size_t j=0;j<3;++j) {
                const auto id=f.triangles[3*s.triangle+j];
                p+=s.barycentric[j]*Vec(f.vertices[3*id],f.vertices[3*id+1],f.vertices[3*id+2]);
            }
            p-=s.planeCorrection*Vector(trace.sourcePlaneNormal);
            Check(cv::norm(p-Vector(cut.samples.points[i]))<1e-12 && std::abs(p[2]-z)<1e-12,
                "sample provenance reconstructs exact projected source position");
        }
    }
    // 绕 Y 轴旋转 90 度并平移；截面法向在对齐坐标中为 X。
    const AlignmentMatrix pose{0,0,1,10,0,1,0,-4,-1,0,0,2,0,0,0,1};
    f.section.planePoint={10.25,-4,2}; f.section.planeNormal={1,0,0};
    auto plan=f.Plan(); const auto result=AlignmentInspection::Build(f.work,plan,pose);
    Check(result.report && result.report->geometries[0].status==AlignmentStatus::FullyDetermined,"section circle inspection succeeds");
    if (result.report) {
        const auto &g=result.report->geometries[0];
        if (g.status!=AlignmentStatus::FullyDetermined) std::cerr<<g.message<<'\n';
        const double chordBound=5*(1-std::cos(std::acos(-1.0)/Fixture::sides));
        Check(g.section && std::abs(g.fitted.radius-5)<=chordBound+1e-10,
            "circle radius is within analytic tessellation chord bound");
        Check(cv::norm(Vector(g.alignedCenter)-Vec(10.25,-4,2))<1e-7 && cv::norm(Vector(g.alignedDirection)-Vec(1,0,0))<1e-12,
            "circle center and normal stay on the declared aligned plane");
    }
    plan.geometries[0].fit.region.vertexIds={0};
    Check(!AlignmentInspection::GetPlanValid(plan,{}),"section and vertex ROI are mutually exclusive");
    plan=f.Plan(); plan.geometries[0].section->pinnedMesh.generation=2;
    Check(AlignmentInspection::Build(f.work,plan,pose).status==AlignmentStatus::InvalidInput,"wrong mesh revision rejected");
}
void TopologyAndSelection() {
    Fixture f; f.Tube(4.999); f.Publish();
    Reject<std::invalid_argument>([&]{f.Cut();},"multiple loops require explicit selection");
    f.section.selectionPoint=AlignmentPoint{5,0,.25}; f.section.maxSelectionDistance=.0001;
    const auto cut=f.Cut();
    Check(cut.trace->components==2 && cut.trace->closedLoops==2,"thin adjacent shells remain distinct without spatial welding");
    Fixture coincident; coincident.Tube(5); coincident.Publish(); coincident.section.selectionPoint=AlignmentPoint{5,0,.25};
    Reject<std::invalid_argument>([&]{coincident.Cut();},"coincident independent loops cannot be selected ambiguously");
    Fixture open; open.triangles.resize(open.triangles.size()-3); open.Publish();
    Reject<std::invalid_argument>([&]{open.Cut();},"open contour rejected");
    Fixture duplicate; duplicate.triangles.insert(duplicate.triangles.end(),{64,65,128}); duplicate.Publish();
    Reject<std::invalid_argument>([&]{duplicate.Cut();},"duplicate intersected face rejected");
    Fixture invalid; invalid.Publish(128);
    Reject<std::invalid_argument>([&]{invalid.Cut();},"invalid crossed face cannot be omitted to fabricate valid contour");
    auto report=AlignmentInspection::Build(invalid.work,invalid.Plan(),alignmentIdentity);
    Check(report.report && report.report->items[0].verdict==AlignmentInspectionVerdict::Indeterminate && !report.report->items[0].measured,
        "invalid section reports no fabricated dimension");
    Fixture boundary; boundary.Publish(); boundary.section.planePoint[2]=1;
    Reject<std::invalid_argument>([&]{boundary.Cut();},"unsupported mesh end boundary rejected");
    Fixture coplanar; coplanar.triangles.insert(coplanar.triangles.end(),{64,65,66}); coplanar.Publish(); coplanar.section.planePoint[2]=0;
    Reject<std::invalid_argument>([&]{coplanar.Cut();},"coplanar face has no unique contour");
}
void Limits() {
    Fixture f; f.Publish(); f.work.config.workingBytes=4096;
    Reject<std::length_error>([&]{f.Cut();},"graph allocations obey working budget");
    f.work.config.workingBytes.reset(); f.work.config.pointLimit=8;
    Reject<std::length_error>([&]{f.Cut();},"arclength sample count obeys limit");
    f.work.config.pointLimit=0; f.work.cancelled->store(true);
    Reject<std::runtime_error>([&]{f.Cut();},"section extraction checks cancellation");
    f.work.cancelled->store(false); f.work.deadline=std::chrono::steady_clock::time_point::min();
    Reject<std::runtime_error>([&]{f.Cut();},"section extraction checks deadline");
    f.work.deadline=std::chrono::steady_clock::time_point::max(); f.work.recipe.requiresMeasurementQuality=true;
    Reject<std::invalid_argument>([&]{f.Cut();},"measurement-quality section requires face quality schema");
}
}
int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
    _set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
    for (const auto mode:{_CRT_WARN,_CRT_ERROR,_CRT_ASSERT}) {
        _CrtSetReportMode(mode,_CRTDBG_MODE_FILE);
        _CrtSetReportFile(mode,_CRTDBG_FILE_STDERR);
    }
#endif
    try {
        std::cerr<<"CASE GeometryAndTrace\n"; GeometryAndTrace();
        std::cerr<<"CASE TopologyAndSelection\n"; TopologyAndSelection();
        std::cerr<<"CASE Limits\n"; Limits();
    }
    catch (const std::exception &e) { ++failures; std::cerr<<"Unexpected: "<<e.what()<<'\n'; }
    return failures ? 1 : 0;
}
