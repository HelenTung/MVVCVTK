#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Render/Strategies/OrthogonalCropPreviewOverlayStrategy.h
// 分类: Strategy / Preview Overlay
// 说明: 把裁切结果渲染为 outline、mask slice 或 clipped polydata overlay。
// =====================================================================
// 显示主链路：
// 1. bridge 把统一的 OrthogonalCropResult 推给 overlay strategy
// 2. strategy 从结果里拆出 box 3D outline / image 2D mask / 可选 polydata 3D clip
// 3. 再根据当前窗口是 2D 还是 3D，决定显示线框参照、mask slice 或可选 polydata
// 4. 所有 prop 跟随主模型变换；颜色只表达 overlay 类型，不表达裁切模式

#include "BaseVisualStrategy.h"
#include "OrthogonalCropTypes.h"

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

    // 记录当前预览模式；颜色不再随模式切换，避免非交互态裁切盒反复变色。
    void SetRemovalMode(CropRemovalMode removalMode);

    // 控制 3D 几何参照线框是否显示；裁切效果、2D mask 和清理链路不受这个开关影响。
    // 为什么由 host/bridge 注入：只有宿主知道哪个窗口是 reference，feature overlay 不能自己猜窗口角色。
    void SetRefVisible(bool isVisible);

    // 注入一次完整裁切结果，自动更新 outline / mask / 可选 polydata 三类显示内容。
    void SetCropResult(const OrthogonalCropResult& result);

    // 清空当前 preview 可视内容。
    void ClearPreview();

    // 根据变换、游标等状态同步 overlay 视觉表现。
    // 这里不重新请求后端结果，只消费已经存在的 crop result 和窗口状态。
    void SetVisualState(const RenderParams& params, UpdateFlags flags) override;

private:
    // 根据当前是否有 outline / mask 和窗口轴向决定显示哪些 prop。
    void SetVisibleProps();

    // 更新 overlay 固定样式；交互选中高亮由 widget 自己负责。
    void SetStyle();

    // 把主模型矩阵同步到 overlay prop。
    static void SetPropTransform(vtkProp3D* prop, const std::array<double, 16>& modelToWorldMatrixData);

    // 旧的 3D 实体区域 actor；当前固定隐藏，保留对象只是为了少改动既有 prop 生命周期。
    vtkSmartPointer<vtkActor> m_previewRegionActor;

    // 旧的 3D 实体区域 mapper。
    vtkSmartPointer<vtkPolyDataMapper> m_previewRegionMapper;

    // 裁切盒 outline actor。
    vtkSmartPointer<vtkActor> m_outlineActor;

    // 裁切盒 outline mapper。
    vtkSmartPointer<vtkPolyDataMapper> m_outlineMapper;

    // polydata 可选裁切结果 actor。
    vtkSmartPointer<vtkActor> m_polyDataActor;

    // polydata 可选裁切结果 mapper。
    vtkSmartPointer<vtkPolyDataMapper> m_polyDataMapper;

    // 2D submit mask image slice。
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

    // 当前预览模式；裁切保留/移除语义由 preview plug 和算法执行，不再通过颜色表达。
    CropRemovalMode m_removalMode = CropRemovalMode::KeepInside;

    // 是否允许当前窗口显示 3D 几何参照线框。
    // 非 reference 预览窗口仍可显示裁切后的主模型效果，但不再额外画一个像可拖拽 box 的线框。
    bool m_allowGeometryReferenceVisible = true;

    // 当前窗口切片轴；3D 时通常为 -1。
    int m_sliceAxis = -1;
};
