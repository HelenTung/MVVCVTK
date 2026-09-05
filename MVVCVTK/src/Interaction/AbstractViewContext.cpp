#include "Interaction/AbstractViewContext.h"

#include <vtkCamera.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>

#include <algorithm>
#include <utility>

namespace {

void SetTransparencyEnvironment(vtkRenderer& renderer)
{
    renderer.SetUseDepthPeeling(1);
    renderer.SetMaximumNumberOfPeels(4);
    renderer.SetOcclusionRatio(0.0);
}

} // namespace

AbstractViewContext::AbstractViewContext()
    : m_renderer(vtkSmartPointer<vtkRenderer>::New())
    , m_renderWindow(vtkSmartPointer<vtkRenderWindow>::New())
{
    SetTransparencyEnvironment(*m_renderer.GetPointer());
    m_renderWindow->AddRenderer(m_renderer);
}

AbstractViewContext::~AbstractViewContext()
{
    if (GetIsOwnerThread() && m_renderWindow && m_renderer) {
        m_renderWindow->RemoveRenderer(m_renderer);
    }
}

bool AbstractViewContext::SetRenderWindow(
    vtkSmartPointer<vtkRenderWindow> renderWindow)
{
    if (!GetIsOwnerThread() || !renderWindow) return false;
    if (renderWindow.GetPointer() == m_renderWindow.GetPointer()) {
        return true;
    }
    if (m_renderWindow && m_renderer) {
        m_renderWindow->RemoveRenderer(m_renderer);
    }
    m_renderWindow = std::move(renderWindow);
    if (m_renderer) {
        SetTransparencyEnvironment(*m_renderer.GetPointer());
        m_renderWindow->AddRenderer(m_renderer);
    }
    return true;
}

bool AbstractViewContext::SendRender()
{
    if (!GetIsOwnerThread() || !m_renderWindow) return false;
    m_renderWindow->Render();
    return true;
}

bool AbstractViewContext::ResetCamera()
{
    if (!GetIsOwnerThread() || !m_renderer) return false;
    m_renderer->ResetCamera();
    return true;
}

std::optional<ViewCameraState>
AbstractViewContext::GetCameraState() const
{
    if (!GetIsOwnerThread() || !m_renderer
        || !m_renderer->GetActiveCamera()) {
        return std::nullopt;
    }
    auto* camera = m_renderer->GetActiveCamera();
    const auto* position = camera->GetPosition();
    const auto* focalPoint = camera->GetFocalPoint();
    const auto* viewUp = camera->GetViewUp();
    const auto* clippingRange = camera->GetClippingRange();
    if (!position || !focalPoint || !viewUp || !clippingRange) {
        return std::nullopt;
    }

    ViewCameraState state;
    std::copy_n(position, state.position.size(), state.position.begin());
    std::copy_n(
        focalPoint, state.focalPoint.size(), state.focalPoint.begin());
    std::copy_n(viewUp, state.viewUp.size(), state.viewUp.begin());
    std::copy_n(
        clippingRange,
        state.clippingRange.size(),
        state.clippingRange.begin());
    state.parallelScale = camera->GetParallelScale();
    state.viewAngle = camera->GetViewAngle();
    state.isParallel = camera->GetParallelProjection() != 0;
    return state;
}

bool AbstractViewContext::SetCameraState(
    const ViewCameraState& state)
{
    if (!GetIsOwnerThread() || !m_renderer
        || !m_renderer->GetActiveCamera()) {
        return false;
    }
    auto* camera = m_renderer->GetActiveCamera();
    camera->SetPosition(state.position.data());
    camera->SetFocalPoint(state.focalPoint.data());
    camera->SetViewUp(state.viewUp.data());
    camera->SetClippingRange(state.clippingRange.data());
    camera->SetParallelScale(state.parallelScale);
    camera->SetViewAngle(state.viewAngle);
    camera->SetParallelProjection(state.isParallel ? 1 : 0);
    return true;
}

vtkRenderer* AbstractViewContext::GetRenderer() const
{
    return GetIsOwnerThread() ? m_renderer.GetPointer() : nullptr;
}

vtkRenderWindow* AbstractViewContext::GetRenderWindow() const
{
    return GetIsOwnerThread() ? m_renderWindow.GetPointer() : nullptr;
}

bool AbstractViewContext::SetWindowSize(
    const int width,
    const int height)
{
    if (!GetIsOwnerThread() || !m_renderWindow) return false;
    m_renderWindow->SetSize(width, height);
    return true;
}

bool AbstractViewContext::SetWindowPosition(
    const int x,
    const int y)
{
    if (!GetIsOwnerThread() || !m_renderWindow) return false;
    m_renderWindow->SetPosition(x, y);
    return true;
}

bool AbstractViewContext::SetWindowTitle(const std::string& title)
{
    if (!GetIsOwnerThread() || !m_renderWindow) return false;
    m_renderWindow->SetWindowName(title.c_str());
    return true;
}

bool AbstractViewContext::SetRendererBackground(
    const BackgroundColor& background)
{
    if (!GetIsOwnerThread() || !m_renderer) return false;
    m_renderer->SetBackground(
        background.r, background.g, background.b);
    return true;
}

void AbstractViewContext::DispatchVTKEvent(
    vtkObject* caller,
    const unsigned long eventId,
    void* clientData,
    void* callData)
{
    auto* context = static_cast<AbstractViewContext*>(clientData);
    if (context) context->OnVTKEvent(caller, eventId, callData);
}

void AbstractViewContext::OnVTKEvent(
    vtkObject*,
    const unsigned long,
    void*)
{
}

bool AbstractViewContext::GetIsOwnerThread() const noexcept
{
    return m_ownerThread == std::this_thread::get_id();
}
