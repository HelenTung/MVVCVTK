#include "Algorithms/CropGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
template<std::size_t count>
bool GetFinite(const std::array<double, count>& values)
{
    return std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); });
}

double GetDot(const CropVectorDouble3Array& a, const CropVectorDouble3Array& b)
{
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}

CropVectorDouble3Array GetOffset(const CropVectorDouble3Array& p, const CropVectorDouble3Array& c)
{
    return {p[0]-c[0], p[1]-c[1], p[2]-c[2]};
}

bool SetUnitVector(CropVectorDouble3Array& value)
{
    const auto length=std::hypot(value[0],value[1],value[2]);
    if (!std::isfinite(length) || length<=0) return false;
    for (auto& component:value) component/=length;
    return true;
}

bool BuildInverse(const CropMatrixDouble16Array& input, CropMatrixDouble16Array& inverse)
{
    if (!GetFinite(input) || input[12]!=0 || input[13]!=0 || input[14]!=0 || input[15]!=1) return false;
    double rows[4][8]{};
    for (int row=0; row<4; ++row) {
        for (int column=0; column<4; ++column) rows[row][column]=input[row*4+column];
        rows[row][row+4]=1;
    }
    for (int column=0;column<4;++column) {
        int pivot=column;
        for (int row=column+1;row<4;++row) {
            if (std::abs(rows[row][column])>std::abs(rows[pivot][column])) pivot=row;
        }
        if (rows[pivot][column]==0 || !std::isfinite(rows[pivot][column])) return false;
        for (int index=0;index<8;++index) std::swap(rows[pivot][index],rows[column][index]);
        const auto divisor=rows[column][column];
        for (auto& value:rows[column]) value/=divisor;
        for (int row=0;row<4;++row) {
            if (row==column) continue;
            const auto factor=rows[row][column];
            for (int index=0;index<8;++index) rows[row][index]-=factor*rows[column][index];
        }
    }
    for (int row=0;row<4;++row) for (int column=0;column<4;++column) inverse[row*4+column]=rows[row][column+4];
    return GetFinite(inverse);
}
}

std::optional<CropGeometry> CropGeometry::Build(CropOpItem operation)
{
    if (operation.recipeVersion!=1 || operation.boundaryPolicyVersion!=1
        || (operation.removalMode!=CropRemovalMode::KeepInside && operation.removalMode!=CropRemovalMode::RemoveInside)) return {};
    CropGeometry result;
    switch (operation.geometryType) {
    case CropShape::Box:
        if (!BuildInverse(operation.boxToInputModelMatrix,result.m_boxInverse)) return {};
        for (int row=0;row<3;++row) {
            result.m_boxRowNorm[row]=std::hypot(result.m_boxInverse[row*4],result.m_boxInverse[row*4+1],result.m_boxInverse[row*4+2]);
            if (!std::isfinite(result.m_boxRowNorm[row]) || result.m_boxRowNorm[row]<=0) return {};
        }
        break;
    case CropShape::Plane:
        if (!GetFinite(operation.planeCenterInInputModel) || !GetFinite(operation.planeNormalInInputModel)
            || !SetUnitVector(operation.planeNormalInInputModel)) return {};
        break;
    case CropShape::Cylinder:
        if (!GetFinite(operation.centerInInputModel) || !GetFinite(operation.axisInInputModel)
            || !SetUnitVector(operation.axisInInputModel) || !std::isfinite(operation.radius) || operation.radius<=0
            || !std::isfinite(operation.height) || operation.height<=0) return {};
        break;
    case CropShape::Sphere:
        if (!GetFinite(operation.centerInInputModel) || !std::isfinite(operation.radius) || operation.radius<=0) return {};
        break;
    default: return {};
    }
    result.m_operation=std::move(operation);
    return result;
}

double CropGeometry::GetSignedDistance(const CropVectorDouble3Array& point) const noexcept
{
    if (!GetFinite(point)) return std::numeric_limits<double>::quiet_NaN();
    switch (m_operation.geometryType) {
    case CropShape::Box: {
        double distance=-std::numeric_limits<double>::infinity();
        for (int row=0;row<3;++row) {
            const auto q=m_boxInverse[row*4]*point[0]+m_boxInverse[row*4+1]*point[1]
                +m_boxInverse[row*4+2]*point[2]+m_boxInverse[row*4+3];
            distance=std::max(distance,(std::abs(q)-(1.0+1.0e-6))/m_boxRowNorm[row]);
        }
        return distance;
    }
    case CropShape::Plane:
        return -GetDot(GetOffset(point,m_operation.planeCenterInInputModel),m_operation.planeNormalInInputModel);
    case CropShape::Sphere: {
        const auto offset=GetOffset(point,m_operation.centerInInputModel);
        return std::hypot(offset[0],offset[1],offset[2])-m_operation.radius;
    }
    case CropShape::Cylinder: {
        const auto offset=GetOffset(point,m_operation.centerInInputModel);
        const auto axial=GetDot(offset,m_operation.axisInInputModel);
        const auto& axis=m_operation.axisInInputModel;
        const auto radial=std::hypot(offset[0]-axial*axis[0],offset[1]-axial*axis[1],offset[2]-axial*axis[2])-m_operation.radius;
        const auto cap=std::abs(axial)-m_operation.height*0.5;
        return std::hypot(std::max(radial,0.0),std::max(cap,0.0))+std::min(std::max(radial,cap),0.0);
    }
    default: return std::numeric_limits<double>::quiet_NaN();
    }
}

bool CropGeometry::GetInside(const CropVectorDouble3Array& point) const noexcept
{
    const auto distance=GetSignedDistance(point);
    return m_operation.geometryType==CropShape::Plane ? distance<0 : distance<=0;
}

bool CropGeometry::GetKept(const CropVectorDouble3Array& point) const noexcept
{
    if (!GetFinite(point)) return false;
    const auto distance=GetSignedDistance(point);
    if (!std::isfinite(distance)) return false;
    const bool inside=m_operation.geometryType==CropShape::Plane ? distance<0 : distance<=0;
    return m_operation.removalMode==CropRemovalMode::KeepInside ? inside : !inside;
}

bool CropGeometry::GetOperationsSame(const CropOpItem& a,const CropOpItem& b) noexcept
{
    if (a.geometryType!=b.geometryType || a.removalMode!=b.removalMode
        || a.recipeVersion!=b.recipeVersion || a.boundaryPolicyVersion!=b.boundaryPolicyVersion) return false;
    switch (a.geometryType) {
    case CropShape::Box: return a.boxToInputModelMatrix==b.boxToInputModelMatrix;
    case CropShape::Plane: return a.planeCenterInInputModel==b.planeCenterInInputModel && a.planeNormalInInputModel==b.planeNormalInInputModel;
    case CropShape::Cylinder: return a.centerInInputModel==b.centerInInputModel && a.axisInInputModel==b.axisInInputModel && a.radius==b.radius && a.height==b.height;
    case CropShape::Sphere: return a.centerInInputModel==b.centerInInputModel && a.radius==b.radius;
    default: return false;
    }
}
