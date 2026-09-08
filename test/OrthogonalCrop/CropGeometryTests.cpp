#include "Algorithms/CropGeometry.h"

#include <cmath>
#include <iostream>
#include <limits>

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
    return failures;
}
