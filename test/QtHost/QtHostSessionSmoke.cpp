#include "Host/VtkAppHostSession.h"

#include <QApplication>
#include <QCoreApplication>
#include <QOpenGLWidget>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkObjectFactory.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

// 默认 true 路径只验证 Host 是否调用 Render；probe 隔离真实 OpenGL context 和平台 Timer。
class RenderProbeWindow final : public vtkGenericOpenGLRenderWindow {
public:
    static RenderProbeWindow* New();
    vtkTypeMacro(RenderProbeWindow, vtkGenericOpenGLRenderWindow);

    void Render() override
    {
        ++m_renderCount;
    }

    std::size_t GetRenderCount() const
    {
        return m_renderCount;
    }

protected:
    RenderProbeWindow() = default;
    ~RenderProbeWindow() override = default;

private:
    std::size_t m_renderCount{0};
};

vtkStandardNewMacro(RenderProbeWindow);

class RenderProbeInteractor final : public vtkRenderWindowInteractor {
public:
    static RenderProbeInteractor* New();
    vtkTypeMacro(RenderProbeInteractor, vtkRenderWindowInteractor);

    void Initialize() override
    {
        this->Initialized = 1;
        this->Enabled = 1;
    }

protected:
    RenderProbeInteractor() = default;
    ~RenderProbeInteractor() override = default;

    int InternalCreateTimer(
        int timerId, int, unsigned long) override
    {
        return timerId;
    }

    int InternalDestroyTimer(int) override
    {
        return 1;
    }
};

vtkStandardNewMacro(RenderProbeInteractor);

bool BuildDefaultRenderTest()
{
    auto renderWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto interactor = vtkSmartPointer<RenderProbeInteractor>::New();
    renderWindow->SetInteractor(interactor);
    interactor->SetRenderWindow(renderWindow);
    const std::size_t renderCount = renderWindow->GetRenderCount();

    HostRenderViewConfig view;
    view.id = "render-probe";
    view.role = HostRenderViewRole::Primary3D;
    view.renderWindow = renderWindow;

    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession session(std::move(config));
    session.BuildSession();
    return renderWindow->GetRenderCount() == renderCount;
}

class SessionFixture final {
public:
    bool BuildWindow()
    {
        if (m_widget || m_renderWindow) {
            return false;
        }

        m_widget = std::make_unique<QVTKOpenGLNativeWidget>();
        m_widget->setWindowTitle(QStringLiteral("MVVCVTK Qt Host Session Smoke"));
        m_renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
        m_renderCallback = vtkSmartPointer<vtkCallbackCommand>::New();
        m_renderCallback->SetClientData(this);
        m_renderCallback->SetCallback(&SessionFixture::OnRenderStart);
        m_renderWindow->AddObserver(vtkCommand::StartEvent, m_renderCallback);
        m_errorCallback = vtkSmartPointer<vtkCallbackCommand>::New();
        m_errorCallback->SetClientData(this);
        m_errorCallback->SetCallback(&SessionFixture::OnVtkError);
        m_renderWindow->AddObserver(vtkCommand::ErrorEvent, m_errorCallback);
        m_widget->setRenderWindow(m_renderWindow);
        m_widget->resize(640, 480);
        m_widget->show();
        return true;
    }

    bool StartHost()
    {
        if (!m_widget || !m_renderWindow || m_session) {
            return false;
        }

        HostRenderViewConfig view;
        view.id = "primary-3d";
        view.role = HostRenderViewRole::Primary3D;
        view.window.title = "Qt Host Session Smoke";
        view.window.width = 640;
        view.window.height = 480;
        view.window.viewInit.viewMode = HostRenderMode::CompositeIsoSurface;
        view.window.viewInit.background = { 0.08, 0.12, 0.16 };
        view.window.viewInit.hasBackground = true;
        view.renderWindow = m_renderWindow;

        HostSessionConfig config;
        config.renderViews.push_back(std::move(view));

        m_session = std::make_unique<VtkAppHostSession>(std::move(config));
        const std::size_t renderStartCount = m_renderStartCount;
        const bool isBuilt = m_session->BuildSession();
        HostTimerConfig timer;
        timer.isTimerEnabled = true;
        timer.targetView = { "primary-3d", false, HostRenderViewRole::Primary3D };
        const bool isTimerAttached = m_session->AttachTimer(timer);
        const bool hasInitialRender = m_renderStartCount != renderStartCount;

        const auto& endpoints = m_session->GetRenderViewEndpoints();
        const auto* endpoint = m_session->GetRenderViewEndpoint("primary-3d");
        const auto* primaryEndpoint = m_session->GetPrimaryEndpoint();
        if (m_vtkErrorCount > 0) {
            std::cerr
                << "FAIL: VTK ErrorEvent during BuildSession"
                << " count=" << m_vtkErrorCount;
            if (!m_vtkErrorText.empty()) {
                std::cerr << " message=" << m_vtkErrorText;
            }
            std::cerr << '\n';
        }
        return isBuilt && isTimerAttached && !hasInitialRender
            && m_vtkErrorCount == 0
            && endpoints.size() == 1
            && endpoint != nullptr
            && primaryEndpoint == endpoint
            && endpoint->role == HostRenderViewRole::Primary3D
            && endpoint->renderer != nullptr
            && endpoint->interactor != nullptr
            && endpoint->renderWindow == m_renderWindow.Get()
            && m_widget->renderWindow() == m_renderWindow.Get();
    }

    bool StopHost()
    {
        m_session.reset();
        if (m_widget) {
            m_widget->setRenderWindow(
                static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
        }
        m_renderWindow = nullptr;
        return true;
    }

    QVTKOpenGLNativeWidget* GetWidget() const
    {
        return m_widget.get();
    }

private:
    static void OnRenderStart(
        vtkObject*, unsigned long, void* clientData, void*)
    {
        auto* fixture = static_cast<SessionFixture*>(clientData);
        if (fixture) {
            ++fixture->m_renderStartCount;
        }
    }

    static void OnVtkError(
        vtkObject*, unsigned long, void* clientData, void* callData)
    {
        auto* fixture = static_cast<SessionFixture*>(clientData);
        if (!fixture) {
            return;
        }
        ++fixture->m_vtkErrorCount;
        if (callData) {
            fixture->m_vtkErrorText = static_cast<const char*>(callData);
        }
    }

    std::unique_ptr<QVTKOpenGLNativeWidget> m_widget;
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> m_renderWindow;
    vtkSmartPointer<vtkCallbackCommand> m_renderCallback;
    vtkSmartPointer<vtkCallbackCommand> m_errorCallback;
    std::unique_ptr<VtkAppHostSession> m_session;
    std::size_t m_renderStartCount{0};
    std::size_t m_vtkErrorCount{0};
    std::string m_vtkErrorText;
};

} // namespace

int main(int argc, char* argv[])
{
    // QVTK surface 格式必须早于 QApplication，和接入指南的构建链保持一致。
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);

    if (!BuildDefaultRenderTest()) {
        std::cerr << "FAIL: BuildSession unexpectedly rendered the Qt-owned window\n";
        return 5;
    }

    SessionFixture smoke;
    if (!smoke.BuildWindow()) {
        std::cerr << "FAIL: Qt Host window build\n";
        return 1;
    }

    const bool isInteractive =
        QCoreApplication::arguments().contains(QStringLiteral("--interactive"));
    int result{2};
    bool hasStarted{false};
    QObject::connect(smoke.GetWidget(), &QOpenGLWidget::frameSwapped, &app, [&]() {
        if (hasStarted) {
            return;
        }
        hasStarted = true;
        const bool isPassed = smoke.StartHost();
        result = isPassed ? 0 : 3;
        std::cout << (isPassed
            ? "PASS: Qt Host session endpoint binding\n"
            : "FAIL: Qt Host session endpoint binding\n");

        if (!isInteractive || !isPassed) {
            QTimer::singleShot(0, &app, [&]() {
                smoke.StopHost();
                QApplication::exit(result);
            });
        }
    });
    QTimer::singleShot(5000, &app, [&]() {
        if (!hasStarted) {
            std::cerr << "FAIL: QVTK first frame timeout\n";
            result = 4;
            smoke.StopHost();
            QApplication::exit(result);
        }
    });

    const int appResult = app.exec();
    smoke.StopHost();
    return appResult;
}
