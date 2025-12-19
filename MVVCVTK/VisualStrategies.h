#pragma once
#include "AppInterfaces.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkImageSlice.h>
#include <vtkImageResliceMapper.h>

// --- 策略 A: 等值面渲染 ---
class IsoSurfaceStrategy : public AbstractVisualStrategy {
private:
    vtkSmartPointer<vtkActor> m_actor;
public:
    IsoSurfaceStrategy();
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) override;
};

// --- 策略 B: 体渲染 ---
class VolumeStrategy : public AbstractVisualStrategy {
private:
    vtkSmartPointer<vtkVolume> m_volume;
public:
    VolumeStrategy();
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) override;
};

// --- 策略 C: 2D 切片 (MPR) ---
// index = z*dx*dy + y*dx + x
class SliceStrategy : public AbstractVisualStrategy {
private:
    vtkSmartPointer<vtkImageSlice> m_slice;
    vtkSmartPointer<vtkImageResliceMapper> m_mapper;
    Orientation m_orientation;
    
    // 状态记录
    int m_currentIndex = 0;
    int m_maxIndex = 0;
public:
    SliceStrategy(Orientation orient);
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) override; // 关键：平行投影

    // 调整切片位置
    void SetInteractionValue(int delta);

    // 设置切片朝向
    void SetOrientation(Orientation orient);

private:
    void UpdatePlanePosition();
};
