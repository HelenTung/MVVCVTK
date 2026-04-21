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
    if (m_mainStrategy) {
        m_mainStrategy->SetInputData(data);
    }

    if (m_referencePlanes) {
        m_referencePlanes->SetInputData(data);
    }
}

void CompositeStrategy::SetRendererAttached(vtkSmartPointer<vtkRenderer> renderer) {
    if (m_mainStrategy) m_mainStrategy->SetRendererAttached(renderer);
    if (m_referencePlanes) m_referencePlanes->SetRendererAttached(renderer);
    renderer->SetBackground(0.05, 0.05, 0.05);
}

void CompositeStrategy::SetRendererDetached(vtkSmartPointer<vtkRenderer> renderer) {
    if (m_mainStrategy) m_mainStrategy->SetRendererDetached(renderer);
    if (m_referencePlanes) m_referencePlanes->SetRendererDetached(renderer);
}

void CompositeStrategy::SetCameraConfigured(vtkSmartPointer<vtkRenderer> renderer) {
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
    if (m_referencePlanes) {
        m_referencePlanes->SetVisualState(params, flags);
    }

    // 更新主视图
    if (m_mainStrategy) {
        m_mainStrategy->SetVisualState(params, flags);
    }
}