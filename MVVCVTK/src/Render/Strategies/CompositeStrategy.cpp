#include "CompositeStrategy.h"
#include "VolumeStrategy.h"
#include "IsoSurfaceStrategy.h"
#include "ColoredPlanesStrategy.h"
#include <vtkCamera.h>

CompositeStrategy::CompositeStrategy(VizMode mode) : m_mode(mode) {
    // 始终创建参考平面
    m_referencePlanes = std::make_shared<ColoredPlanesStrategy>();

    // 根据模式创建主策略
    if (m_mode == VizMode::CompositeVolume) {
        m_mainStrategy = std::make_shared<VolumeStrategy>();
    }
    else if (m_mode == VizMode::CompositeIsoSurface) {
        m_mainStrategy = std::make_shared<IsoSurfaceStrategy>();
    }
}

void CompositeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    if (m_lastInput == data) {
        return;
    }
    m_lastInput = data;

    // 组合策略本身不产生新 VTK 对象，它只是把同一份输入同时分发给主内容和参考平面两部分。

    if (m_mainStrategy) {
        m_mainStrategy->SetInputData(data);
    }

    if (m_referencePlanes) {
        m_referencePlanes->SetInputData(data);
    }
}

void CompositeStrategy::AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) {
    if (m_mainStrategy) m_mainStrategy->AttachRenderer(renderer);
    if (m_referencePlanes) m_referencePlanes->AttachRenderer(renderer);
}

void CompositeStrategy::DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) {
    if (m_mainStrategy) m_mainStrategy->DetachRenderer(renderer);
    if (m_referencePlanes) m_referencePlanes->DetachRenderer(renderer);
}

void CompositeStrategy::ConfigureCamera(vtkSmartPointer<vtkRenderer> renderer) {
    // 通常 3D 视图使用透视投影
    if (renderer && renderer->GetActiveCamera()) {
        renderer->GetActiveCamera()->ParallelProjectionOff();
    }
}

int CompositeStrategy::GetPlaneAxis(vtkActor* actor) {
    return m_referencePlanes->GetPlaneAxis(actor);
}

vtkProp3D* CompositeStrategy::GetMainProp()
{
    if (m_mainStrategy) return m_mainStrategy->GetMainProp();
    else return nullptr;
}

void CompositeStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    // 参考平面先同步，这样 3D 主内容与切片参照在同一帧里看到的是一致状态。
    if (m_referencePlanes) {
        m_referencePlanes->SetVisualState(params, flags);
    }

    // 更新主视图
    if (m_mainStrategy) {
        m_mainStrategy->SetVisualState(params, flags);
    }
}
