#pragma once
#include "BaseVisualStrategy.h" 
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
class CompositeStrategy : public BaseVisualStrategy {
public:
    CompositeStrategy(VizMode mode);

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetRendererAttached(vtkSmartPointer<vtkRenderer> renderer);
    void SetRendererDetached(vtkSmartPointer<vtkRenderer> renderer);
    void SetCameraConfigured(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    int GetPlaneAxis(vtkActor* actor) override;
    vtkProp3D* GetMainProp() override; //
private:
    std::shared_ptr<AbstractVisualStrategy> GetMainStrategy() { return m_mainStrategy; }

    std::shared_ptr<AbstractVisualStrategy> m_mainStrategy; // 主 3D 内容：Volume 或 IsoSurface 二选一
    std::shared_ptr<AbstractVisualStrategy> m_referencePlanes; // 叠加在主内容上的三向参考平面
    vtkSmartPointer<vtkDataObject> m_lastInput; // 缓存当前组合输入，避免主策略与参考平面重复接收相同数据
    VizMode m_mode; // 决定主策略究竟是体渲染还是等值面
};