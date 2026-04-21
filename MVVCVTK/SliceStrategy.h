#pragma once
#include "BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkImageSlice.h>
#include <vtkImageResliceMapper.h>
#include <vtkLineSource.h>
#include <vtkPlane.h>
#include <vtkPlaneSource.h>
#include <vtkLookupTable.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCubeAxesActor.h>
#include <vtkFlyingEdges3D.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>

// --- 策略 C: 2D 切片 (MPR) ---
// index = z*dx*dy + y*dx + x
class SliceStrategy : public BaseVisualStrategy {
public:
    SliceStrategy(Orientation orient);

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetRendererAttached(vtkSmartPointer<vtkRenderer> renderer);
    void SetCameraConfigured(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    int GetNavigationAxis() const override { return (int)m_orientation; }
    // [Public] 业务必需接口：供 Service 查询交互轴向

private:
    //    Transform 支持：4 个局部端点经 M 变换到世界空间后提交。
    //
    //  cx/cy/cz  — 游标索引（数据空间）
    //  bounds    — 数据在局部空间的包围盒 [xMin,xMax, yMin,yMax, zMin,zMax]
    //  origin    — 数据原点
    //  spacing   — 数据间距
    //  mat       — 当前模型矩阵 M（局部→世界）
    //  safeOffset— 沿法线微量偏移，防穿模闪烁
    void SetCrosshair(const double focusModel[3],
        const double bounds[6],
        double safeOffset);

    vtkWeakPointer<vtkRenderer> m_renderer;
    vtkSmartPointer<vtkImageSlice> m_slice;
    vtkSmartPointer<vtkImageResliceMapper> m_mapper;
    Orientation m_orientation;

    // --- 十字线相关 ---
    vtkSmartPointer<vtkActor> m_vLineActor; // 垂直线
    vtkSmartPointer<vtkActor> m_hLineActor; // 水平线
    vtkSmartPointer<vtkLineSource> m_vLineSource;
    vtkSmartPointer<vtkLineSource> m_hLineSource;
};