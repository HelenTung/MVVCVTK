#include "Algorithms/CropGeometry.h"
#include "Algorithms/CropAlgorithm.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
int GetFloatIntervalFailures()
{
    int failures=0;std::size_t certified=0,boundary=0;
    const auto check=[&](bool ok,const char* message){if(!ok){++failures;std::cerr<<"Float interval: "<<message<<'\n';}};
    for(auto shape:{CropShape::Box,CropShape::Plane,CropShape::Sphere,CropShape::Cylinder})
        for(auto mode:{CropRemovalMode::KeepInside,CropRemovalMode::RemoveInside}) {
        CropOpItem op;op.operationIndex=1;op.geometryType=shape;op.removalMode=mode;
        op.radius=1.25;op.height=2;op.axisInInputModel={2,-3,1};op.planeNormalInInputModel={1,2,-1};
        const auto geometry=CropGeometry::Build(op);const auto table=CropAlgorithm::BuildPredicateTable({op},1);
        if(!geometry||!table.isSucceeded)return failures+1;
        for(int z=-6;z<=6;++z)for(int y=-7;y<=7;++y)for(int x=-9;x<=9;++x) {
            const CropVectorDouble3Array point{double(x)/4,double(y)/4,double(z)/4};
            const auto bounds=geometry->GetFloatBounds(point);
            const int square=x*x+y*y+z*z,axial=2*x-3*y+z;
            // Exact integer oracle, avoiding MSVC's 64-bit long-double precision limitation.
            bool inside=false;
            switch(shape) {
            case CropShape::Box:inside=std::abs(x)<=4&&std::abs(y)<=4&&std::abs(z)<=4;break;
            case CropShape::Plane:inside=x+2*y-z>0;break;
            case CropShape::Sphere:inside=square<=25;break;
            case CropShape::Cylinder:inside=14*square-axial*axial<=350&&axial*axial<=224;break;
            }
            const bool expected=mode==CropRemovalMode::KeepInside?inside:!inside;
            if(bounds.classification==CropPointClassification::BoundaryBand){++boundary;continue;}
            ++certified;
            if(bounds.classification==CropPointClassification::PrecisionNotMet
                ||(bounds.classification==CropPointClassification::Kept)!=expected
                ||geometry->GetKept(point)!=expected
                ||CropAlgorithm::GetPointKept(*table.predicateTable,1,{float(point[0]),float(point[1]),float(point[2])})!=expected) {
                check(false,"certified sign differs from exact rational geometry or float evaluation");return failures;
            }
        }
    }
    check(certified>20000&&boundary>0,"certification and boundary-band coverage are missing");
    CropOpItem plane;plane.geometryType=CropShape::Plane;plane.planeNormalInInputModel={1,0,0};
    plane.planeCenterInInputModel={100000000.125,0,0};const auto geometry=CropGeometry::Build(plane);
    check(geometry->GetFloatBounds({100000000.25,0,0}).classification==CropPointClassification::BoundaryBand,
        "large-origin quantization must be reported as a boundary band");
    check(geometry->GetFloatBounds({0,0,0},{-1,0,0}).classification==CropPointClassification::PrecisionNotMet,
        "negative coordinate uncertainty was accepted");
    check(geometry->GetFloatBounds({std::numeric_limits<double>::infinity(),0,0}).classification==CropPointClassification::PrecisionNotMet,
        "non-finite interval was accepted");
    plane.planeCenterInInputModel={0,0,0};
    check(CropGeometry::Build(plane)->GetFloatBounds({1e-46,0,0}).classification==CropPointClassification::BoundaryBand,
        "conversion to zero and subnormal flushing must remain in the interval");
    CropOpItem thinBox;thinBox.boxToInputModelMatrix={1,1,0,0, 1,1+1e-10,0,0, 0,0,1,0, 0,0,0,1};
    const auto thin=CropGeometry::Build(thinBox);
    check(thin&&thin->GetFloatBounds({0.25,0.25,0}).classification==CropPointClassification::BoundaryBand,
        "near-singular affine precision must be bounded without a condition-number cutoff");
    const CropMatrixDouble16Array matrix{1e10,1e10,1,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const auto error=CropGeometry::GetAffineFloatError(matrix,{1,-1,1});
    const float left=1e10f,small=1,right=-1e10f;
    const float reordered=(left+small)+right;
    check(error&&(*error)[0]>=std::abs(double(reordered)-1.0),"affine dot error omitted cancellation/reordering");
    std::cout<<"Float interval certified="<<certified<<" boundary-band="<<boundary<<'\n';
    return failures;
}
}

int GetCropGeometryFailures()
{
    int failures=0;
    const auto check=[&](bool ok,const char* message) { if (!ok) {++failures;std::cerr<<"Geometry: "<<message<<'\n';} };
    CropOpItem sphere; sphere.geometryType=CropShape::Sphere; sphere.radius=2; sphere.centerInInputModel={3,4,5};
    const auto inside=CropGeometry::Build(sphere);
    check(inside&&inside->GetInside({5,4,5})&&!inside->GetInside({5.001,4,5}),"sphere inclusive boundary");
    sphere.removalMode=CropRemovalMode::RemoveInside;
    const auto outside=CropGeometry::Build(sphere);
    check(outside&&!outside->GetKept({3,4,5})&&outside->GetKept({6,4,5}),"sphere complement");
    CropOpItem cylinder; cylinder.geometryType=CropShape::Cylinder; cylinder.radius=2;cylinder.height=4;cylinder.axisInInputModel={2,0,0};
    const auto finite=CropGeometry::Build(cylinder);
    check(finite&&finite->GetInside({2,2,0})&&!finite->GetInside({2.01,0,0})&&!finite->GetInside({0,2.01,0}),"finite oriented cylinder caps and side");
    cylinder.removalMode=CropRemovalMode::RemoveInside;
    const auto complement=CropGeometry::Build(cylinder);
    check(complement&&complement->GetKept({3,0,0})&&complement->GetKept({0,3,0})&&!complement->GetKept({0,0,0}),"cylinder outside must be OR of outside constraints");
    CropOpItem plane; plane.geometryType=CropShape::Plane;plane.planeNormalInInputModel={0,0,3};
    const auto half=CropGeometry::Build(plane);
    check(half&&!half->GetInside({0,0,0})&&half->GetInside({0,0,1}),"legacy strict plane boundary");
    CropOpItem box;box.boxToInputModelMatrix={2,1,0,10, 0,3,0,20, 0,0,-4,30, 0,0,0,1};
    const auto affine=CropGeometry::Build(box);
    check(affine&&affine->GetInside({13,23,26})&&!affine->GetInside({14,23,26}),"box shear/reflection inverse");
    cylinder.height=0;check(!CropGeometry::Build(cylinder),"zero cylinder height accepted");
    sphere.radius=std::numeric_limits<double>::quiet_NaN();check(!CropGeometry::Build(sphere),"NaN radius accepted");
    plane.planeNormalInInputModel={0,0,0};check(!CropGeometry::Build(plane),"zero normal accepted");
    box.boxToInputModelMatrix[0]=0;box.boxToInputModelMatrix[1]=0;check(!CropGeometry::Build(box),"singular box accepted");
    // 使用独立轴向分解参考，避免只让CPU与GPU共享同一个错误。
    cylinder.radius=1.7;cylinder.height=3.2;cylinder.axisInInputModel={0,0,1};cylinder.removalMode=CropRemovalMode::KeepInside;
    const auto reference=CropGeometry::Build(cylinder);
    for (int x=-20;x<=20;++x)for(int z=-20;z<=20;++z) {
        const long double px=static_cast<long double>(x)/10, pz=static_cast<long double>(z)/10;
        const bool expected=px*px<=static_cast<long double>(cylinder.radius)*cylinder.radius
            && std::abs(pz)<=static_cast<long double>(cylinder.height)/2;
        const double dx=static_cast<double>(x)/10,dz=static_cast<double>(z)/10;
        // 十进制边界的binary表示差异单独留给精度带测试。
        if (std::abs(std::abs(dx)-cylinder.radius)<1e-12||std::abs(std::abs(dz)-cylinder.height/2)<1e-12)continue;
        check(reference&&reference->GetInside({dx,0,dz})==expected,"independent cylinder reference");
    }
    return failures + GetFloatIntervalFailures();
}
