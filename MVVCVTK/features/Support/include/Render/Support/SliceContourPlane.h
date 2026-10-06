#pragma once
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <array>
#include <cmath>

// cutter 消费模型坐标；SlicePlane 消费世界坐标，法线按协变方向转换。
namespace SliceContourPlane {
inline bool SetPlane(vtkPlane& plane, const std::array<double,3>& origin,
    const std::array<double,3>& normal, const std::array<double,16>& modelToWorld)
{
    vtkNew<vtkMatrix4x4> matrix; matrix->DeepCopy(modelToWorld.data());
    const double determinant=matrix->Determinant();
    if (!std::isfinite(determinant) || determinant==0) return false;
    vtkNew<vtkMatrix4x4> inverse; vtkMatrix4x4::Invert(matrix,inverse);
    const double world[4]{origin[0],origin[1],origin[2],1};
    double model[4]{}; inverse->MultiplyPoint(world,model);
    std::array<double,3> n{};
    for(int col=0;col<3;++col) for(int row=0;row<3;++row)
        n[col]+=matrix->GetElement(row,col)*normal[row];
    plane.SetOrigin(model); plane.SetNormal(n.data());
    return true;
}
}
