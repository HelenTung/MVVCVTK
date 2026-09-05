#include "Render/Strategies/PartOverlayStrategies.h"
#include "Render/Contracts/SlicePlaneState.h"

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

bool SetLookupTable(
    vtkLookupTable& table,
    const PartRenderStateTable& states)
{
    if (states.statesByLabel.empty()
        || states.statesByLabel.size() - 1U > maxOverlayPartCount
        || states.statesByLabel.size()
            > static_cast<std::size_t>(
                std::numeric_limits<vtkIdType>::max())) {
        return false;
    }
    table.SetNumberOfTableValues(
        static_cast<vtkIdType>(states.statesByLabel.size()));
    table.SetTableRange(
        0.0,
        static_cast<double>(std::max<std::size_t>(
            1U, states.statesByLabel.size() - 1U)));
    for (std::size_t index = 0;
        index < states.statesByLabel.size(); ++index) {
        const auto& color = states.statesByLabel[index].color;
        table.SetTableValue(
            static_cast<vtkIdType>(index),
            color[0], color[1], color[2], color[3]);
    }
    table.Build();
    return true;
}

} // namespace

std::optional<PartRenderStateTable> BuildPartRenderStateTable(
    const PartCatalog& catalog)
{
    if (!GetPartSetIdValid(catalog.partSetId)
        || catalog.resultRevision == 0
        || catalog.catalogRevision == 0
        || catalog.partsByLabel.empty()
        || catalog.partsByLabel.size() - 1U > maxOverlayPartCount) {
        return std::nullopt;
    }
    try {
        PartRenderStateTable table;
        table.statesByLabel.resize(catalog.partsByLabel.size());
        for (std::size_t index = 1;
            index < catalog.partsByLabel.size(); ++index) {
            const auto& entry = catalog.partsByLabel[index];
            if (entry.labelId != static_cast<PartLabelId>(index)
                || !GetPartObjectIdValid(entry.objectId)) {
                return std::nullopt;
            }
            auto& state = table.statesByLabel[index];
            state.color = entry.presentation.color;
            state.color[3] = entry.presentation.isVisible
                ? state.color[3] * entry.presentation.opacity : 0.0;
            state.isSelected = entry.presentation.isSelected;
            if (state.isSelected && state.color[3] > 0.0) {
                // 选择高亮只改变本次 LUT 投影，不改变目录中的稳定颜色。
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    state.color[channel] =
                        0.5 + 0.5 * state.color[channel];
                }
            }
        }
        return table;
    }
    catch (...) {
        return std::nullopt;
    }
}

bool SetPartStates(
    const std::vector<std::shared_ptr<PartOverlayControl>>& controls,
    const PartRenderStateTable& next,
    const PartRenderStateTable& previous) noexcept
{
    std::size_t appliedCount = 0;
    for (const auto& control : controls) {
        if (!control) {
            while (appliedCount > 0) {
                --appliedCount;
                if (controls[appliedCount]) {
                    (void)controls[appliedCount]->SetPartStates(previous);
                }
            }
            return false;
        }
        if (!control->SetPartStates(next)) {
            // control 可以在返回 false 前触及内部 VTK 状态；失败项也做
            // best-effort 恢复，再逆序恢复此前已经完整应用的 View。
            (void)control->SetPartStates(previous);
            while (appliedCount > 0) {
                --appliedCount;
                if (controls[appliedCount]) {
                    (void)controls[appliedCount]->SetPartStates(previous);
                }
            }
            return false;
        }
        ++appliedCount;
    }
    return true;
}

PartSurfaceOverlayStrategy::PartSurfaceOverlayStrategy()
    : m_actor(vtkSmartPointer<vtkActor>::New())
    , m_mapper(vtkSmartPointer<vtkPolyDataMapper>::New())
    , m_lut(vtkSmartPointer<vtkLookupTable>::New())
{
    m_mapper->SetLookupTable(m_lut);
    m_mapper->SetResolveCoincidentTopologyToPolygonOffset();
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetOpacity(1.0);
    m_actor->GetProperty()->SetLighting(false);
    m_actor->SetPickable(false);
    AttachProp(m_actor);
}

void PartSurfaceOverlayStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* surface = vtkPolyData::SafeDownCast(data);
    if (!surface) return;
    m_mapper->SetInputData(surface);
}

void PartSurfaceOverlayStrategy::SetOverlayState(
    const FeatureOverlayState& state)
{
    Set3DPropsTransform(state.modelToWorld);
}

bool PartSurfaceOverlayStrategy::SetPartStates(
    const PartRenderStateTable& states) noexcept
{
    try {
        if (!m_mapper->GetInput() || !SetLookupTable(*m_lut, states)) return false;
        const auto partCount = static_cast<std::uint32_t>(
            states.statesByLabel.size() - 1U);
        m_mapper->SetScalarRange(
            0.0,
            static_cast<double>(std::max<std::uint32_t>(1U, partCount)));
        return true;
    }
    catch (...) {
        return false;
    }
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
    m_mapper->SetInputData(image);

    double center[3]{};
    image->GetCenter(center);
    m_plane->SetOrigin(center);
    m_normal = SlicePlaneState::Build(m_orientation, {}).worldNormal;
    m_plane->SetNormal(m_normal.data());
}

void PartSliceOverlayStrategy::SetOverlayState(
    const FeatureOverlayState& state)
{
    Set3DPropsTransform(state.modelToWorld);

    constexpr double sliceOffset = 0.001;
    const auto plane = SlicePlaneState::Build(m_orientation, state.cursor, sliceOffset);
    m_plane->SetOrigin(plane.worldOrigin.data());
    m_plane->SetNormal(plane.worldNormal.data());
}

bool PartSliceOverlayStrategy::SetPartStates(
    const PartRenderStateTable& states) noexcept
{
    try {
        return SetLookupTable(*m_lut, states);
    }
    catch (...) {
        return false;
    }
}
