#pragma once

// =====================================================================
// Path: MVVCVTK/include/Strategies/OrthogonalCropPreviewStrategy/OrthogonalCropPreviewOverlayStrategy.h
// 分类: Strategy / Preview Overlay
// 说明: 把裁切结果渲染为 outline、mask slice 或 clipped polydata overlay。
// =====================================================================
// 显示主链路：
// 1. bridge 把统一的 OrthogonalCropResult 推给 overlay strategy
// 2. strategy 从结果里拆出 outline / virtual mask / derived polydata
// 3. 再根据当前窗口是 2D 还是 3D，决定显示实体区域、mask slice 还是 polydata
// 4. 所有 prop 都共享同一套 removal mode 颜色语义与主模型变换

#include "BaseVisualStrategy.h"
#include "OrthogonalCrop/OrthogonalCropTypes.h"

#include <vtkActor.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkLookupTable.h>
#include <vtkPlane.h>
#include <vtkPolyDataMapper.h>
#include <vtkProp3D.h>

class OrthogonalCropPreviewOverlayStrategy : public BaseVisualStrategy {
public:
    OrthogonalCropPreviewOverlayStrategy();

    // BaseVisualStrategy 要求的输入接口；本策略实际通过 SetCropResult 驱动。
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;

    // 指定当前窗口是 2D 哪个切片轴，-1 表示 3D 视图。
    void SetSliceAxis(int axis);

    // 设置预览颜色语义：保留盒内或移除盒内。
    void SetRemovalMode(CropRemovalMode removalMode);

    // 注入一次完整裁切结果，自动更新 outline / mask / polydata 三类显示内容。
    void SetCropResult(const OrthogonalCropResult& result);

    // 清空当前 preview 可视内容。
    void ClearPreview();

    // 根据变换、游标等状态同步 overlay 视觉表现。
    // 这里不重新请求后端结果，只消费已经存在的 crop result 和窗口状态。
    void SetVisualState(const RenderParams& params, UpdateFlags flags) override;

private:
    // 根据当前是否有 outline / mask 和窗口轴向决定显示哪些 prop。
    void UpdateVisiblePreviewProps();

    // 按 removal mode 更新 preview 颜色与 LUT。
    void ApplyRemovalVisualStyle();

    // 把主模型矩阵同步到 overlay prop。
    static void SetPropTransform(vtkProp3D* prop, const std::array<double, 16>& modelToWorldMatrixData);

    // 3D 预览区域 actor，显示裁切盒实体范围。
    vtkSmartPointer<vtkActor> m_previewRegionActor;

    // 3D 预览区域 mapper。
    vtkSmartPointer<vtkPolyDataMapper> m_previewRegionMapper;

    // 裁切盒 outline actor。
    vtkSmartPointer<vtkActor> m_outlineActor;

    // 裁切盒 outline mapper。
    vtkSmartPointer<vtkPolyDataMapper> m_outlineMapper;

    // polydata 裁切结果 actor。
    vtkSmartPointer<vtkActor> m_polyDataActor;

    // polydata 裁切结果 mapper。
    vtkSmartPointer<vtkPolyDataMapper> m_polyDataMapper;

    // 2D mask 预览 image slice。
    vtkSmartPointer<vtkImageSlice> m_maskSlice;

    // 2D mask slice 对应的 reslice mapper。
    vtkSmartPointer<vtkImageResliceMapper> m_maskMapper;

    // mask 颜色查找表。
    vtkSmartPointer<vtkLookupTable> m_maskLut;

    // 当前切片平面。
    vtkSmartPointer<vtkPlane> m_slicePlane;

    // 当前是否已经有 outline 可显示。
    bool m_hasOutline = false;

    // 当前是否已经有 mask image 可显示。
    bool m_hasMaskImage = false;

    // 当前颜色语义。
    CropRemovalMode m_removalMode = CropRemovalMode::KeepInside;

    // 当前窗口切片轴；3D 时通常为 -1。
    int m_sliceAxis = -1;
};
