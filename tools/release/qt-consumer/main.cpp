#include "Host/VtkAppHostSession.h"

#include <QApplication>
#include <QCoreApplication>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkRendererCollection.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace {

HostSessionConfig GetConfig(
    const vtkSmartPointer<vtkGenericOpenGLRenderWindow>& renderWindow,
    std::string viewId)
{
    HostRenderViewConfig view;
    view.id = std::move(viewId);
    view.role = HostRenderViewRole::Primary3D;
    view.window.title = "MVVCVTK Qt clean-room";
    view.window.width = 480;
    view.window.height = 360;
    view.window.viewInit.viewMode = HostRenderMode::Volume;
    view.renderWindow = renderWindow;

    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    config.sendOwnerTask = [](std::function<void()> task) {
        auto* app = QCoreApplication::instance();
        if (!app || !task) {
            return false;
        }
        QTimer::singleShot(
            0,
            app,
            [ownerTask = std::move(task)]() mutable {
                ownerTask();
            });
        return true;
    };
    return config;
}

bool StartSessionCheck(
    const vtkSmartPointer<vtkGenericOpenGLRenderWindow>& renderWindow,
    const std::string& viewId,
    const int rendererBaseline)
{
    auto session = std::make_unique<VtkAppHostSession>(
        GetConfig(renderWindow, viewId));
    if (!session->BuildSession()) {
        return false;
    }
    const auto* endpoint = session->GetRenderViewEndpoint(viewId);
    const bool isInjected = endpoint
        && endpoint->renderWindow == renderWindow.GetPointer();
    const bool isStopped = session->Stop();
    session.reset();
    return isInjected
        && isStopped
        && renderWindow->GetRenderers()->GetNumberOfItems()
            == rendererBaseline;
}

}

int main(int argc, char* argv[])
{
    QSurfaceFormat::setDefaultFormat(
        QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);

    QVTKOpenGLNativeWidget widget;
    vtkNew<vtkGenericOpenGLRenderWindow> renderWindow;
    widget.setRenderWindow(renderWindow);
    widget.resize(480, 360);
    widget.show();

    const int rendererBaseline =
        renderWindow->GetRenderers()->GetNumberOfItems();
    int result = 1;
    QTimer::singleShot(0, &widget, [&]() {
        const bool isFirstValid = StartSessionCheck(
            renderWindow, "clean-room-first", rendererBaseline);
        const bool isReusable = isFirstValid && StartSessionCheck(
            renderWindow, "clean-room-second", rendererBaseline);
        result = isReusable ? 0 : 1;
        widget.setRenderWindow(
            static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
        QApplication::exit(result);
    });
    QTimer::singleShot(30000, &widget, [&]() {
        QApplication::exit(2);
    });
    return app.exec();
}
