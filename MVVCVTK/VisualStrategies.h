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

// --- 策略 A: 等值面渲染 ---
class IsoSurfaceStrategy : public AbstractVisualStrategy {
public:
    IsoSurfaceStrategy();

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) override;
    void UpdateVisuals(const RenderParams& params) override;

private:
    vtkSmartPointer<vtkActor> m_actor;
	vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes; // 坐标轴
};

// --- 策略 B: 体渲染 ---
class VolumeStrategy : public AbstractVisualStrategy {
public:
    VolumeStrategy();

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) override;
    void UpdateVisuals(const RenderParams& params) override;

private:
    // [Private] 内部辅助：应用传输函数
    void ApplyTransferParams(vtkSmartPointer<vtkColorTransferFunction> ctf,
        vtkSmartPointer<vtkPiecewiseFunction> otf);
	vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes; // 坐标轴
    vtkSmartPointer<vtkVolume> m_volume;
};

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
    void UpdateVisuals(const RenderParams& params) override;
    int GetNavigationAxis() const override { return (int)GetOrientation(); }
    // [Public] 业务必需接口：供 Service 查询交互轴向

private:
    // [Private] 内部实现：这些方法仅由 UpdateVisuals 内部驱动
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

// --- 策略 D: 三面切片 (MPR) ---
class MultiSliceStrategy : public AbstractVisualStrategy {
public:
    MultiSliceStrategy();

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void UpdateVisuals(const RenderParams& params) override;

private:
    // [Private] 内部实现：由 UpdateVisuals 统一调用
    void UpdateAllPositions(int x, int y, int z);

    vtkSmartPointer<vtkImageSlice> m_slices[3];
    vtkSmartPointer<vtkImageResliceMapper> m_mappers[3];
    int m_indices[3] = { 0, 0, 0 };
};

// --- 策略 E: 彩色切片平面 (红绿蓝) ---
class ColoredPlanesStrategy : public AbstractVisualStrategy {
public:
    ColoredPlanesStrategy();

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void UpdateVisuals(const RenderParams& params) override;
    int GetPlaneAxis(vtkActor* actor) override;

private:
    // [Private] 内部实现
    void UpdateAllPositions(int x, int y, int z);

    vtkSmartPointer<vtkActor> m_planeActors[3];
    vtkSmartPointer<vtkPlaneSource> m_planeSources[3];
    vtkSmartPointer<vtkImageData> m_imageData;
};

// --- 组合策略: 体渲染/等值面 + 切片平面 ---
class CompositeStrategy : public AbstractVisualStrategy {
public:
    CompositeStrategy(VizMode mode);

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) override;
    void UpdateVisuals(const RenderParams& params) override;
    int GetPlaneAxis(vtkActor* actor) override;

private:
    std::shared_ptr<AbstractVisualStrategy> GetMainStrategy() { return m_mainStrategy; }

    std::shared_ptr<AbstractVisualStrategy> m_mainStrategy;
    std::shared_ptr<AbstractVisualStrategy> m_referencePlanes;
    VizMode m_mode;
};