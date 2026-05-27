// =====================================================================
// Path: MVVCVTK/src/Strategies/OrthogonalCropPreviewStrategy/OrthogonalCropPreviewOverlayStrategy.cpp
// 分类: Strategy / Preview Overlay Implementation
// 说明: 把裁切结果拆成 3D outline、3D clipped polydata、2D mask slice 三类可视表现。
// =====================================================================

#include "OrthogonalCropPreviewStrategy/OrthogonalCropPreviewOverlayStrategy.h"

#include <vtkImageProperty.h>
#include <vtkMatrix4x4.h>
#include <vtkProperty.h>

OrthogonalCropPreviewOverlayStrategy::OrthogonalCropPreviewOverlayStrategy()
{
    // 3D 区域体、outline、polydata 结果和 2D mask slice 在构造时全部建好，
    // 后续每次 preview 只切换输入与可见性，避免频繁重建 prop。
    m_previewRegionActor = vtkSmartPointer<vtkActor>::New();
    m_previewRegionMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_previewRegionMapper->ScalarVisibilityOff();
    m_previewRegionActor->SetMapper(m_previewRegionMapper);
    m_previewRegionActor->GetProperty()->SetOpacity(0.18);
    m_previewRegionActor->GetProperty()->SetLighting(false);
    m_previewRegionActor->SetPickable(false);
    m_previewRegionActor->SetVisibility(false);

    m_outlineActor = vtkSmartPointer<vtkActor>::New();
    m_outlineMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_outlineMapper->ScalarVisibilityOff();
    m_outlineActor->SetMapper(m_outlineMapper);
    m_outlineActor->GetProperty()->SetColor(1.0, 0.55, 0.12);
    m_outlineActor->GetProperty()->SetLineWidth(2.0);
    m_outlineActor->GetProperty()->SetLighting(false);
    m_outlineActor->GetProperty()->SetRepresentationToWireframe();
    m_outlineActor->SetPickable(false);
    m_outlineActor->SetVisibility(false);

    m_polyDataActor = vtkSmartPointer<vtkActor>::New();
    m_polyDataMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_polyDataActor->SetMapper(m_polyDataMapper);
    m_polyDataActor->GetProperty()->SetColor(1.0, 0.55, 0.12);
    m_polyDataActor->GetProperty()->SetOpacity(0.35);
    m_polyDataActor->GetProperty()->SetLighting(false);
    m_polyDataActor->SetPickable(false);
    m_polyDataActor->SetVisibility(false);

    m_maskSlice = vtkSmartPointer<vtkImageSlice>::New();
    m_maskMapper = vtkSmartPointer<vtkImageResliceMapper>::New();
    m_slicePlane = vtkSmartPointer<vtkPlane>::New();
    m_maskMapper->SliceFacesCameraOff();
    m_maskMapper->SliceAtFocalPointOff();
    m_maskMapper->SetSlicePlane(m_slicePlane);
    m_maskSlice->SetMapper(m_maskMapper);
    m_maskSlice->SetPickable(false);
    m_maskSlice->SetVisibility(false);

    m_maskLut = vtkSmartPointer<vtkLookupTable>::New();
    m_maskLut->SetNumberOfTableValues(256);
    m_maskLut->SetTableRange(0, 255);
    m_maskLut->SetTableValue(0, 0.0, 0.0, 0.0, 0.0);
    for (int value = 1; value < 256; ++value) {
        m_maskLut->SetTableValue(value, 1.0, 0.45, 0.05, 0.42);
    }
    m_maskLut->Build();

    m_maskSlice->GetProperty()->SetLookupTable(m_maskLut);
    m_maskSlice->GetProperty()->SetUseLookupTableScalarRange(1);
    m_maskSlice->GetProperty()->SetInterpolationTypeToNearest();
    m_maskSlice->GetProperty()->SetLayerNumber(2);

    ApplyRemovalVisualStyle();

    SetManagedProp(m_previewRegionActor);
    SetManagedProp(m_outlineActor);
    SetManagedProp(m_polyDataActor);
    SetManagedProp(m_maskSlice);
}

void OrthogonalCropPreviewOverlayStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data)
{
    (void)data;
}

void OrthogonalCropPreviewOverlayStrategy::SetSliceAxis(int axis)
{
    m_sliceAxis = axis;
    UpdateVisiblePreviewProps();
}

void OrthogonalCropPreviewOverlayStrategy::SetRemovalMode(CropRemovalMode removalMode)
{
    if (m_removalMode == removalMode) {
        return;
    }

    m_removalMode = removalMode;
    ApplyRemovalVisualStyle();
}

void OrthogonalCropPreviewOverlayStrategy::SetCropResult(const OrthogonalCropResult& result)
{
    if (!result.GetSucceeded()) {
        ClearPreview();
        return;
    }

    // outline 是所有窗口都共享的最低公共几何表示。
    auto outline = result.GetOutlinePolyData();
    m_hasOutline = outline && outline->GetNumberOfPoints() > 0;
    if (m_hasOutline) {
        m_outlineMapper->SetInputData(outline);
        m_previewRegionMapper->SetInputData(outline);
    }

    auto clippedPolyData = result.GetDerivedPolyData();
    const bool hasPolyData = clippedPolyData && clippedPolyData->GetNumberOfPoints() > 0;
    if (hasPolyData) {
        m_polyDataMapper->SetInputData(clippedPolyData);
    }
    m_polyDataActor->SetVisibility(hasPolyData ? 1 : 0);

    // image 虚拟裁切结果通过 mask image 提供给 2D 切片窗口显示。
    auto maskImage = result.GetVirtualMaskImage();
    m_hasMaskImage = maskImage != nullptr;
    if (m_hasMaskImage) {
        m_maskMapper->SetInputData(maskImage);
    }
    UpdateVisiblePreviewProps();
}

void OrthogonalCropPreviewOverlayStrategy::ClearPreview()
{
    m_hasOutline = false;
    m_hasMaskImage = false;
    m_previewRegionActor->SetVisibility(false);
    m_outlineActor->SetVisibility(false);
    m_polyDataActor->SetVisibility(false);
    m_maskSlice->SetVisibility(false);
}

void OrthogonalCropPreviewOverlayStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    if (HasFlag(flags, UpdateFlags::Transform)) {
        // 所有 3D prop 都跟随主模型矩阵，保持 overlay 与主内容严格对齐。
        SetPropTransform(m_previewRegionActor, params.modelMatrix);
        SetPropTransform(m_outlineActor, params.modelMatrix);
        SetPropTransform(m_polyDataActor, params.modelMatrix);
        if (m_hasMaskImage && m_sliceAxis >= 0) {
            SetPropTransform(m_maskSlice, params.modelMatrix);
        }
    }

    if (!m_hasMaskImage || m_sliceAxis < 0) {
        return;
    }

    if (HasFlag(flags, UpdateFlags::Cursor) || HasFlag(flags, UpdateFlags::Transform)) {
        double normal[3] = { 0.0, 0.0, 0.0 };
        if (m_sliceAxis >= 0 && m_sliceAxis < 3) {
            normal[m_sliceAxis] = 1.0;
        }
        else {
            m_maskSlice->SetVisibility(false);
            return;
        }

        m_slicePlane->SetOrigin(params.cursor[0], params.cursor[1], params.cursor[2]);
        m_slicePlane->SetNormal(normal[0], normal[1], normal[2]);

        // 2D 预览始终以当前窗口 cursor 所在切面切 mask。
        m_maskSlice->SetVisibility(true);
    }
}

void OrthogonalCropPreviewOverlayStrategy::UpdateVisiblePreviewProps()
{
    // 3D 窗口显示实体区域，2D 窗口显示 mask slice，两者都保留 outline 作为共同参照。
    m_outlineActor->SetVisibility(m_hasOutline ? 1 : 0);
    m_previewRegionActor->SetVisibility(m_hasOutline && m_sliceAxis < 0 ? 1 : 0);
    m_maskSlice->SetVisibility(m_hasMaskImage && m_sliceAxis >= 0 ? 1 : 0);
}

void OrthogonalCropPreviewOverlayStrategy::ApplyRemovalVisualStyle()
{
    // KeepInside / RemoveInside 用同一套 prop，但通过颜色语义区分“保留”与“移除”。
    const bool keepInside = m_removalMode == CropRemovalMode::KeepInside;
    const double red = keepInside ? 0.10 : 1.0;
    const double green = keepInside ? 0.90 : 0.18;
    const double blue = keepInside ? 0.45 : 0.06;

    m_previewRegionActor->GetProperty()->SetColor(red, green, blue);
    m_outlineActor->GetProperty()->SetColor(red, green, blue);

    for (int value = 1; value < 256; ++value) {
        m_maskLut->SetTableValue(value, red, green, blue, 0.42);
    }
    m_maskLut->Build();
}

void OrthogonalCropPreviewOverlayStrategy::SetPropTransform(vtkProp3D* prop, const std::array<double, 16>& matrixData)
{
    if (!prop) {
        return;
    }

    auto matrix = prop->GetUserMatrix();
    if (!matrix) {
        auto nextMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
        nextMatrix->DeepCopy(matrixData.data());
        prop->SetUserMatrix(nextMatrix);
        return;
    }

    matrix->DeepCopy(matrixData.data());
}
