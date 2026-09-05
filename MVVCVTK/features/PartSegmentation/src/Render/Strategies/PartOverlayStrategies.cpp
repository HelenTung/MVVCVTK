#include "Render/Strategies/PartOverlayStrategies.h"

#include <vtkActor.h>
#include <vtkDataArray.h>
#include <vtkDataSet.h>
#include <vtkImageData.h>
#include <vtkImageProperty.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkLookupTable.h>
#include <vtkMatrix3x3.h>
#include <vtkMatrix4x4.h>
#include <vtkPlane.h>
#include <vtkPointData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyData.h>
#include <vtkProperty.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

constexpr std::uint32_t maxOverlayPartCount = 4096;

std::uint32_t GetPartCount(vtkDataSet& data)
{
    auto* scalars = data.GetPointData()
        ? data.GetPointData()->GetScalars() : nullptr;
    if (!scalars || scalars->GetNumberOfComponents() != 1) return 0;
    double range[2]{};
    scalars->GetRange(range);
    if (!std::isfinite(range[1]) || range[1] <= 0.0
        || range[1] > static_cast<double>(maxOverlayPartCount)) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::floor(range[1]));
}

std::array<double, 3> GetImageNormal(
    vtkImageData& image,
    const Orientation orientation)
{
    int column = 0;
    if (orientation == Orientation::Top_down) column = 2;
    else if (orientation == Orientation::Front_back) column = 1;
    std::array<double, 3> normal{};
    const auto* direction = image.GetDirectionMatrix();
    if (direction) {
        for (std::size_t row = 0; row < normal.size(); ++row) {
            normal[row] = direction->GetElement(
                static_cast<int>(row), column);
        }
        const double length = std::sqrt(
            normal[0] * normal[0]
            + normal[1] * normal[1]
            + normal[2] * normal[2]);
        if (std::isfinite(length)
            && length > std::numeric_limits<double>::epsilon()) {
            for (double& value : normal) value /= length;
            return normal;
        }
    }

    normal = { 0.0, 0.0, 0.0 };
    normal[static_cast<std::size_t>(column)] = 1.0;
    return normal;
}

void SetPartColor(
    vtkLookupTable& table,
    const std::uint32_t partId)
{
    const std::uint32_t hash = partId * 2654435761U;
    const double red = 0.25 + 0.75
        * static_cast<double>((hash >> 16) & 0xffU) / 255.0;
    const double green = 0.25 + 0.75
        * static_cast<double>((hash >> 8) & 0xffU) / 255.0;
    const double blue = 0.25 + 0.75
        * static_cast<double>(hash & 0xffU) / 255.0;
    table.SetTableValue(
        static_cast<vtkIdType>(partId), red, green, blue, 0.85);
}

bool SetLookupTable(
    vtkLookupTable& table,
    const std::uint32_t partCount)
{
    if (partCount >= static_cast<std::uint32_t>(
            std::numeric_limits<int>::max())) {
        return false;
    }
    table.SetNumberOfTableValues(
        static_cast<vtkIdType>(partCount) + 1);
    table.SetTableRange(0.0, static_cast<double>(
        std::max<std::uint32_t>(1U, partCount)));
    table.SetTableValue(0, 0.0, 0.0, 0.0, 0.0);
    for (std::uint32_t partId = 1;
        partId <= partCount; ++partId) {
        SetPartColor(table, partId);
    }
    table.Build();
    return true;
}

} // namespace

PartSurfaceOverlayStrategy::PartSurfaceOverlayStrategy()
    : m_actor(vtkSmartPointer<vtkActor>::New())
    , m_mapper(vtkSmartPointer<vtkPolyDataMapper>::New())
    , m_lut(vtkSmartPointer<vtkLookupTable>::New())
{
    m_mapper->SetLookupTable(m_lut);
    m_mapper->SetResolveCoincidentTopologyToPolygonOffset();
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetOpacity(0.85);
    m_actor->GetProperty()->SetLighting(false);
    m_actor->SetPickable(false);
    AttachProp(m_actor);
}

void PartSurfaceOverlayStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* surface = vtkPolyData::SafeDownCast(data);
    if (!surface) return;
    const std::uint32_t partCount = GetPartCount(*surface);
    if (!SetLookupTable(*m_lut, partCount)) return;

    m_mapper->SetInputData(surface);
    m_mapper->SetScalarRange(
        1.0, static_cast<double>(std::max<std::uint32_t>(1U, partCount)));
}

void PartSurfaceOverlayStrategy::SetOverlayState(
    const FeatureOverlayState& state)
{
    Set3DPropsTransform(state.modelToWorld);
}

PartSliceOverlayStrategy::PartSliceOverlayStrategy(
    const Orientation orientation)
    : m_slice(vtkSmartPointer<vtkImageSlice>::New())
    , m_mapper(vtkSmartPointer<vtkImageResliceMapper>::New())
    , m_lut(vtkSmartPointer<vtkLookupTable>::New())
    , m_plane(vtkSmartPointer<vtkPlane>::New())
    , m_orientation(orientation)
{
    m_slice->SetMapper(m_mapper);
    m_slice->GetProperty()->SetLookupTable(m_lut);
    m_slice->GetProperty()->SetUseLookupTableScalarRange(true);
    m_slice->GetProperty()->SetLayerNumber(1);
    m_slice->GetProperty()->SetInterpolationTypeToNearest();
    m_mapper->SliceFacesCameraOff();
    m_mapper->SliceAtFocalPointOff();
    m_mapper->SetSlicePlane(m_plane);
    AttachProp(m_slice);
}

void PartSliceOverlayStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* image = vtkImageData::SafeDownCast(data);
    if (!image) return;
    SetLookupTable(*image);
    m_mapper->SetInputData(image);

    double center[3]{};
    image->GetCenter(center);
    m_plane->SetOrigin(center);
    m_normal = GetImageNormal(*image, m_orientation);
    m_plane->SetNormal(m_normal.data());
}

void PartSliceOverlayStrategy::SetOverlayState(
    const FeatureOverlayState& state)
{
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->DeepCopy(state.modelToWorld.data());
    m_slice->SetUserMatrix(matrix);

    constexpr double sliceOffset = 0.001;
    m_plane->SetOrigin(
        state.cursor[0] + m_normal[0] * sliceOffset,
        state.cursor[1] + m_normal[1] * sliceOffset,
        state.cursor[2] + m_normal[2] * sliceOffset);
    m_plane->SetNormal(m_normal.data());
}

void PartSliceOverlayStrategy::SetLookupTable(vtkImageData& image)
{
    (void)::SetLookupTable(*m_lut, GetPartCount(image));
}
