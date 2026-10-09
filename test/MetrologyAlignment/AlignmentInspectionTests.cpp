#include "AlignmentInspection.h"
#include <cmath>
#include <iostream>

namespace {
int failures=0;
constexpr double epsilon=1e-7;
void Check(bool value,const char *message) { if (!value) { ++failures; std::cerr<<"FAILED: "<<message<<'\n'; } }
DataRevisionRef Ref(unsigned char id) { DataRevisionRef r; r.entityId.bytes[0]=id; r.generation=1; return r; }
struct Fixture final {
    AlignmentWork work;
    InspectionPlan plan;
    std::vector<double> vertices,reference;
    AlignmentMatrix pose{0,-1,0,10,1,0,0,-4,0,0,1,2,0,0,0,1};
    Fixture() {
         work.cancelled=std::make_shared<std::atomic<bool>>(false);
        work.input.mesh=Ref(1); work.recipe.nominalData=Ref(2); plan.id="inspection-fixture"; plan.alignmentResult=Ref(3);
    }
    std::size_t Add(const char *id,AlignmentGeometryKind kind,const std::vector<AlignmentPoint> &points,bool nominal=false) {
        InspectionGeometrySpec spec; spec.fit.id=id; spec.fit.kind=kind; spec.fit.maxFitRms=1;
        spec.source=nominal ? InspectionGeometrySource::ReferenceSurface : InspectionGeometrySource::MeasuredSurface;
        spec.fit.region.pinnedMesh=nominal ? work.recipe.nominalData : work.input.mesh;
        auto &v=nominal ? reference : vertices;
        for (const auto &p:points) { spec.fit.region.vertexIds.push_back(v.size()/3); v.insert(v.end(),p.begin(),p.end()); }
        plan.geometries.push_back(spec); return plan.geometries.size()-1;
    }
    std::size_t Ideal(const char *id,AlignmentGeometryKind kind,AlignmentPoint center,AlignmentPoint direction={0,0,1},double radius=0) {
        InspectionGeometrySpec spec; spec.fit.id=id; spec.fit.kind=kind; spec.source=InspectionGeometrySource::NominalDefinition;
        spec.ideal=InspectionIdealGeometry{center,direction,radius}; spec.definitionSource="drawing fixture";
        plan.geometries.push_back(spec); return plan.geometries.size()-1;
    }
    void Item(const char *id,InspectionMetric metric,std::size_t geometry,double nominal,double tolerance) {
        InspectionItem item; item.id=id; item.name=id; item.measured.metric=metric; item.measured.first.geometry=geometry;
        item.nominal=nominal; item.nominalSource="analytic drawing"; item.deviationLimits=std::array<double,2>{-tolerance,tolerance};
        plan.items.push_back(item);
    }
    InspectionData Run() {
        work.mesh=std::make_shared<const SurfaceMeshPayload>(vertices,std::vector<std::uint64_t>{});
        work.referenceMesh=std::make_shared<const SurfaceMeshPayload>(reference,std::vector<std::uint64_t>{});
        return AlignmentInspection::Build(work,plan,pose);
    }
};
std::vector<AlignmentPoint> Sphere(AlignmentPoint c,double radius) {
    std::vector<AlignmentPoint> points; const double pi=std::acos(-1.0);
    for (int j=1;j<8;++j) for (int i=0;i<24;++i) {
        const double p=pi*j/8,t=2*pi*i/24;
        points.push_back({c[0]+radius*std::sin(p)*std::cos(t),c[1]+radius*std::sin(p)*std::sin(t),c[2]+radius*std::cos(p)});
    }
    return points;
}
void SizeVersusFit() {
    Fixture f;
    const auto sphere=f.Add("measured-sphere",AlignmentGeometryKind::Sphere,Sphere({1,2,3},5.1));
    const auto nominal=f.Add("nominal-mesh-sphere",AlignmentGeometryKind::Sphere,Sphere({8,-3,5},5),true);
    f.Item("diameter",InspectionMetric::Diameter,sphere,10,.1);
    f.plan.items.back().nominal.reset(); f.plan.items.back().nominalMeasure=InspectionMeasure{};
    f.plan.items.back().nominalMeasure->first.geometry=nominal;
    f.Item("fit-residual",InspectionMetric::FitRms,sphere,0,epsilon);
    f.Item("center-x",InspectionMetric::CenterX,sphere,8,epsilon);
    const auto result=f.Run();
    Check(result.status==AlignmentStatus::FullyDetermined && result.report,"independent inspection completes");
    if (!result.report) { std::cerr<<result.message<<'\n'; return; }
    const auto &r=*result.report;
    Check(r.items[0].measured && std::abs(*r.items[0].measured-10.2)<epsilon && r.items[0].nominal &&
          std::abs(*r.items[0].nominal-10)<epsilon && r.items[0].verdict==AlignmentInspectionVerdict::Fail,
          "perfectly fitted but oversized sphere fails diameter requirement against reference mesh fit");
    Check(r.items[1].measured && *r.items[1].measured<epsilon && r.items[1].verdict==AlignmentInspectionVerdict::Pass,
          "fit residual remains separate from dimensional failure");
    Check(r.items[2].verdict==AlignmentInspectionVerdict::Pass && r.verdict==AlignmentInspectionVerdict::Fail,
          "center is evaluated in pinned alignment coordinates while overall preserves dimension failure");
    Check(r.geometries[1].source==InspectionGeometrySource::ReferenceSurface && r.evaluatedUnixMilliseconds>0,
          "report records reference-fit origin and evaluation timestamp");

    f.plan.items.resize(1); f.plan.items[0].deviationLimits.reset();
    const auto unjudged=f.Run();
    Check(unjudged.report && unjudged.report->verdict==AlignmentInspectionVerdict::NotEvaluated,
          "dimensions without acceptance limits do not imply pass");
}
void RelationsAndSections() {
    Fixture f; std::vector<AlignmentPoint> cylinder,circle;
    for (int z=-1;z<=1;++z) for (int i=0;i<32;++i) {
        const double t=2*std::acos(-1.0)*i/32;
        cylinder.push_back({4+2*std::cos(t),2*std::sin(t),static_cast<double>(z)});
        if (z==0) circle.push_back({3*std::cos(t),3*std::sin(t),6});
    }
    const auto axis=f.Add("hole",AlignmentGeometryKind::Cylinder,cylinder);
    const auto section=f.Ideal("section-z4",AlignmentGeometryKind::Plane,{0,0,4});
    f.Item("axis-at-section-z",InspectionMetric::CenterZ,axis,4,epsilon);
    Check(!AlignmentInspection::GetPlanValid(f.plan,{}),"unanchored cylinder center is not a position measurement");
    f.plan.items.back().measured.first.sectionPlane=section;
    const auto side=f.Ideal("plane-x",AlignmentGeometryKind::Plane,{0,0,0},{1,0,0});
    f.Item("axis-direction",InspectionMetric::OrientedDirectionAngleDegrees,axis,90,epsilon);
    f.plan.items.back().measured.second=InspectionOperand{side,{}};
    const auto freeCircle=f.Add("free-circle",AlignmentGeometryKind::Circle,circle);
    f.Item("free-circle-diameter",InspectionMetric::Diameter,freeCircle,6,epsilon);
    const auto first=f.Add("plane-a",AlignmentGeometryKind::Plane,{{0,0,0},{1,0,0},{0,1,0},{1,1,0}});
    const auto second=f.Add("plane-b",AlignmentGeometryKind::Plane,{{0,0,3},{1,0,3},{0,1,3},{1,1,3}});
    f.Item("plane-offset",InspectionMetric::SignedParallelPlaneDistance,first,3,epsilon);
    f.plan.items.back().measured.second=InspectionOperand{second,{}};
    const auto rounded=f.Ideal("rounded-normal-plane",AlignmentGeometryKind::Plane,{0,0,5},{0,0,1-5e-9});
    f.Item("rounded-normal-offset",InspectionMetric::SignedParallelPlaneDistance,first,3,epsilon);
    f.plan.items.back().measured.second=InspectionOperand{rounded,{}};
    const auto result=f.Run();
    Check(result.report && result.report->verdict==AlignmentInspectionVerdict::Pass,
          "axis section construction, oriented angle, free circle and parallel plane offset match analytic values");
    if (!result.report) return;
    for (const auto &row:result.report->items) if (row.verdict!=AlignmentInspectionVerdict::Pass) std::cerr<<row.id<<' '<<row.message<<'\n';
    f.plan.geometries[section].ideal->direction={1,0,0};
    const auto tangent=f.Run();
    Check(tangent.report && tangent.report->items[0].verdict==AlignmentInspectionVerdict::Indeterminate &&
          !tangent.report->items[0].measured,"axis parallel to construction plane returns no fabricated center coordinate");
}
void FailureAndBudgets() {
    Fixture f;
    const auto plane=f.Add("collinear-plane",AlignmentGeometryKind::Plane,{{0,0,0},{1,0,0},{2,0,0}});
    f.Item("plane-fit",InspectionMetric::FitRms,plane,0,.01);
    auto result=f.Run();
    Check(result.report && result.report->items[0].nominal==0 && !result.report->items[0].measured &&
          result.report->verdict==AlignmentInspectionVerdict::Indeterminate &&
          result.report->geometries[0].fitted.sampleCount==3,"degenerate fit retains sample provenance and known nominal, without pass or zero measurement");
    f.work.cancelled->store(true);
    Check(f.Run().status==AlignmentStatus::Cancelled,"cancelled inspection produces no report");
    f.work.cancelled->store(false); f.plan.items[0].nominalSource.clear();
    Check(!AlignmentInspection::GetPlanValid(f.plan,{}),"scalar nominal requires an explicit origin");
    f.plan.items[0].nominalSource="drawing"; f.plan.items[0].measured.first.geometry=999;
    Check(!AlignmentInspection::GetPlanValid(f.plan,{}),"inspection operand indices are checked");
    Fixture line;
    const auto axis=line.Add("ambiguous-axis",AlignmentGeometryKind::Line,{{0,0,0},{1,0,0},{2,0,0}});
    const auto target=line.Ideal("nominal-axis",AlignmentGeometryKind::Line,{0,0,0},{0,1,0});
    line.Item("directed-angle",InspectionMetric::OrientedDirectionAngleDegrees,axis,0,epsilon);
    line.plan.items.back().measured.second=InspectionOperand{target,{}};
    const auto ambiguous=line.Run();
    Check(ambiguous.report && ambiguous.report->items[0].verdict==AlignmentInspectionVerdict::Indeterminate,
          "orthogonal direction hint cannot assign an arbitrary sign to an oriented axis measurement");
}
} // namespace
int main() {
    try { SizeVersusFit(); RelationsAndSections(); FailureAndBudgets(); }
    catch (const std::exception &e) { ++failures; std::cerr<<e.what()<<'\n'; }
    return failures ? 1 : 0;
}
