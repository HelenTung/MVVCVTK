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


// --- 组合策略: 体渲染/等值面 + 切片平面 ---
class CompositeStrategy : public AbstractVisualStrategy {
public:
    CompositeStrategy(VizMode mode);

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) override;
    void UpdateVisuals(const RenderParams& params, UpdateFlags flags) override;
    int GetPlaneAxis(vtkActor* actor) override;
    vtkProp3D* GetMainProp() override; //
private:
    std::shared_ptr<AbstractVisualStrategy> GetMainStrategy() { return m_mainStrategy; }

    std::shared_ptr<AbstractVisualStrategy> m_mainStrategy;
    std::shared_ptr<AbstractVisualStrategy> m_referencePlanes;
    VizMode m_mode;
};