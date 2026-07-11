#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Render/Strategies/CropOverlay.h
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

class CropOverlay : public BaseVisualStrategy {
public:
    CropOverlay();

    // BaseVisualStrategy 要求的输入接口；本策略实际通过 SetCropResult 驱动。
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;

    // 指定当前窗口是 2D 哪个切片轴，-1 表示 3D 视图。
    void SetSliceAxis(int axis);

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

    // 构造期创建并由本策略与 BaseVisualStrategy prop 列表共同保留的 Box outline pipeline；
    // mapper 保留最后输入，m_hasOutline/ClearPreview 负责决定其是否可见。
    vtkSmartPointer<vtkActor> m_outlineActor;
    vtkSmartPointer<vtkPolyDataMapper> m_outlineMapper;

    // 构造期创建并保留的可选 clipped-polydata pipeline；每次成功 result 替换 mapper 输入和可见性。
    vtkSmartPointer<vtkActor> m_polyDataActor;
    vtkSmartPointer<vtkPolyDataMapper> m_polyDataMapper;

    // 2D mask pipeline；mapper 通过 VTK 引用计数保留最新 mask，ClearPreview 只关闭可见状态。
    vtkSmartPointer<vtkImageSlice> m_maskSlice;
    vtkSmartPointer<vtkImageResliceMapper> m_maskMapper;
    // 本策略持有的 LUT：0 透明、非零为固定 preview 色，生命周期覆盖整个策略。
    vtkSmartPointer<vtkLookupTable> m_maskLut;
    // mapper 引用的当前切片平面；SetVisualState 用 cursor 和 m_sliceAxis 原地更新。
    vtkSmartPointer<vtkPlane> m_slicePlane;

    // 从最新成功 result 派生的 payload 有效位；ClearPreview 清零，但不作为几何真源。
    bool m_hasOutline = false;
    bool m_hasMaskImage = false;

    // 是否允许当前窗口显示 3D 几何参照线框。
    // 非 reference 预览窗口仍可显示裁切后的主模型效果，但不再额外画一个像可拖拽 box 的线框。
    bool m_isRefVisible = true;

    // 当前窗口切片轴：0/1/2 选择轴法线，负值表示 3D；其它正值会使 mask 隐藏。
    int m_sliceAxis = -1;
};
