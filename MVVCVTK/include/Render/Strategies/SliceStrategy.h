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
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer);
    void ConfigureCamera(vtkSmartPointer<vtkRenderer> renderer);
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
    //  modelToWorldMatrix — 当前模型矩阵 M（局部→世界）
    //  safeOffset— 沿法线微量偏移，防穿模闪烁
    void SetCrosshair(const double focusWorld[3],
        const double worldBounds[6],
        double safeOffset);
    void AlignCamera(const std::array<double, 16>& modelMatrix,
        const double bounds[6]);
    void SetWorldBounds(const double bounds[6],
        const std::array<double, 16>& modelMatrix,
        double worldBounds[6]) const; // 把局部数据包围盒映射到当前模型变换后的世界包围盒

    vtkWeakPointer<vtkRenderer> m_renderer; // 当前切片视图依附的 renderer，用于相机和窗口尺寸相关操作 
    vtkSmartPointer<vtkImageSlice> m_slice; // 切片主显示对象
    vtkSmartPointer<vtkImageResliceMapper> m_mapper; // 负责按当前切片平面从体数据抽取图像
    vtkSmartPointer<vtkPlane> m_slicePlane;           // 持久切片平面，保持 mapper 输入关系稳定，仅更新原点与法线
    vtkSmartPointer<vtkDataObject> m_lastInput;       // 缓存当前输入，避免重复绑定相同数据触发额外管线更新
    Orientation m_orientation; // 当前切片朝向，决定法线方向、相机姿态与十字线配色

    // --- 十字线相关 ---
    vtkSmartPointer<vtkActor> m_vLineActor; // 垂直线
    vtkSmartPointer<vtkActor> m_hLineActor; // 水平线
    vtkSmartPointer<vtkLineSource> m_vLineSource; // 十字线垂直段几何源
    vtkSmartPointer<vtkLineSource> m_hLineSource; // 十字线水平段几何源
};
