#include "App/Services/FeatureViewService.h"
#include "Host/HostFeature.h"
#include "Host/VtkAppHostSession.h"
#include "Host/Types/HostRequestTypes.h"
#include "Render/Contracts/OverlayService.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <QtGlobal>

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkRendererCollection.h>

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class CleanRoomFeature final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override
    {
        return "sdk.clean-room";
    }

    bool AttachHost(const HostFeatureContext& context) override
    {
        if (m_host || !context.views || !context.read
            || !context.data || !context.host) {
            return false;
        }
        const auto feature = context.views->GetFeaturePort(
            "clean-room-primary");
        const auto overlay = context.views->GetOverlayPort(
            "clean-room-primary");
        if (!feature || !overlay || !feature->SetRenderNeeded()) {
            return false;
        }
        if (!context.host->SetActiveViews({
                "clean-room-primary" })) {
            return false;
        }

        HostInputBinding binding;
        binding.featureId = std::string(GetFeatureId());
        binding.targetViews.viewIds = {
            "clean-room-primary" };
        binding.onInput = [](const InteractionEvent&) {
            return InteractionResult{};
        };
        if (!context.host->AttachInput(std::move(binding))) {
            (void)context.host->SetActiveViews({});
            return false;
        }

        // 空 overlay 验证窄契约已进入 staged closure，且不改变渲染树。
        overlay->RemoveOverlay({});
        m_views = context.views;
        m_read = context.read;
        m_data = context.data;
        m_host = context.host;
        m_feature = std::move(feature);
        m_overlay = std::move(overlay);
        return true;
    }

    bool DetachHost() override
    {
        if (!m_host) return true;
        const bool isInputDetached =
            m_host->DetachInput(GetFeatureId());
        const bool areViewsCleared =
            m_host->SetActiveViews({});
        if (!isInputDetached || !areViewsCleared) {
            return false;
        }
        m_overlay.reset();
        m_feature.reset();
        m_host.reset();
        m_data.reset();
        m_read.reset();
        m_views.reset();
        return true;
    }

    bool OnHostTick() override
    {
        return true;
    }

    bool GetPortsValid() const
    {
        if (!m_views || !m_read || !m_data || !m_host
            || !m_feature || !m_overlay
            || !m_data->GetImageSnapshot()) {
            return false;
        }
        ImageReadRequest request;
        const auto image = m_read->GetImageReadResult(request);
        const auto views = m_views->GetViews(HostViewTargets{
            { "clean-room-primary" }, {}
        });
        return image.error == ImageReadError::None
            && image.requiredBytes == 512 * sizeof(float)
            && image.state
            && image.state->dims
                == std::array<int, 3>{ 8, 8, 8 }
            && image.state->values
            && image.state->values->size()
                == 512 * sizeof(float)
            && views.size() == 1
            && views.front().id == "clean-room-primary";
    }

private:
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<ImageReadPort> m_read;
    std::shared_ptr<TrustedFeatureDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::shared_ptr<FeatureViewService> m_feature;
    std::shared_ptr<OverlayService> m_overlay;
};

}

int main(int argc, char* argv[])
{
    static_assert(
        QT_VERSION == QT_VERSION_CHECK(5, 14, 2),
        "QtHostSmoke requires Qt 5.14.2 headers");

    // QVTKOpenGLNativeWidget 在 QApplication 创建前确定 OpenGL surface 格式。
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);

    QVTKOpenGLNativeWidget widget;
    widget.setWindowTitle(QStringLiteral("MVVCVTK Qt/VTK Smoke"));
    vtkNew<vtkGenericOpenGLRenderWindow> renderWindow;
    struct VtkErrorState final {
        std::size_t count = 0;
        std::string text;
    };
    VtkErrorState vtkError;
    vtkNew<vtkCallbackCommand> errorCallback;
    errorCallback->SetClientData(&vtkError);
    errorCallback->SetCallback(
        [](vtkObject*, unsigned long, void* clientData, void* callData) {
            auto* errorState = static_cast<VtkErrorState*>(clientData);
            if (!errorState) return;
            ++errorState->count;
            const auto* message = static_cast<const char*>(callData);
            if (message) errorState->text = message;
        });
    renderWindow->AddObserver(vtkCommand::ErrorEvent, errorCallback);
    widget.setRenderWindow(renderWindow);
    widget.resize(640, 480);
    widget.show();

    const int rendererBaseline =
        renderWindow->GetRenderers()->GetNumberOfItems();
    std::unique_ptr<VtkAppHostSession> session;
    std::shared_ptr<CleanRoomFeature> feature;
    bool isFinished = false;
    int result = 1;

    const auto getConfig = [&renderWindow](const std::string& viewId) {
        HostRenderViewConfig view;
        view.id = viewId;
        view.role = HostRenderViewRole::Primary3D;
        view.window.title = "MVVCVTK SDK clean-room";
        view.window.width = 640;
        view.window.height = 480;
        view.window.viewInit.viewMode = HostRenderMode::Volume;
        view.window.viewInit.background = { 0.08, 0.12, 0.16 };
        view.window.viewInit.hasBackground = true;
        view.renderWindow = renderWindow;

        HostSessionConfig config;
        config.renderViews.push_back(std::move(view));
        config.sendOwnerTask = [](std::function<void()> task) {
            auto* appInstance = QCoreApplication::instance();
            if (!appInstance || !task) return false;
            QTimer::singleShot(
                0,
                appInstance,
                [ownerTask = std::move(task)]() mutable {
                    ownerTask();
                });
            return true;
        };
        return config;
    };

    std::function<void(bool, const char*)> sendExit;
    sendExit = [&](bool isSucceeded, const char* reason) {
        if (isFinished) return;
        isFinished = true;

        const bool isStopped = !session || session->Stop();
        session.reset();
        feature.reset();
        const bool isFirstDetached =
            renderWindow->GetRenderers()->GetNumberOfItems()
                == rendererBaseline;

        bool isWindowReusable = false;
        if (isSucceeded && isStopped && isFirstDetached) {
            auto second = std::make_unique<VtkAppHostSession>(
                getConfig("clean-room-second"));
            const bool isSecondBuilt = second->BuildSession();
            const bool isSecondStopped = second->Stop();
            second.reset();
            isWindowReusable = isSecondBuilt
                && isSecondStopped
                && renderWindow->GetRenderers()->GetNumberOfItems()
                    == rendererBaseline;
        }

        const bool isValid = isSucceeded
            && isStopped
            && isFirstDetached
            && isWindowReusable
            && vtkError.count == 0;
        result = isValid ? 0 : 1;
        if (isValid) {
            std::cout
                << "PASS: SDK Feature ports/read/input plus Session lifecycle\n";
        } else {
            std::cerr
                << "FAIL: SDK clean-room smoke"
                << " reason=" << reason
                << " stopped=" << isStopped
                << " detached=" << isFirstDetached
                << " reusable=" << isWindowReusable
                << " vtkErrors=" << vtkError.count
                << " vtkMessage=" << vtkError.text
                << '\n';
        }
        widget.setRenderWindow(
            static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
        QApplication::exit(result);
    };

    QTimer::singleShot(0, &widget, [&]() {
        const bool isBound = widget.renderWindow() == renderWindow;
        const bool isQtVersion =
            QLatin1String(qVersion()) == QLatin1String("5.14.2");
        const bool isWindowsPlugin =
            QGuiApplication::platformName() == QStringLiteral("windows");
        if (!isBound || !isQtVersion || !isWindowsPlugin) {
            sendExit(false, "Qt/VTK binding prerequisite");
            return;
        }

        session = std::make_unique<VtkAppHostSession>(
            getConfig("clean-room-primary"));
        if (!session->BuildSession()) {
            sendExit(false, "BuildSession");
            return;
        }
        feature = std::make_shared<CleanRoomFeature>();
        if (!session->AttachFeature(feature)) {
            sendExit(false, "AttachFeature");
            return;
        }

        HostTimerConfig timer;
        timer.isTimerEnabled = true;
        timer.targetView.viewId = "clean-room-primary";
        if (!session->AttachTimer(timer)) {
            sendExit(false, "AttachTimer");
            return;
        }
        widget.makeCurrent();
        if (auto* context = QOpenGLContext::currentContext()) {
            auto* functions = context->functions();
            while (functions
                && functions->glGetError() != GL_NO_ERROR) {
            }
        }
        const bool isStarted = session->Start();
        widget.doneCurrent();
        if (!isStarted) {
            sendExit(false, "Start");
            return;
        }

        HostReloadRequest reload;
        constexpr int sideLength = 8;
        reload.geometry.dimensions = {
            sideLength, sideLength, sideLength
        };
        reload.geometry.spacing = { 1.0f, 1.0f, 1.0f };
        reload.geometry.origin = { 0.0f, 0.0f, 0.0f };
        reload.voxels.resize(
            sideLength * sideLength * sideLength);
        for (std::size_t index = 0;
            index < reload.voxels.size();
            ++index) {
            reload.voxels[index] = static_cast<float>(index);
        }

        const bool isAccepted = session->SendRequestResult(
            std::move(reload),
            [&](HostResult result) {
                const bool isLoaded = result.isSucceeded
                    && result.errorCode == HostErrorCode::None;
                QTimer::singleShot(0, &widget, [&, isLoaded]() {
                    if (!isLoaded || !session) {
                        sendExit(false, "reload");
                        return;
                    }

                    HostViewSetRequest viewRequest;
                    viewRequest.targetView.viewId =
                        "clean-room-primary";
                    viewRequest.mode = HostRenderMode::Volume;
                    viewRequest.opacity = 0.7;
                    viewRequest.isAxesVisible = true;
                    auto viewResultCount = std::make_shared<int>(0);
                    const bool isViewAccepted =
                        session->SendRequestResult(
                            std::move(viewRequest),
                            [&, viewResultCount](HostResult viewResult) {
                                ++(*viewResultCount);
                                QTimer::singleShot(
                                    0,
                                    &widget,
                                    [&, viewResultCount,
                                        viewResult = std::move(viewResult)]() {
                                        if (*viewResultCount != 1
                                            || !viewResult.isSucceeded
                                            || viewResult.errorCode
                                                != HostErrorCode::None) {
                                            sendExit(false, "view command");
                                            return;
                                        }

                                        const auto state =
                                            session->GetRenderViewState(
                                                HostViewTarget{
                                                    "clean-room-primary",
                                                    false,
                                                    HostRenderViewRole::Primary3D
                                                });
                                        const auto* endpoint =
                                            session->GetPrimaryEndpoint();
                                        const bool hasCommittedData = state
                                            && state->scalarRange[0] == 0.0
                                            && state->scalarRange[1] == 511.0;
                                        if (!endpoint
                                            || !endpoint->renderWindow
                                            || !hasCommittedData
                                            || !feature
                                            || !feature->GetPortsValid()) {
                                            sendExit(false, "committed state");
                                            return;
                                        }
                                        sendExit(true, "ok");
                                    });
                            });
                    if (!isViewAccepted) {
                        sendExit(false, "view command admission");
                    }
                });
            });
        if (!isAccepted) sendExit(false, "reload admission");
    });

    QTimer::singleShot(
        30000,
        &widget,
        [&]() { sendExit(false, "timeout"); });

    const int appResult = app.exec();
    widget.setRenderWindow(
        static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
    return appResult;
}
