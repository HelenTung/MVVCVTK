#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "Interaction/AbstractViewContext.h"

#include <vtkCamera.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#if defined(_WIN32)
#include <vtkWin32OpenGLRenderWindow.h>
#endif

#include <algorithm>
#include <limits>
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
    // Generic 窗口由宿主建立上下文；原生窗口首次 Render 可以自行初始化。
    auto window = m_renderWindow;
    auto* genericWindow = vtkGenericOpenGLRenderWindow::SafeDownCast(window);
    if (genericWindow && !genericWindow->GetReadyForRendering()) return false;

    struct RenderWatch final {
        vtkSmartPointer<vtkRenderWindow> window;
        vtkSmartPointer<vtkCallbackCommand> callback;
        unsigned long endTag = 0;
        unsigned long errorTag = 0;
        bool hasEnded = false;
        bool hasError = false;

        ~RenderWatch()
        {
            window->RemoveObserver(endTag);
            window->RemoveObserver(errorTag);
            callback->SetClientData(nullptr);
        }
    };
    RenderWatch watch;
    watch.window = window;
    watch.callback = vtkSmartPointer<vtkCallbackCommand>::New();
    watch.callback->SetClientData(&watch);
    watch.callback->SetCallback([](vtkObject*, unsigned long eventId,
        void* clientData, void*) {
        auto& state = *static_cast<RenderWatch*>(clientData);
        if (eventId == vtkCommand::EndEvent) state.hasEnded = true;
        if (eventId == vtkCommand::ErrorEvent) state.hasError = true;
    });
    watch.endTag = window->AddObserver(vtkCommand::EndEvent, watch.callback);
    watch.errorTag = window->AddObserver(vtkCommand::ErrorEvent, watch.callback);
    window->Render();
    // EndEvent 只证明 VTK 绘制结束，不代表 GPU fence 或屏幕呈现。
    return watch.hasEnded && !watch.hasError;
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
#if defined(_WIN32)
    auto* nativeWindow = vtkWin32OpenGLRenderWindow::SafeDownCast(m_renderWindow);
    if (nativeWindow && nativeWindow->GetWindowId()) {
        if (title.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
        std::wstring wideTitle;
        if (!title.empty()) {
            const auto length = static_cast<int>(title.size());
            const auto wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                title.data(), length, nullptr, 0);
            if (wideLength == 0) return false;
            wideTitle.resize(static_cast<std::size_t>(wideLength));
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                title.data(), length, wideTitle.data(), wideLength) != wideLength) return false;
        }
        // VTK 9.4 的 WM_SETTEXT 分支把消息参数当作 UTF-8 再解码。
        // 直接交给 Unicode 默认处理器，避免 A/W 消息转换后被二次解码。
        nativeWindow->vtkWindow::SetWindowName(title.c_str());
        return DefWindowProcW(nativeWindow->GetWindowId(), WM_SETTEXT, 0,
            reinterpret_cast<LPARAM>(wideTitle.c_str())) != 0;
    }
#endif
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
