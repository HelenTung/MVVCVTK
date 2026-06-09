// =====================================================================
// Path: MVVCVTK/src/Strategies/OrthogonalCropPreviewStrategy/OrthogonalCropPreviewOverlayStrategy.cpp
// 分类: Strategy / Preview Overlay Implementation
// 说明: 把裁切结果拆成 3D outline、3D clipped polydata、2D mask slice 三类可视表现。
// =====================================================================
// 这个策略的职责不是“计算裁切”，而是“解释裁切结果”：
// - 同一份结果在 2D 窗口主要表现为 mask slice
// - 在 3D 窗口主要表现为 outline / preview region / clipped polydata
// - 视觉语义由 removal mode 与当前窗口轴向共同决定

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
    // sliceAxis 直接决定当前窗口是走 2D mask 逻辑还是 3D preview region 逻辑。
    m_sliceAxis = axis;
    UpdateVisiblePreviewProps();
}

void OrthogonalCropPreviewOverlayStrategy::SetRemovalMode(CropRemovalMode removalMode)
{
    if (m_removalMode == removalMode) {
        return;
    }

    m_removalMode = removalMode;

    // 颜色语义切换不依赖新结果，当前缓存的 prop 直接改样式即可。
    ApplyRemovalVisualStyle();
}

void OrthogonalCropPreviewOverlayStrategy::SetCropResult(const OrthogonalCropResult& result)
{
    // ═══ 裁切结果 → 三类可视化数据分发 ═══
    // a) outlinePolyData  → m_outlineMapper    (3D 线框 + 2D 参考, 所有窗口共享)
    // b) derivedPolyData  → m_polyDataMapper   (仅 3D 窗口: clipped 半透明网格)
    // c) virtualMaskImage → m_maskMapper       (仅 2D 窗口: 半透明颜色叠加)
    if (!result.GetSucceeded()) {
        ClearPreview();
        return;
    }

    // ── 分发 A：outlinePolyData（所有窗口共享的公共几何表示） ──
    // 3D 窗口 → 裁切盒线框 + 半透明实体区域（m_previewRegionMapper）
    // 2D 窗口 → 裁切盒在切片上的投影轮廓
    auto outline = result.GetOutlinePolyData();
    m_hasOutline = outline && outline->GetNumberOfPoints() > 0;
    if (m_hasOutline) {
        m_outlineMapper->SetInputData(outline);
        m_previewRegionMapper->SetInputData(outline);
    }

    // ── 分发 B：derivedPolyData（仅 polydata 路径） ──
    // clipped 网格以半透明叠加方式显示在 3D 窗口
    // 若 3D 主窗口已被 bridge 常驻 clip 管道接管，bridge 会先剥离此字段避免重复绘制
    auto clippedPolyData = result.GetDerivedPolyData();
    const bool hasPolyData = clippedPolyData && clippedPolyData->GetNumberOfPoints() > 0;
    if (hasPolyData) {
        m_polyDataMapper->SetInputData(clippedPolyData);
    }
    m_polyDataActor->SetVisibility(hasPolyData ? 1 : 0);

    // ── 分发 C：virtualMaskImage（仅 image 路径虚拟裁切） ──
    // 2D 窗口 → vtkImageResliceMapper 切片显示，颜色由 m_maskLut 控制
    // 3D 窗口 → mask 不可见（只依赖 outline + 主模型 clip）
    auto maskImage = result.GetVirtualMaskImage();
    m_hasMaskImage = maskImage != nullptr;
    if (m_hasMaskImage) {
        m_maskMapper->SetInputData(maskImage);
    }

    // ── 最终可见性决策 ──
    // m_sliceAxis < 0 (3D)：显示 outline + region，隐藏 mask
    // m_sliceAxis >= 0 (2D)：显示 outline + mask，隐藏 region
    UpdateVisiblePreviewProps();
}

void OrthogonalCropPreviewOverlayStrategy::ClearPreview()
{
    // 清空时只重置本策略持有的显示状态，不触碰主模型或上游缓存结果。
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

void OrthogonalCropPreviewOverlayStrategy::SetPropTransform(vtkProp3D* prop, const std::array<double, 16>& modelToWorldMatrixData)
{
    if (!prop) {
        return;
    }

    auto userMatrix = prop->GetUserMatrix();
    if (!userMatrix) {
        // 首次赋值时为 prop 建一份 modelToWorld user matrix，后续只做 in-place 覆盖，避免重复分配。
        auto nextUserMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
        nextUserMatrix->DeepCopy(modelToWorldMatrixData.data());
        prop->SetUserMatrix(nextUserMatrix);
        return;
    }

    userMatrix->DeepCopy(modelToWorldMatrixData.data());
}
