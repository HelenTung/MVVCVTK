#pragma once

#include "../TestDataPort.h"
#include "Host/SurfaceDeterminationHostTypes.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace SurfaceTest {

using Point3 = std::array<double, 3>;
using ScalarField = std::function<double(const Point3&)>;
using ValidityField = std::function<bool(const Point3&)>;

struct Checks final {
    int failureCount = 0;

    void Get(const bool condition, const std::string& name)
    {
        if (condition) return;
        ++failureCount;
        std::cerr << "FAILED: " << name << '\n';
    }
};

inline Point3 GetModelPoint(
    const std::array<double, 3>& origin,
    const std::array<double, 3>& spacing,
    const std::array<double, 9>& direction,
    const int x,
    const int y,
    const int z)
{
    const Point3 scaled{
        spacing[0] * static_cast<double>(x),
        spacing[1] * static_cast<double>(y),
        spacing[2] * static_cast<double>(z)
    };
    return {
        origin[0] + direction[0] * scaled[0]
            + direction[1] * scaled[1]
            + direction[2] * scaled[2],
        origin[1] + direction[3] * scaled[0]
            + direction[4] * scaled[1]
            + direction[5] * scaled[2],
        origin[2] + direction[6] * scaled[0]
            + direction[7] * scaled[1]
            + direction[8] * scaled[2]
    };
}

inline VtkImageGridSnapshot BuildSnapshot(
    const std::array<int, 3>& dimensions,
    const std::array<double, 3>& spacing,
    const std::array<double, 3>& origin,
    const std::array<double, 9>& direction,
    const int scalarType,
    const ScalarField& getScalar,
    const ValidityField& getValidity = {},
    const std::uint64_t version = 1,
    const std::array<int, 3>& extentMinimum = { 0, 0, 0 })
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(
        extentMinimum[0], extentMinimum[0] + dimensions[0] - 1,
        extentMinimum[1], extentMinimum[1] + dimensions[1] - 1,
        extentMinimum[2], extentMinimum[2] + dimensions[2] - 1);
    image->SetSpacing(spacing.data());
    image->SetOrigin(origin.data());
    auto matrix = vtkSmartPointer<vtkMatrix3x3>::New();
    matrix->DeepCopy(direction.data());
    image->SetDirectionMatrix(matrix);
    image->AllocateScalars(scalarType, 1);
    auto* scalars = image->GetPointData()->GetScalars();

    vtkSmartPointer<vtkImageData> mask;
    vtkDataArray* maskScalars = nullptr;
    if (getValidity) {
        mask = vtkSmartPointer<vtkImageData>::New();
        mask->SetExtent(image->GetExtent());
        mask->SetSpacing(spacing.data());
        mask->SetOrigin(origin.data());
        mask->SetDirectionMatrix(matrix);
        mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        maskScalars = mask->GetPointData()->GetScalars();
    }

    vtkIdType tuple = 0;
    for (int z = extentMinimum[2];
        z < extentMinimum[2] + dimensions[2]; ++z) {
        for (int y = extentMinimum[1];
            y < extentMinimum[1] + dimensions[1]; ++y) {
            for (int x = extentMinimum[0];
                x < extentMinimum[0] + dimensions[0]; ++x, ++tuple) {
                const Point3 point = GetModelPoint(
                    origin, spacing, direction, x, y, z);
                scalars->SetComponent(tuple, 0, getScalar(point));
                if (maskScalars) {
                    maskScalars->SetComponent(
                        tuple, 0, getValidity(point) ? 1.0 : 0.0);
                }
            }
        }
    }

    VtkDataBridge bridge;
    const auto payload = bridge.CreateImagePayload(image, mask);
    if (!payload) return {};
    const auto ref = GetTestDataRef(version);
    const auto data = std::make_shared<const DataRevision>(DataRevision{
        ref, DataTypes::imageGrid3D, {}, payload, {} });
    return std::make_shared<const VtkImageGridView>(VtkImageGridView{
        {}, DataBinding{ std::string(primaryVolumeBinding), ref, 1 },
        data, std::move(image), std::move(mask) });
}

inline double GetSmoothInside(
    const double signedDistance,
    const double background = 0.0,
    const double material = 1000.0,
    const double blur = 0.6)
{
    return background + (material - background)
        * 0.5 * (1.0 - std::tanh(signedDistance / blur));
}

inline VtkImageGridSnapshot BuildPlane(
    const int scalarType = VTK_FLOAT,
    const double boundary = 15.35,
    const std::array<double, 3>& spacing = { 1.0, 1.0, 1.0 },
    const std::array<double, 9>& direction = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0 },
    const ValidityField& validity = {})
{
    const double material = scalarType == VTK_UNSIGNED_CHAR
        ? 255.0 : 1000.0;
    return BuildSnapshot(
        { 32, 24, 20 },
        spacing,
        { 0.0, 0.0, 0.0 },
        direction,
        scalarType,
        [boundary, material](const Point3& point) {
            return GetSmoothInside(
                point[0] - boundary, 0.0, material);
        },
        validity);
}

inline VtkImageGridSnapshot BuildSphere(
    const int scalarType = VTK_FLOAT,
    const Point3& center = { 15.5, 15.5, 15.5 },
    const double radius = 8.0,
    const std::array<double, 3>& spacing = { 1.0, 1.0, 1.0 },
    const std::array<double, 9>& direction = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0 })
{
    return BuildSnapshot(
        { 32, 32, 32 },
        spacing,
        { 0.0, 0.0, 0.0 },
        direction,
        scalarType,
        [center, radius](const Point3& point) {
            const double dx = point[0] - center[0];
            const double dy = point[1] - center[1];
            const double dz = point[2] - center[2];
            return GetSmoothInside(
                std::sqrt(dx * dx + dy * dy + dz * dz) - radius);
        });
}

inline SurfaceDeterminationStartParams GetParams(
    const SurfaceDeterminationMethod method =
        SurfaceDeterminationMethod::LocalAdaptiveIso50)
{
    SurfaceDeterminationStartParams params;
    params.targetViews.viewRoles.push_back(HostRenderViewRole::Primary3D);
    params.method = method;
    params.initialIsoValue = 500.0;
    params.minimumContrast = 50.0;
    return params;
}

inline bool GetPointAccepted(const SurfacePointRecord& point)
{
    constexpr SurfacePointFlags rejected =
        SurfacePointFlags::LowContrast
        | SurfacePointFlags::InvalidSupport
        | SurfacePointFlags::ProfileClipped
        | SurfacePointFlags::ExcessiveOffset
        | SurfacePointFlags::FitRejected
        | SurfacePointFlags::TriangleFlipRisk;
    return (point.flags & rejected) == SurfacePointFlags::None;
}

} // namespace SurfaceTest
