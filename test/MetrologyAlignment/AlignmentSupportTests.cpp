// 样本支撑的解析真值；展宽/跨度不是零件公差或统计不确定度。
#include "AlignmentGeometry.h"
#include "AlignmentMath.h"
#include <iostream>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<AlignmentGeometrySupport>,"Support metadata must not own dynamic sample buffers.");

namespace {
using namespace AlignmentMath;
int failures=0;
void Check(bool value,const char *message) { if (!value) { ++failures; std::cerr<<"FAILED: "<<message<<'\n'; } }
AlignmentGeometry Fit(AlignmentGeometryKind kind,const std::vector<AlignmentPoint> &points,double scale=1,
                      AlignmentPoint direction={0,0,1}) {
    AlignmentWork work; work.cancelled=std::make_shared<std::atomic<bool>>(false);
     work.recipe.lengthScale=scale;
    work.recipe.solveTolerance=scale*1e-9;
    AlignmentGeometrySpec spec; spec.id="support"; spec.kind=kind; spec.targetDirection=direction; spec.maxFitRms=scale*1e-6;
    return AlignmentGeometryFit::BuildGeometry(work,spec,AlignmentSamples{points,0},alignmentIdentity);
}
void SpreadsAndScale() {
    for (const double scale:{.001,1.0,1000.0}) {
        std::vector<AlignmentPoint> plane;
        for (const double x:{-2.0,2.0}) for (const double y:{-3.0,3.0}) plane.push_back({x*scale,y*scale,10*scale});
        const auto fit=Fit(AlignmentGeometryKind::Plane,plane,scale);
        Check(fit.support && AlignmentGeometryFit::GetSupportValid(fit),"plane support is present and valid");
        if (fit.support) {
            const auto &s=*fit.support;
            Check(std::abs(s.principalRmsSpreads[0]/scale-3)<1e-9 && std::abs(s.principalRmsSpreads[1]/scale-2)<1e-9 &&
                s.principalRmsSpreads[2]/scale<1e-9 && !s.axialSpan && !s.angularSpanDegrees && !s.radialDirectionMeanLength,
                "plane RMS spreads match rectangular sample variance at all scales");
        }
        const auto line=Fit(AlignmentGeometryKind::Line,{{0,0,-2*scale},{0,0,0},{0,0,2*scale}},scale);
        Check(line.support && line.support->axialSpan && std::abs(*line.support->axialSpan/scale-4)<1e-9 &&
              std::abs(line.support->principalRmsSpreads[0]/scale-std::sqrt(8.0/3))<1e-9,"line projection span differs from RMS spread");
    }
}
void AngularSupport() {
    const double pi=std::acos(-1.0); std::vector<AlignmentPoint> arc,tube,full;
    for (int i=0;i<=30;++i) {
        const double a=(350+i)*pi/180;
        arc.push_back({5*std::cos(a),5*std::sin(a),7});
        for (double z:{-2.0,0.0,2.0}) tube.push_back({5*std::cos(a),5*std::sin(a),z});
    }
    for (int i=0;i<36;++i) { const double a=2*pi*i/36; full.push_back({3*std::cos(a),3*std::sin(a),0}); }
    const auto circle=Fit(AlignmentGeometryKind::Circle,arc),cylinder=Fit(AlignmentGeometryKind::Cylinder,tube);
    Check(circle.support && circle.support->angularSpanDegrees && std::abs(*circle.support->angularSpanDegrees-30)<1e-6,
          "short circle arc across angle wrap reports 30 degrees, not 330");
    Check(cylinder.support && cylinder.support->angularSpanDegrees && cylinder.support->axialSpan &&
          std::abs(*cylinder.support->angularSpanDegrees-30)<1e-6 && std::abs(*cylinder.support->axialSpan-4)<1e-8,
          "cylinder reports independent angular and axial support");
    const AlignmentMatrix rotation{0,0,1,100,0,1,0,-50,-1,0,0,20,0,0,0,1};
    for (auto &p:tube) p=Point(Transform(rotation,Vector(p)));
    std::reverse(tube.begin(),tube.end());
    const auto rotated=Fit(AlignmentGeometryKind::Cylinder,tube,1,{1,0,0});
    Check(rotated.support && std::abs(*rotated.support->angularSpanDegrees-*cylinder.support->angularSpanDegrees)<1e-6 &&
          std::abs(*rotated.support->axialSpan-*cylinder.support->axialSpan)<1e-8,"support is invariant to rigid pose and ordering");
    const auto ring=Fit(AlignmentGeometryKind::Circle,full);
    Check(ring.support && std::abs(*ring.support->angularSpanDegrees-350)<1e-9 &&
          std::abs(ring.support->principalRmsSpreads[0]-3/std::sqrt(2.0))<1e-9,
          "finite full-circle samples preserve the largest unsampled gap");
    auto invalid=ring; invalid.support->angularSpanDegrees=361;
    Check(!AlignmentGeometryFit::GetSupportValid(invalid),"out-of-range support is rejected");
    invalid=ring; invalid.support->axialSpan=1;
    Check(!AlignmentGeometryFit::GetSupportValid(invalid),"inapplicable support field is rejected");
    invalid=ring; invalid.support.reset();
    Check(AlignmentGeometryFit::GetSupportValid(invalid),"legacy results without support remain readable");
}
void SphereConcentration() {
    const auto full=Fit(AlignmentGeometryKind::Sphere,{{2,0,0},{-2,0,0},{0,2,0},{0,-2,0},{0,0,2},{0,0,-2}});
    Check(full.support && full.support->radialDirectionMeanLength && *full.support->radialDirectionMeanLength<1e-10 &&
          std::abs(full.support->principalRmsSpreads[2]-2/std::sqrt(3.0))<1e-9,"balanced sphere samples have zero radial mean");
    std::vector<AlignmentPoint> cap; const double pi=std::acos(-1.0); double expected=0;
    for (int polar:{10,20,30}) {
        const double p=polar*pi/180; expected+=std::cos(p)/3;
        for (int i=0;i<24;++i) { const auto a=2*pi*i/24; cap.push_back({2*std::sin(p)*std::cos(a),2*std::sin(p)*std::sin(a),2*std::cos(p)}); }
    }
    const auto patch=Fit(AlignmentGeometryKind::Sphere,cap);
    Check(patch.support && std::abs(*patch.support->radialDirectionMeanLength-expected)<1e-8,
          "spherical cap concentration matches analytic average cosine");
}
}
int TestGeometrySupport() {
    std::cout<<"GeometrySupportBytes="<<sizeof(AlignmentGeometrySupport)
             <<" OptionalSupportBytes="<<sizeof(std::optional<AlignmentGeometrySupport>)<<'\n';
    try { SpreadsAndScale(); AngularSupport(); SphereConcentration(); }
    catch (const std::exception &e) { ++failures; std::cerr<<"Support exception: "<<e.what()<<'\n'; }
    return failures;
}
