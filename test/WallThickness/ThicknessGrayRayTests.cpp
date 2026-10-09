#include "ThicknessGrayRay.h"
#include <cmath>
#include <iostream>

int GetGrayRayTestFailures()
{
    int failures=0;
    const auto check=[&](bool value,const char* text) {if(!value){++failures;std::cerr<<"FAILED: "<<text<<'\n';}};
    ThicknessMaterialField::Field field;
    field.extent={0,9,0,2,0,2};field.threshold=0;
    field.node=[](const auto& index,double& value) {value=std::min(double(index[0])-2.,6.-double(index[0]));return true;};
    const auto forward=ThicknessGrayRay::Trace(field,{2,1,1},{1,0,0},7,.25,1e-10,true);
    check(forward.status==ThicknessGrayRay::Status::Found && std::abs(forward.entry)<1e-9 && std::abs(forward.exit-4)<1e-9,
        "implicit slab returns adjacent threshold boundaries");
    const auto reverse=ThicknessGrayRay::Trace(field,{6,1,1},{-1,0,0},6,.25,1e-10,true);
    check(reverse.status==ThicknessGrayRay::Status::Found && std::abs(reverse.exit-4)<1e-9,
        "negative voxel traversal preserves the reciprocal slab interval");
    ThicknessGrayRay::Sample sample;
    check(ThicknessGrayRay::GetSample(field,{2.25,1,1},sample) && std::abs(sample.value-.25)<1e-12
        && std::abs(sample.gradient[0]-1)<1e-12 && sample.gradient[1]==0 && sample.gradient[2]==0,
        "trilinear source supplies the analytic slab gradient");
    for (double scale : {1e-30,1e30}) {
        field.node=[scale](const auto& index,double& value) {value=scale*std::min(double(index[0])-2.,6.-double(index[0]));return true;};
        const auto scaled=ThicknessGrayRay::Trace(field,{2,1,1},{1,0,0},7,.25,1e-10,true);
        check(scaled.status==ThicknessGrayRay::Status::Found && std::abs(scaled.exit-4)<1e-9,
            "gray scale magnitude does not change the physical boundaries");
    }
    field.node=[](const auto& index,double& value) {const double values[]{-2,-1,0,1,-1,1,1,0,-1,-2};value=values[index[0]];return true;};
    const auto pore=ThicknessGrayRay::Trace(field,{2,1,1},{1,0,0},7,.25,1e-10,true);
    check(pore.status==ThicknessGrayRay::Status::Found && std::abs(pore.exit-1.5)<1e-9,
        "ray ends at the first internal gap rather than jumping to another layer");
    field.node=[](const auto& index,double& value) {value=std::min(double(index[0])-2.,6.-double(index[0]));return index[0]!=4;};
    const auto missing=ThicknessGrayRay::Trace(field,{2,1,1},{1,0,0},7,.25,1e-10,true);
    check(missing.status==ThicknessGrayRay::Status::MissingSupport && missing.hasEntry,
        "missing interior voxel support stays unresolved");
    // 同一个已知 16 mm 区间，以负 extent 和非零起点核验纯块跳跃的端点与 exact 支撑。
    field.extent={-10,14,5,7,-6,-4};
    std::size_t reads=0;
    field.node=[&reads](const auto& index,double& value) {
        ++reads;value=std::min(double(index[0])+6.,10.-double(index[0]));return true;
    };
    const auto plain=ThicknessGrayRay::Trace(field,{-6,6,-5},{1,0,0},20,.25,1e-10,true);
    const auto plainReads=reads;
    const auto plainPath=field.GetMaterialPath({-6,6,-5},{16,0,0},16,1e-6,1e-4);
    field.uniformRegion=[](const auto& cell) {
        ThicknessMaterialField::Field::UniformRegion region;
        if (cell[0]>=-2 && cell[0]<6) {region.extent={-2,6,5,7,-6,-4};region.sign=1;}
        return region;
    };
    reads=0;
    const auto cached=ThicknessGrayRay::Trace(field,{-6,6,-5},{1,0,0},20,.25,1e-10,true);
    check(cached.status==plain.status && cached.entry==plain.entry && cached.exit==plain.exit && reads<plainReads,
        "uniform blocks reduce node reads without changing shifted slab ray boundaries");
    const auto cachedReverse=ThicknessGrayRay::Trace(field,{10,6,-5},{-1,0,0},20,.25,1e-10,true);
    check(cachedReverse.status==ThicknessGrayRay::Status::Found && std::abs(cachedReverse.exit-16)<1e-9,
        "negative block-face traversal preserves the reciprocal shifted slab");
    const auto cachedPath=field.GetMaterialPath({-6,6,-5},{16,0,0},16,1e-6,1e-4);
    check(plainPath.status==ThicknessMaterialField::Status::Valid && cachedPath.status==plainPath.status
        && cachedPath.trim==plainPath.trim && cachedPath.cells<plainPath.cells,
        "exact block-face cuts preserve path support and trims with fewer spans");
    const auto reversePath=field.GetMaterialPath({10,6,-5},{-16,0,0},16,1e-6,1e-4);
    check(reversePath.status==ThicknessMaterialField::Status::Valid && reversePath.trim==plainPath.trim,
        "exact negative block-face cuts preserve material support");
    field.uniformRegion={};field.extent={0,12,0,12,0,12};
    field.node=[](const auto& p,double& value) {value=std::min(double(p[0]+p[1])-4.,10.-double(p[0]+p[1]));return true;};
    const double diagonalDirection=std::sqrt(.5);
    const auto diagonal=ThicknessGrayRay::Trace(field,{2,2,4},{diagonalDirection,diagonalDirection,0},8,.1,1e-10,true);
    check(diagonal.status==ThicknessGrayRay::Status::Found && std::abs(diagonal.exit-6*diagonalDirection)<1e-9,
        "oblique planar slab preserves boundaries across simultaneous cell faces");
    field.extent={0,1,0,1,0,1};
    field.node=[](const auto& p,double& value) {value=(double(p[0])-.2)*(double(p[1])-.4)*(double(p[2])-.6);return true;};
    const auto threeRoots=ThicknessGrayRay::Trace(field,{.2,.2,.2},{1,1,1},.75,.02,1e-10,true);
    check(threeRoots.status==ThicknessGrayRay::Status::Found && std::abs(threeRoots.exit-.2)<1e-9,
        "three roots in one cell terminate at the first continuous interval exit");
    field.node=[](const auto& p,double& value) {value=(double(p[0])-.25)*(double(p[1])-.5)*(double(p[2])-.5);return true;};
    const auto tangent=ThicknessGrayRay::Trace(field,{.25,.25,.25},{1,1,1},.7,.05,1e-10,true);
    check(tangent.status==ThicknessGrayRay::Status::Ambiguous,
        "stationary threshold contact cannot become an exit through floating point residuals");
    field.extent={-2,2,-2,2,-2,2};
    field.node=[](const auto& p,double& value) {
        value=p[0]>=0 && p[1]>=0 && p[2]>=0
            ? -(double(p[0])-.25)*(double(p[1])-.5)*(double(p[2])-.5) : double(p[0])+1.5;
        return true;
    };
    const auto firstExit=ThicknessGrayRay::Trace(field,{-1.5,-1.5,-1.5},{1,1,1},3,.05,1e-10,true);
    check(firstExit.status==ThicknessGrayRay::Status::Found && std::abs(firstExit.exit-1.75)<1e-9,
        "later stationary contact does not reject an earlier simple exit");
    field.extent={0,5,0,2,0,2};
    field.node=[](const auto& p,double& value) {const double nodes[]{-1,1,1,1e-18,.5,.5};value=nodes[p[0]];return true;};
    const auto cusp=ThicknessGrayRay::Trace(field,{.5,1,1},{1,0,0},2.5,.05,1e-10,true);
    check(cusp.status==ThicknessGrayRay::Status::Ambiguous,
        "positive cell-face minimum cannot create an exit at the distance limit");
    return failures;
}
