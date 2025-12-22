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

    //  获取当前朝向，供 Service 查询以决定更新 x, y 还是 z
    Orientation GetOrientation() const { return m_orientation; }

    // 调整切片位置
    void SetSliceIndex(int delta);

    // 设置切片朝向
    void SetOrientation(Orientation orient);

private:
    void UpdatePlanePosition();
};


class MultiSliceStrategy : public AbstractVisualStrategy {
private:
    vtkSmartPointer<vtkImageSlice> m_slices[3];
    vtkSmartPointer<vtkImageResliceMapper> m_mappers[3];

    // 记录三个面的当前索引
    int m_indices[3] = { 0, 0, 0 };

public:
    MultiSliceStrategy();

    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;

    // 这个接口让 Service 一次性把 (x,y,z) 全传进来
    void UpdateAllPositions(int x, int y, int z);
};


class CompositeStrategy : public AbstractVisualStrategy {
private:
    // 主视图策略 (VolumeStrategy 或 IsoSurfaceStrategy)
    std::shared_ptr<AbstractVisualStrategy> m_mainStrategy;
    // 参考平面策略 (MultiSliceStrategy)
    std::shared_ptr<AbstractVisualStrategy> m_referencePlanes; // 使用基类指针

    VizMode m_mode;

public:
    CompositeStrategy(VizMode mode);

    void SetReferenceData(vtkSmartPointer<vtkImageData> img);
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) override;

    // 专门用于更新参考平面的接口
    void UpdateReferencePlanes(int x, int y, int z);
};
