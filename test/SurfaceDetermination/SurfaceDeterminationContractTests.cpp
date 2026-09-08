#include "SurfaceDeterminationTestSupport.h"
#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceContracts.h"

#include <limits>
#include <locale>

namespace {
using namespace SurfaceTest;
struct CommaPunctuation final : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
};

void TestRecipeReplay(Checks& checks)
{
    const auto source = BuildSphere();
    auto params = GetParams(); params.initialIsoValue.reset();
    params.resultScope = "part/\"quoted\""; params.modelUnit = "mm";
    const auto result = SurfaceDeterminationAlgorithm::BuildSurface(source,params,128U*1024U*1024U,[]{return false;},{});
    checks.Get(result.status == SurfaceResultStatus::Succeeded && result.resolvedParams.initialIsoValue
        && result.resolvedParams.profileHalfLengthModel && result.resolvedParams.maximumOffsetModel
        && result.resolvedParams.profileSampleStepModel && result.resolvedParams.profileSmoothingSigmaModel,
        "auto settings preserve every resolved physical parameter");
    const auto text = SurfaceContract::BuildParameters(params,result.resolvedParams,"RAS",128U*1024U*1024U);
    const auto oldLocale=std::locale();
    std::locale::global(std::locale(oldLocale,new CommaPunctuation));
    const auto localized=SurfaceContract::BuildParameters(params,result.resolvedParams,"RAS",128U*1024U*1024U);
    std::locale::global(oldLocale);
    checks.Get(localized==text,"canonical recipe is locale independent");
    SurfaceDeterminationStartParams requested,resolved;
    std::string frame;std::size_t bytes=0;
    checks.Get(SurfaceContract::GetParameters(text,requested,resolved,frame,bytes)
        && !requested.initialIsoValue && resolved.initialIsoValue==result.resolvedParams.initialIsoValue
        && requested.resultScope==params.resultScope && frame=="RAS" && bytes==128U*1024U*1024U,
        "recipe round trip retains auto intent and actual values");
    const auto replay=SurfaceDeterminationAlgorithm::BuildSurface(source,resolved,bytes,[]{return false;},{});
    bool same=replay.status==SurfaceResultStatus::Succeeded && replay.points.size()==result.points.size();
    if(same)for(std::size_t i=0;i<replay.points.size();++i)
        same=same && replay.points[i].positionModel==result.points[i].positionModel && replay.points[i].flags==result.points[i].flags;
    checks.Get(same && replay.triangleIndices==result.triangleIndices,"resolved recipe reproduces positions and quality");
    auto changed=params; changed.minimumContrast+=1;
    checks.Get(SurfaceContract::BuildParameters(changed,result.resolvedParams,"RAS",bytes)!=text,
        "user parameter changes remain traceable");
    checks.Get(!SurfaceContract::GetParameters(text+"extra",requested,resolved,frame,bytes),"trailing recipe content is rejected");
    changed=params;changed.minimumContrast=-1;
    checks.Get(!SurfaceContract::GetParameters(SurfaceContract::BuildParameters(changed,result.resolvedParams,"RAS",bytes),
        requested,resolved,frame,bytes),"negative recipe quality is rejected without changing output");
}

void TestBaseQuality(Checks& checks)
{
    SurfacePointRecord point;
    point.normalModel={1,0,0}; point.validSupportRatio=1;
    checks.Get(SurfaceContract::GetPointValid(point,SurfaceDeterminationMethod::LocalAdaptiveIso50),"finite located point is valid");
    for(unsigned bit=0;bit<7;++bit){
        auto rejected=point;rejected.flags=static_cast<SurfacePointFlags>(1U<<bit);
        checks.Get(!SurfaceContract::GetPointValid(rejected,SurfaceDeterminationMethod::LocalAdaptiveIso50),
            "each rejected point flag blocks base measurement validity");
    }
    auto invalid=point;invalid.normalModel={0,0,0};
    checks.Get(!SurfaceContract::GetPointValid(invalid,SurfaceDeterminationMethod::GradientPeak),"zero normal rejected");
    invalid=point;invalid.positionModel[0]=std::numeric_limits<double>::infinity();
    checks.Get(!SurfaceContract::GetPointValid(invalid,SurfaceDeterminationMethod::GradientPeak),"infinite position rejected");
    invalid=point;invalid.estimatedLocalizationSigma=std::numeric_limits<float>::quiet_NaN();
    checks.Get(!SurfaceContract::GetPointValid(invalid,SurfaceDeterminationMethod::GradientPeak),"NaN evidence rejected");
    checks.Get(!SurfaceContract::GetPointValid(point,SurfaceDeterminationMethod::GlobalIsoPreview),"global preview is never measurement valid");
}

void TestAreaCoverage(Checks& checks)
{
    const auto source=BuildSnapshot({32,32,32},{1,1,1},{0,0,0},{1,0,0,0,1,0,0,0,1},VTK_FLOAT,
        [](const Point3& p){return GetSmoothInside(std::sqrt((p[0]-15.5)*(p[0]-15.5)+(p[1]-15.5)*(p[1]-15.5)+(p[2]-15.5)*(p[2]-15.5))-8);},
        [](const Point3& p){return p[1]>11;});
    const auto result=SurfaceDeterminationAlgorithm::BuildSurface(source,GetParams(),128U*1024U*1024U,[]{return false;},{});
    checks.Get(result.status==SurfaceResultStatus::Succeeded && result.triangleValidity.size()==result.triangleIndices.size()/3,
        "face quality preserves exact triangle indexing");
    if(result.objects.empty())return;
    double total=0,valid=0;
    for(std::size_t i=0;i<result.triangleValidity.size();++i){
        const auto& a=result.points[result.triangleIndices[i*3]].positionModel;
        const auto& b=result.points[result.triangleIndices[i*3+1]].positionModel;
        const auto& c=result.points[result.triangleIndices[i*3+2]].positionModel;
        const double x=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
        const double y=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
        const double z=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        const double area=0.5*std::sqrt(x*x+y*y+z*z);total+=area;
        if(result.triangleValidity[i])valid+=area;
    }
    const auto& object=result.objects.front();
    checks.Get(total>0 && valid>0 && valid<total && std::abs(object.validAreaRatio-valid/total)<1e-12,
        "coverage is independently verified by triangle area rather than vertex count");
    checks.Get(!object.volumeModelUnit3 && object.volumeValidity!=SurfaceMetricValidity::Valid,
        "closed geometry with missing measurement support cannot report complete volume");
    checks.Get(!object.areaModelUnit2 || std::abs(*object.areaModelUnit2-valid)<1e-10,
        "reported observed area excludes rejected faces");
    const auto count=std::count_if(result.points.begin(),result.points.end(),[&](const auto& p){return SurfaceContract::GetPointValid(p,result.method);});
    checks.Get(static_cast<std::uint64_t>(count)==result.acceptedPointCount,"summary uses same point rule as mesh publication");
}
}

int GetSurfaceContractFailCount()
{
    Checks checks;TestRecipeReplay(checks);TestBaseQuality(checks);TestAreaCoverage(checks);return checks.failureCount;
}
