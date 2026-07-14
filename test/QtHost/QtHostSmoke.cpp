#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <QtGlobal>

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkRenderer.h>

#include <iostream>

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
    std::size_t vtkErrorCount{0};
    vtkNew<vtkCallbackCommand> errorCallback;
    errorCallback->SetClientData(&vtkErrorCount);
    errorCallback->SetCallback(
        [](vtkObject*, unsigned long, void* clientData, void*) {
            auto* errorCount = static_cast<std::size_t*>(clientData);
            if (errorCount) {
                ++(*errorCount);
            }
        });
    renderWindow->AddObserver(vtkCommand::ErrorEvent, errorCallback);
    vtkNew<vtkRenderer> renderer;
    renderer->SetBackground(0.08, 0.12, 0.16);
    renderWindow->AddRenderer(renderer);
    widget.setRenderWindow(renderWindow);
    widget.resize(640, 480);
    widget.show();

    const bool isInteractive =
        QCoreApplication::arguments().contains(QStringLiteral("--interactive"));
    int result{1};
    QTimer::singleShot(0, &widget, [&]() {
        const bool isBound = widget.renderWindow() == renderWindow;
        const bool isQtVersion =
            QLatin1String(qVersion()) == QLatin1String("5.14.2");
        const bool isWindowsPlugin =
            QGuiApplication::platformName() == QStringLiteral("windows");
        if (isBound && isQtVersion && isWindowsPlugin) {
            renderWindow->Render();
            if (vtkErrorCount == 0) {
                result = 0;
                std::cout
                    << "PASS: Qt 5.14.2 QVTK render window binding\n";
            }
        }
        if (result != 0) {
            std::cerr
                << "FAIL: QVTK smoke prerequisites"
                << " bound=" << isBound
                << " qt=" << qVersion()
                << " platform="
                << QGuiApplication::platformName().toStdString()
                << " vtkErrors=" << vtkErrorCount
                << '\n';
        }

        if (!isInteractive || result != 0) {
            widget.setRenderWindow(
                static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
            QApplication::exit(result);
        }
    });

    const int appResult = app.exec();
    widget.setRenderWindow(
        static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
    return appResult;
}
