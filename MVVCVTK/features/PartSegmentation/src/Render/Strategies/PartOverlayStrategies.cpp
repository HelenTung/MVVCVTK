#include "Render/Strategies/PartOverlayStrategies.h"
#include "Render/Contracts/SlicePlaneState.h"

#include <vtkActor.h>
#include <vtkCellPicker.h>
#include <vtkCellData.h>
#include <vtkProp3D.h>
#include <vtkRenderer.h>
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
#include <vtkPolyDataNormals.h>
#include <vtkProperty.h>
#include <vtkThreshold.h>
#include <vtkGeometryFilter.h>
#include <vtkPolyDataSilhouette.h>
#include <vtkImageReslice.h>
#include <vtkImageThreshold.h>
#include <vtkFlyingEdges2D.h>
#include <vtkNew.h>
#include <vtkImageMathematics.h>
#include <vtkInformation.h>
#include <vtkStreamingDemandDrivenPipeline.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

constexpr std::uint32_t maxOverlayPartCount = 4096;

std::optional<PartLabelId> GetPickedLabel(vtkProp3D& prop, vtkDataSet* data,
    vtkLookupTable& table, const int x, const int y, vtkRenderer* renderer,
    const bool useHiddenGeometry = false)
{
    if (!renderer || !data || (!prop.GetVisibility() && !useHiddenGeometry)) return std::nullopt;
    // picker 只观察本 Feature 的临时 prop 壳，不遍历 renderer，也不改原 prop 的可拾取状态。
    vtkSmartPointer<vtkProp3D> candidate;
    candidate.TakeReference(prop.NewInstance());
    candidate->ShallowCopy(&prop);
    candidate->VisibilityOn();
    candidate->PickableOn();
    auto picker = vtkSmartPointer<vtkCellPicker>::New();
    picker->PickFromListOn();
    picker->AddPickList(candidate);
    if (!picker->Pick(x, y, 0.0, renderer)) return std::nullopt;
    auto elementId = picker->GetPointId();
    auto* points = data->GetPointData();
    auto* scalars = points ? points->GetScalars() : nullptr;
    // 离散等值面把零件标签放在 cell 上；二维标签图使用 point scalars。
    if (!scalars && data->GetCellData()) {
        scalars = data->GetCellData()->GetScalars();
        elementId = picker->GetCellId();
    }
    if (!scalars || elementId < 0 || elementId >= scalars->GetNumberOfTuples())
        return std::nullopt;
    const auto value = scalars->GetComponent(elementId, 0);
    if (!std::isfinite(value) || value < 1.0 || value != std::floor(value)
        || value >= static_cast<double>(table.GetNumberOfTableValues())) return std::nullopt;
    const auto label = static_cast<PartLabelId>(value);
    return table.GetTableValue(label)[3] > 0.0 ? std::optional<PartLabelId>(label) : std::nullopt;
}

bool SetLookupTable(
    vtkLookupTable& table,
    const PartRenderStateTable& states,
    PartRenderStateTable& previous,
    const bool isSlice = false,
    const bool isSelectionOnly = false)
{
    if (states.statesByLabel.empty()
        || states.statesByLabel.size() - 1U > maxOverlayPartCount
        || states.statesByLabel.size()
            > static_cast<std::size_t>(
                std::numeric_limits<vtkIdType>::max())) {
        return false;
    }
    for (const auto& state : states.statesByLabel) {
        if (!std::all_of(state.color.begin(), state.color.end(), [](double value) {
            return std::isfinite(value) && value >= 0.0 && value <= 1.0;
        })) return false;
    }
    if (states == previous) return true;
    // 先完成可能失败的分配，再触碰 VTK；previous 始终描述最后成功应用的表。
    auto next = states;
    const bool hasSizeChange = previous.statesByLabel.size() != states.statesByLabel.size();
    // VTK 写入若抛异常，清空成功缓存，使完整 previous 重放能够修复半更新。
    auto applied = std::move(previous);
    previous.statesByLabel.clear();
    if (hasSizeChange) {
        table.SetNumberOfTableValues(static_cast<vtkIdType>(states.statesByLabel.size()));
        table.SetTableRange(0.0, static_cast<double>(std::max<std::size_t>(
            1U, states.statesByLabel.size() - 1U)));
    }
    bool hasColorChange = hasSizeChange;
    for (std::size_t index = 0; index < states.statesByLabel.size(); ++index) {
        const auto& color = states.statesByLabel[index].color;
        if (!hasSizeChange && color == applied.statesByLabel[index].color
            && (!(isSlice || isSelectionOnly) || states.statesByLabel[index].isSelected == applied.statesByLabel[index].isSelected)) continue;
        table.SetTableValue(static_cast<vtkIdType>(index), color[0], color[1], color[2], color[3] * (isSelectionOnly
            ? (states.statesByLabel[index].isSelected ? 0.35 : 0.0)
            : isSlice ? (states.statesByLabel[index].isSelected ? 0.8 : 0.18) : 1.0));
        hasColorChange = true;
    }
    if (hasColorChange) table.Build();
    previous = std::move(next);
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

PartSurfaceOverlayStrategy::PartSurfaceOverlayStrategy(const bool isSelectionOnly, const bool isPreview)
    : m_actor(vtkSmartPointer<vtkActor>::New())
    , m_mapper(vtkSmartPointer<vtkPolyDataMapper>::New())
    , m_normals(vtkSmartPointer<vtkPolyDataNormals>::New())
    , m_lut(vtkSmartPointer<vtkLookupTable>::New())
    , m_pickLut(vtkSmartPointer<vtkLookupTable>::New())
    , m_isSelectionOnly(isSelectionOnly)
    , m_isPreview(isPreview)
{
    m_mapper->SetLookupTable(m_lut);
    // 只生成显示法线；不拆点、不改三角形方向，正式产品保持只读共享。
    m_normals->SplittingOff();
    m_normals->ConsistencyOff();
    m_normals->AutoOrientNormalsOff();
    m_normals->ComputeCellNormalsOff();
    m_normals->ComputePointNormalsOn();
    m_mapper->SetInputConnection(m_normals->GetOutputPort());
    m_mapper->SetResolveCoincidentTopologyToPolygonOffset();
    // 候选与正式表面可能共面；仅给显示投影偏移，不改顶点或正式标签。
    if (isPreview) m_mapper->SetRelativeCoincidentTopologyPolygonOffsetParameters(0, -4);
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetOpacity(1.0);
    m_actor->GetProperty()->SetLighting(true);
    m_actor->GetProperty()->SetInterpolationToPhong();
    m_actor->GetProperty()->SetAmbient(0.35);
    m_actor->GetProperty()->SetDiffuse(0.65);
    m_actor->GetProperty()->SetSpecular(0.12);
    m_actor->GetProperty()->SetSpecularPower(20.0);
    m_actor->SetPickable(false);
    AttachProp(m_actor);
    m_selection = vtkSmartPointer<vtkThreshold>::New();
    m_selection->SetInputArrayToProcess(0, 0, 0, vtkDataObject::FIELD_ASSOCIATION_CELLS,
        vtkDataSetAttributes::SCALARS);
    m_selection->SetThresholdFunction(vtkThreshold::THRESHOLD_BETWEEN);
    m_geometry = vtkSmartPointer<vtkGeometryFilter>::New();
    m_geometry->SetInputConnection(m_selection->GetOutputPort());
    m_outline = vtkSmartPointer<vtkPolyDataSilhouette>::New();
    m_outline->SetInputConnection(m_geometry->GetOutputPort());
    m_outline->SetEnableFeatureAngle(false);
    m_outline->SetProp3D(m_actor);
    auto outlineMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    outlineMapper->SetInputConnection(m_outline->GetOutputPort());
    outlineMapper->ScalarVisibilityOff();
    m_outlineActor = vtkSmartPointer<vtkActor>::New();
    m_outlineActor->SetMapper(outlineMapper);
    m_outlineActor->GetProperty()->SetColor(0.1, 0.95, 1);
    if (isPreview) m_outlineActor->GetProperty()->SetColor(1, 0.68, 0.16);
    m_outlineActor->GetProperty()->LightingOff();
    m_outlineActor->GetProperty()->SetLineWidth(2.5);
    m_outlineActor->VisibilityOff(); m_outlineActor->PickableOff();
    AttachProp(m_outlineActor);
    SetCaption(isPreview ? u8"部件编辑：候选" : u8"零件分割", isPreview ? 2 : 1);
    SetCaptionVisible(false);
}

void PartSurfaceOverlayStrategy::AttachRenderer(vtkSmartPointer<vtkRenderer> renderer)
{
    if (renderer) m_outline->SetCamera(renderer->GetActiveCamera());
    FeatureOverlayBase::AttachRenderer(renderer);
}
void PartSurfaceOverlayStrategy::DetachRenderer(vtkSmartPointer<vtkRenderer> renderer)
{
    FeatureOverlayBase::DetachRenderer(renderer);
    m_outline->SetCamera(nullptr);
}

void PartSurfaceOverlayStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* surface = vtkPolyData::SafeDownCast(data);
    if (!surface) return;
    m_normals->SetInputData(surface);
    m_normals->Update();
    m_selection->SetInputData(surface);
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
        if (!m_mapper->GetInput() || !SetLookupTable(*m_lut, states, m_partStates, false, m_isSelectionOnly)
            || !SetLookupTable(*m_pickLut, states, m_pickStates)) return false;
        // DVR 仅叠加选中位置，不以整套不透明表面覆盖真实体渲染。
        // 拾取仍使用完整目录的可见性，不能把“未高亮”误判为“业务隐藏”。
        m_actor->SetVisibility(!m_isSelectionOnly || std::any_of(states.statesByLabel.begin(), states.statesByLabel.end(),
            [](const auto& state) { return state.isSelected && state.color[3] > 0.0; }));
        const auto partCount = static_cast<std::uint32_t>(
            states.statesByLabel.size() - 1U);
        m_mapper->SetScalarRange(
            0.0,
            static_cast<double>(std::max<std::uint32_t>(1U, partCount)));
        int selected = -1;
        for (std::size_t i = 1; i < states.statesByLabel.size(); ++i)
            if ((m_isPreview || states.statesByLabel[i].isSelected) && states.statesByLabel[i].color[3] > 0) {
                selected = static_cast<int>(i); break;
            }
        m_selection->SetLowerThreshold(selected);
        m_selection->SetUpperThreshold(selected);
        m_outlineActor->SetVisibility(selected > 0);
        SetCaptionVisible(m_actor->GetVisibility() && std::any_of(states.statesByLabel.begin(), states.statesByLabel.end(),
            [](const auto& part) { return part.color[3] > 0.0; }));
        return true;
    }
    catch (...) {
        return false;
    }
}

std::optional<PartLabelId> PartSurfaceOverlayStrategy::GetPickedLabel(
    const int x, const int y, vtkRenderer* renderer) const
{
    return ::GetPickedLabel(*m_actor, m_mapper->GetInput(), *m_pickLut, x, y, renderer, m_isSelectionOnly);
}

PartSliceOverlayStrategy::PartSliceOverlayStrategy(
    const Orientation orientation, vtkImageData* previous)
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
    m_reslice = vtkSmartPointer<vtkImageReslice>::New();
    m_reslice->SetOutputDimensionality(2);
    m_reslice->AutoCropOutputOn();
    m_reslice->SetInterpolationModeToNearestNeighbor();
    m_selection = vtkSmartPointer<vtkImageThreshold>::New();
    m_selection->SetInputConnection(m_reslice->GetOutputPort());
    m_selection->ReplaceInOn(); m_selection->ReplaceOutOn();
    m_selection->SetInValue(1); m_selection->SetOutValue(0);
    m_selection->SetOutputScalarTypeToUnsignedChar();
    if (previous) {
        m_previousReslice=vtkSmartPointer<vtkImageReslice>::New();
        m_previousReslice->SetInputData(previous);
        m_previousReslice->SetOutputDimensionality(2);
        m_previousReslice->AutoCropOutputOff();
        m_previousReslice->SetInterpolationModeToNearestNeighbor();
        m_difference=vtkSmartPointer<vtkImageMathematics>::New();
        m_difference->SetOperationToSubtract();
        m_difference->SetInputConnection(0,m_reslice->GetOutputPort());
        m_difference->AddInputConnection(0,m_previousReslice->GetOutputPort());
        m_selection->SetInputConnection(m_difference->GetOutputPort());
        m_selection->ThresholdBetween(0,0);
        m_selection->SetInValue(0);m_selection->SetOutValue(1);
    }
    m_outline = vtkSmartPointer<vtkFlyingEdges2D>::New();
    m_outline->SetInputConnection(m_selection->GetOutputPort());
    m_outline->SetValue(0, 0.5);
    auto outlineMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    outlineMapper->SetInputConnection(m_outline->GetOutputPort());
    outlineMapper->ScalarVisibilityOff();
    m_outlineActor = vtkSmartPointer<vtkActor>::New();
    m_outlineActor->SetMapper(outlineMapper);
    m_outlineActor->GetProperty()->SetColor(0.1, 0.95, 1);
    if (previous) m_outlineActor->GetProperty()->SetColor(1,0.68,0.16);
    m_outlineActor->GetProperty()->LightingOff();
    m_outlineActor->GetProperty()->SetLineWidth(2);
    m_outlineActor->VisibilityOff(); m_outlineActor->PickableOff();
    AttachProp(m_outlineActor);
    SetCaption(previous ? u8"部件编辑：候选" : u8"零件分割", previous ? 2 : 1);
    SetCaptionVisible(false);
}

void PartSliceOverlayStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* image = vtkImageData::SafeDownCast(data);
    if (!image) return;
    m_mapper->SetInputData(image);
    m_reslice->SetInputData(image);

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
    // 仅重采样当前二维切面供轮廓使用；不创建整卷标签副本。
    const int axis = m_orientation == Orientation::Top_down ? 2
        : m_orientation == Orientation::Front_back ? 1 : 0;
    vtkNew<vtkMatrix4x4> axes; axes->Zero(); axes->SetElement(3, 3, 1);
    axes->SetElement((axis + 1) % 3, 0, 1);
    axes->SetElement((axis + 2) % 3, 1, 1);
    axes->SetElement(axis, 2, 1);
    for (int i = 0; i < 3; ++i) axes->SetElement(i, 3, plane.worldOrigin[i]);
    vtkNew<vtkMatrix4x4> model; model->DeepCopy(state.modelToWorld.data());
    vtkNew<vtkMatrix4x4> inverse; vtkMatrix4x4::Invert(model, inverse);
    vtkNew<vtkMatrix4x4> dataAxes; vtkMatrix4x4::Multiply4x4(inverse, axes, dataAxes);
    m_reslice->SetResliceAxes(dataAxes);
    if (m_previousReslice) {
        m_previousReslice->SetResliceAxes(dataAxes);
        // 两路采样必须在同一个二维网格逐点比较，不能各自推导 autocrop。
        m_reslice->UpdateInformation();
        auto* information = m_reslice->GetOutputInformation(0);
        m_previousReslice->SetOutputExtent(information->Get(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT()));
        m_previousReslice->SetOutputOrigin(information->Get(vtkDataObject::ORIGIN()));
        m_previousReslice->SetOutputSpacing(information->Get(vtkDataObject::SPACING()));
        m_previousReslice->SetOutputDirection(information->Get(vtkDataObject::DIRECTION()));
    }
    m_outlineActor->SetUserMatrix(axes);
}

bool PartSliceOverlayStrategy::SetPartStates(
    const PartRenderStateTable& states) noexcept
{
    try {
        // 保留灰度切片细节，选中零件才加强填充。
        if (!SetLookupTable(*m_lut, states, m_partStates, true)) return false;
        int selected = -1;
        for (std::size_t i = 1; i < states.statesByLabel.size(); ++i)
            if (states.statesByLabel[i].isSelected && states.statesByLabel[i].color[3] > 0) {
                selected = static_cast<int>(i); break;
            }
        if (!m_difference) m_selection->ThresholdBetween(selected, selected);
        m_outlineActor->SetVisibility(m_difference != nullptr || selected > 0);
        SetCaptionVisible(std::any_of(states.statesByLabel.begin(), states.statesByLabel.end(),
            [](const auto& part) { return part.color[3] > 0.0; }));
        return true;
    }
    catch (...) {
        return false;
    }
}

std::optional<PartLabelId> PartSliceOverlayStrategy::GetPickedLabel(
    const int x, const int y, vtkRenderer* renderer) const
{
    return ::GetPickedLabel(*m_slice, m_mapper->GetInput(), *m_lut, x, y, renderer);
}
