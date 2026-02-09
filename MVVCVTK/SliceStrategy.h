#pragma once
#include "AppInterfaces.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkImageSlice.h>
#include <vtkImageResliceMapper.h>
#include <vtkLineSource.h>
#include <vtkPlane.h>
#include <vtkPlaneSource.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCubeAxesActor.h>
#include <vtkFlyingEdges3D.h>
#include <vtkPolyDataMapper.h>

// --- 策略 C: 2D 切片 (MPR) ---
// index = z*dx*dy + y*dx + x
class SliceStrategy : public AbstractVisualStrategy {
public:
    SliceStrategy(Orientation orient);

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) override;
    void UpdateVisuals(const RenderParams& params, UpdateFlags flags) override;
    int GetNavigationAxis() const override { return (int)GetOrientation(); }
    // [Public] 业务必需接口：供 Service 查询交互轴向

private:
    // [Private] 内部实现：由 UpdateVisuals 内部驱动
    void SetSliceIndex(int delta);
    void SetOrientation(Orientation orient);
    void UpdateCrosshair(int x, int y, int z);
    void ApplyColorMap(vtkSmartPointer<vtkColorTransferFunction> ctf);
    void UpdatePlanePosition();
    Orientation GetOrientation() const { return m_orientation; }

    vtkSmartPointer<vtkImageSlice> m_slice;
    vtkSmartPointer<vtkImageResliceMapper> m_mapper;
    Orientation m_orientation;

    // 状态记录
    int m_currentIndex = 0;
    int m_maxIndex = 0;

    // --- 十字线相关 ---
    vtkSmartPointer<vtkActor> m_vLineActor; // 垂直线
    vtkSmartPointer<vtkActor> m_hLineActor; // 水平线
    vtkSmartPointer<vtkLineSource> m_vLineSource;
    vtkSmartPointer<vtkLineSource> m_hLineSource;

    // 颜色映射缓存LUT
    vtkSmartPointer<vtkColorTransferFunction> m_lut;
};