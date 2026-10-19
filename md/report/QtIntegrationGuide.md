# Qt 宿主接入指南

Qt 宿主负责窗口、布局和事件循环，`VtkAppHostSession` 负责组装 Host。应用通过 `MVVCVTK/API/` 下的请求、结果、状态快照和端点（endpoint）接入，不取得 `AppRuntime`、`Context` 或内部 service；Qt 由宿主提供，不随 SDK（Software Development Kit，开发包）交付。

接入流程：创建 Qt 控件和渲染窗口 → 构建 Session → 加载数据 → 按需更新、绘制和交互 → 停止 Session → 释放窗口。

## 1. 配置工程

当前安装包要求 Windows x64、MSVC v145、C++17，Debug/Release 分别使用 `/MDd`、`/MD`。下面示例使用与仓内 Qt 宿主一致的 Qt 5.14.2、VTK 9.4.2。头、库和运行依赖应来自同一套包。

`MVVCVTK::Host` 是实现库，公开依赖接口目标 `MVVCVTK::HostAPI`；创建 Session 时链接前者即可。仅链接 `HostAPI` 不提供 Session 实现。

```cmake
cmake_minimum_required(VERSION 4.2)
project(QtViewer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

find_package(Qt5 5.14.2 EXACT CONFIG REQUIRED COMPONENTS Core Gui Widgets OpenGL)
find_package(MVVCVTK CONFIG REQUIRED COMPONENTS Host)
find_package(VTK 9.4.2 EXACT CONFIG REQUIRED COMPONENTS
    GUISupportQt RenderingOpenGL2 RenderingVolumeOpenGL2 InteractionStyle)

add_executable(QtViewer main.cpp)
target_compile_options(QtViewer PRIVATE /utf-8
    "/FI${CMAKE_CURRENT_SOURCE_DIR}/QtStlCompat.h")
target_link_libraries(QtViewer PRIVATE
    MVVCVTK::Host Qt5::Widgets Qt5::OpenGL VTK::GUISupportQt)
vtk_module_autoinit(TARGETS QtViewer MODULES
    VTK::RenderingOpenGL2 VTK::RenderingVolumeOpenGL2 VTK::InteractionStyle)
```

Qt 5.14 与 v145 配套时需要下面的 `QtStlCompat.h`，处理 Qt 对旧 MSVC 数组迭代器的引用。上面的 `/FI` 将它放在其他头文件之前。

```cpp
#pragma once
#include <QtCore/qcompilerdetection.h>
#if defined(_MSC_VER)
#undef QT_MAKE_UNCHECKED_ARRAY_ITERATOR
#undef QT_MAKE_CHECKED_ARRAY_ITERATOR
#define QT_MAKE_UNCHECKED_ARRAY_ITERATOR(value) (value)
#define QT_MAKE_CHECKED_ARRAY_ITERATOR(value, size) (value)
#endif
```

将开发包根目录和 Qt 目录填入 `CMAKE_PREFIX_PATH` 后构建：

```powershell
cmake -S . -B build -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_PREFIX_PATH=<SDK根目录>;<Qt目录>"
cmake --build build --config Debug
cmake --build build --config Release
```

运行时部署对应配置的 VTK/OpenCV 动态库，并用 Qt 的 `windeployqt` 部署 Qt 动态库和平台插件。Qt 的 VTK 适配模块来自同一套 VTK，不能另找一个版本替换。

## 2. 嵌入窗口并创建会话

下面第 2～6 节的 C++ 代码块组成 `main.cpp`。`Viewer` 和 `RenderWidget` 是示例中的 Qt 类；`LoadRaw()` 接到宿主已有的打开文件入口，几何参数由宿主填写。

示例建立一个三维视图和三个切片视图。每个控件绑定独立的 `vtkGenericOpenGLRenderWindow`，同一个窗口同时传给控件和 Session 配置。视图 ID 由应用命名，必须唯一，后续请求按 ID 定位。

```cpp
#include <MVVCVTK/API/Host/VtkAppHostSession.h>
#include <MVVCVTK/API/Interaction/InteractionController.h>
#include <MVVCVTK/API/Interaction/ViewNavigation.h>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkSmartPointer.h>
#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QEvent>
#include <QGridLayout>
#include <QLocale>
#include <QMainWindow>
#include <QMetaObject>
#include <QStatusBar>
#include <QString>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUuid>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

class RenderWidget final : public QVTKOpenGLNativeWidget {
public:
    using QVTKOpenGLNativeWidget::QVTKOpenGLNativeWidget;
protected:
    void initializeGL() override {
        QVTKOpenGLNativeWidget::initializeGL();
        auto* window = vtkGenericOpenGLRenderWindow::SafeDownCast(renderWindow());
        if (window && window->GetReadyForRendering()) {
            // 为 HostDriven 的首帧建立缓冲附件，不绘制场景。
            window->Start();
            window->End();
        }
    }
};

class Viewer final : public QMainWindow {
public:
    Viewer();
    ~Viewer() override { wake_->stopped.store(true); }
    void LoadRaw(const QString& path, const HostVolumeGeometry& geometry);
    void Send(HostRequest&& request, HostResultCallback done = {});
    VtkAppHostSession* Session() const {
        return built_ && !closing_ ? session_.get() : nullptr;
    }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
private:
    void Pump();
    struct View {
        std::string id;
        RenderWidget* widget;
        vtkSmartPointer<vtkGenericOpenGLRenderWindow> window;
    };
    struct Wake {
        std::atomic_bool queued{false}, stopped{false};
        Viewer* owner = nullptr; // 只在 Qt 线程读取。
    };
    std::vector<View> views_;
    std::unique_ptr<VtkAppHostSession> session_;
    std::set<std::string> pending_;
    std::shared_ptr<Wake> wake_ = std::make_shared<Wake>();
    std::function<void()> queueWork_;
    bool built_ = false, closing_ = false;
};

Viewer::Viewer() {
    wake_->owner = this;
    const std::weak_ptr<Wake> weak = wake_;
    queueWork_ = [weak] {
        const auto state = weak.lock();
        if (!state || state->stopped.load() || state->queued.exchange(true)) return;
        bool posted = false;
        try {
            posted = QMetaObject::invokeMethod(qApp, [weak] {
                const auto state = weak.lock();
                if (!state || state->stopped.load()) return;
                // 先清排队标记，本次更新中新产生的通知仍可再次排队。
                state->queued.store(false);
                state->owner->Pump();
            }, Qt::QueuedConnection);
        } catch (...) {}
        if (!posted) {
            state->queued.store(false);
            qWarning() << "Host work delivery failed";
        }
    };

    HostSessionConfig config;
    config.driveMode = HostDriveMode::HostDriven;
    config.onWorkAvailable = queueWork_;
    config.sendOwnerTask = [](std::function<void()> task) {
        return task && QMetaObject::invokeMethod(qApp, std::move(task), Qt::QueuedConnection);
    };
    config.sendDiagnostic = [](const std::string& message) {
        qWarning().noquote() << QString::fromStdString(message);
    };

    auto* panel = new QWidget(this);
    auto* grid = new QGridLayout(panel);
    setCentralWidget(panel);
    const char* ids[] = {"primary-3d", "slice-top-down", "slice-front-back", "slice-left-right"};
    const HostRenderViewRole roles[] = {HostRenderViewRole::Primary3D,
        HostRenderViewRole::TopDownSlice, HostRenderViewRole::FrontBackSlice,
        HostRenderViewRole::LeftRightSlice};
    const HostRenderMode modes[] = {HostRenderMode::CompositeIsoSurface,
        HostRenderMode::SliceTopDown, HostRenderMode::SliceFrontBack,
        HostRenderMode::SliceLeftRight};
    for (int i = 0; i < 4; ++i) {
        auto window = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
        window->SetAlphaBitPlanes(1);
        window->SetMultiSamples(0);
        auto* widget = new RenderWidget(panel);
        widget->setRenderWindow(window);
        widget->installEventFilter(this);
        grid->addWidget(widget, i / 2, i % 2);
        views_.push_back({ids[i], widget, window});

        HostRenderViewConfig view;
        view.id = ids[i];
        view.role = roles[i];
        view.window.viewInit.viewMode = modes[i];
        view.renderWindow = window;
        view.isEventLoopEnabled = false;
        config.renderViews.push_back(std::move(view));
    }
    installEventFilter(this);
    session_ = std::make_unique<VtkAppHostSession>(std::move(config));
    built_ = session_->BuildSession();
    if (!built_) {
        qWarning() << "BuildSession failed";
        QTimer::singleShot(0, this, [this] { close(); });
    }
}
```

在 Qt 界面线程创建、构建和停止 Session，这个线程就是会话所属线程（owner thread）。Host 请求、状态查询、导航及渲染也在该线程执行。耗时文件处理交给后台线程；后台线程通过 `sendOwnerTask` 回到界面线程，不直接写渲染对象或控件。

`onWorkAvailable` 可能从任意线程调用，甚至早于发送函数返回，因此只排队，不同步访问 Session。示例合并通知，消费通知时先清除排队标记，再调用 `Pump()`，让处理期间的新通知仍能排队。接收方活到 `Stop()` 成功；晚到的排队任务检查停止标记。

`RenderWidget::initializeGL()` 沿用当前 CT 宿主的缓冲初始化方式。这里的 `window->Start()/End()` 属于 VTK 窗口；Session 的 `Start()` 用于独立 VTK 事件循环，Qt 宿主使用 `app.exec()`。

## 3. 加载数据与处理结果

`SendRequest()` 返回的是接纳结果：`Rejected` 不回调，`Accepted` 后提供的完成回调在会话所属线程恰好执行一次。短命令可能在 `SendRequest()` 返回前完成，所以先设置界面等待状态，再发送请求。

```cpp
void Viewer::Send(HostRequest&& request, HostResultCallback done) {
    if (!Session()) {
        statusBar()->showMessage(QStringLiteral("会话未就绪或正在关闭"));
        return;
    }
    const auto admission = session_->SendRequest(std::move(request),
        [this, done = std::move(done)](HostResult result) {
            if (closing_) return;
            if (!result.isSucceeded) {
                statusBar()->showMessage(QString::fromStdString(result.message));
                return;
            }
            if (done) done(std::move(result));
        });
    if (admission.status != HostAdmissionStatus::Accepted)
        statusBar()->showMessage(QString::fromStdString(admission.message));
}

void Viewer::LoadRaw(const QString& path, const HostVolumeGeometry& geometry) {
    if (path.isEmpty()) return;
    HostLoadRequest request;
    request.filePath = path.toUtf8().toStdString();
    request.geometry = geometry;
    request.metadata.identity.datasetId = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toStdString();
    request.metadata.source.kind = ImageSourceKind::RawFile;
    request.metadata.source.uri = request.filePath;
    statusBar()->showMessage(QStringLiteral("正在加载数据"));
    Send(std::move(request), [this](HostResult) {
        const auto image = session_->GetImageDescriptor();
        statusBar()->showMessage(image ? QStringLiteral("数据已加载，等待绘制")
                                      : QStringLiteral("未取得主图像描述"));
    });
}
```

调用 `LoadRaw(path, geometry)` 前，从数据说明或宿主输入控件填写 `HostVolumeGeometry`：`dimensions` 是 X/Y/Z 体素数，`spacing` 是正的有限间距，单位 mm；`origin/direction` 描述输入的 LPS（Left-Posterior-Superior，左后上）物理空间，方向矩阵为 3×3 行主序。不能用缺省原点和方向代替未知几何。加载边界转换到内部 RAS（Right-Anterior-Superior，右前上），`GetImageDescriptor()` 返回转换后的几何。

当前 RAW 入口读取无文件头、单分量、小端 float32，X 最快，其次 Y/Z；文件大小必须等于 `Nx × Ny × Nz × 4` 字节。其他输入沿用同一请求协议：

| 输入或操作 | 公开请求 | 要填写的内容 |
| --- | --- | --- |
| TIFF 序列 | `HostLoadRequest` | `source.kind = ImageSourceKind::TiffSeries`，路径及对应几何 |
| 内存体素 | `HostReloadRequest` | `voxels` 为拥有存储的连续 `std::vector<float>`，长度等于三维尺寸乘积；`source.kind = ImageSourceKind::Memory` |
| 选择已发布图像 | `HostDataSelectRequest` | 精确 `dataRevision`，以及当前描述符中的 `bindingRevision` 填入 `expectedBindingRevision` |

`HostAdmissionStatus::Accepted` 不表示加载完成，加载回调成功也不表示 Qt 已呈现画面。继续处理工作通知并绘制；失败从接纳结果或完成结果读取 `errorCode/message`。回调保持简短，关闭或销毁操作排到后续 Qt 事件中执行。

## 4. 按需更新和绘制

每次工作通知先 `SendUpdates()`，把待处理变化提交到场景；再把返回的视图 ID 合并进待绘制集合，调用 `SendRender()`。后者不会隐式更新或派发业务完成回调。空 ID 集合不绘制，未知或重复 ID 会整批拒绝。

```cpp
void Viewer::Pump() {
    if (!Session()) return;
    const auto update = session_->SendUpdates();
    if (closing_ || update.status == HostUpdateStatus::Stopped) return;
    if (update.status == HostUpdateStatus::Failed)
        qWarning() << "SendUpdates failed";
    pending_.insert(update.renderViewIds.begin(), update.renderViewIds.end());

    HostRenderRequest request;
    for (const auto& view : views_)
        if (pending_.count(view.id) && view.widget->isVisible()
            && view.window->GetReadyForRendering())
            request.viewIds.push_back(view.id);
    if (request.viewIds.empty()) return;
    const auto result = session_->SendRender(request);
    if (result.status == HostRenderStatus::Failed)
        qWarning() << "SendRender failed";
    bool drew = false;
    for (const auto& view : result.views) {
        drew = drew || view.status == HostRenderStatus::Rendered;
        if (view.status == HostRenderStatus::Rendered || view.status == HostRenderStatus::Unchanged)
            pending_.erase(view.viewId);
        else if (view.status == HostRenderStatus::Failed)
            qWarning() << "SendRender failed:" << QString::fromStdString(view.viewId);
    }
    // 实际绘制后再推进一次更新，收口依赖绘制结束的资源和效果提交。
    if (drew) queueWork_();
}

bool Viewer::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show || event->type() == QEvent::Resize
        || event->type() == QEvent::WindowStateChange || event->type() == QEvent::Paint)
        queueWork_();
    return QMainWindow::eventFilter(watched, event);
}
```

只在 `Rendered/Unchanged` 时移除待绘制 ID。`Deferred` 保留需求，等待下一次 Host 工作通知或窗口事件再尝试；`Failed` 记录错误并保留需求，修复后再试。隐藏或尚未就绪的窗口保留需求，不持续重绘。`desiredUpdateRate` 是 VTK 时间预算率，不是 Qt 定时器周期。

## 5. 切换视图、交互和查询状态

### 显示设置

`HostViewSetRequest` 只改指定视图，未填写的可选字段保留当前值。下面两个函数可直接接到模式下拉框和窗宽窗位控件。

```cpp
void SetViewMode(Viewer& viewer, HostRenderMode mode) {
    HostViewSetRequest request;
    request.targetView = HostViewTarget{"primary-3d"};
    request.mode = mode;
    request.volumeQuality = HostVolumeQuality::Auto;
    request.background = HostBackgroundColor{0.08, 0.08, 0.08};
    viewer.Send(std::move(request));
}

void SetSliceWindowLevel(Viewer& viewer, double width, double center) {
    HostViewSetRequest request;
    request.targetView = HostViewTarget{"slice-top-down"};
    HostWindowLevelParams level;
    level.windowWidth = width;
    level.windowCenter = center;
    request.windowLevel = level;
    viewer.Send(std::move(request));
}
```

| 模式 | 画面 |
| --- | --- |
| `Volume` / `IsoSurface` | 体渲染 / 等值面 |
| `CompositeVolume` / `CompositeIsoSurface` | 体渲染或等值面，加三向参考平面 |
| `SliceTopDown` / `SliceFrontBack` / `SliceLeftRight` | 三个正交切片方向 |

同一请求还支持 `iso`、`material/materialPreset`、`opacity`、`volumeTransferFunction`、`isAxesVisible`、`visibility` 和 `ruler`。等值阈值、窗宽窗位和传输函数节点使用实际体素标量域；体渲染质量取 `Auto/Low/High/XHigh/Ultra`。这些显示设置不会改变业务计算或导出分辨率。

### 导航和工具

默认 `HostInputMode::NativeInteractor` 直接接收 QVTK 鼠标、键盘事件。工具按钮通过请求切换，缩放、平移、旋转、翻片等通过公开导航句柄调用。

```cpp
void SetTool(Viewer& viewer, HostToolMode mode) {
    HostToolSetRequest request;
    request.targetView = HostViewTarget{"primary-3d"};
    request.toolMode = mode; // Navigation 或 ModelTransform。
    viewer.Send(std::move(request));
}

void ZoomIn(Viewer& viewer) {
    auto* session = viewer.Session();
    if (!session) return;
    const auto controller = session->GetInteractionController();
    const auto navigation = controller ? controller->GetNavigation("primary-3d") : nullptr;
    if (navigation && navigation->ZoomBy(1.2) != NavigationStatus::Applied)
        qWarning() << "Zoom failed";
}
```

`ViewNavigation` 还提供 `PanByPixels(dx,dy)`、`RotateBy(yaw,pitch,roll)`、`ScrollSlices(count)`、`SetWindowLevel(width,center)` 和 `FitToView()`，均返回 `NavigationStatus`。相机复位可发送 `HostViewResetRequest`；按键绑定可通过 `InteractionController::ApplyBindings()` 调整。

需要宿主自行转发输入时，在构建前把对应视图设为 `HostInputMode::HostInjected`，再经 `GetInputEndpoint()->SendInput(event)` 注入。`HostInputEvent` 的位置是 VTK 左下角原点的显示像素坐标，Qt 坐标须处理原点和缩放差异。每个事件使用一种接入方式，避免重复处理。

### 状态与输出

```cpp
void InspectView(Viewer& viewer) {
    auto* session = viewer.Session();
    if (!session) return;
    const auto image = session->GetImageDescriptor();
    const auto scene = session->GetSceneViewState(HostViewTarget{"primary-3d"});
    if (!image || !scene || !scene->isAvailable || !scene->presentation) return;
    const bool sameData = scene->presentation->dataRevision == image->dataRevision;
    const bool caughtUp = scene->renderedEpoch >= scene->sceneEpoch;
    qInfo() << "Scene uses current image:" << sameData << "render caught up:" << caughtUp;
}
```

`GetRenderViewState()` 读取显示参数；`GetSceneViewState()` 和 `GetSceneViewStates()` 读取单个或全部视图的显示、相机、输入和场景提交状态；`GetStateSnapshot()` 汇总加载、场景和业务操作状态。它们都是值快照，查询不会驱动更新。`presentationRevision` 仅覆盖显示参数，不能当作全场景版本。

`Rendered` 和 `renderedEpoch` 表示 VTK 绘制或逻辑提交的覆盖情况，不承诺显示器呈现。加载后仍需确认 Qt 控件实际显示本次数据。

`GetRenderViewEndpoint(id)` 可取得该视图的渲染窗口、交互器和 renderer 观察指针，仅在 Session 拓扑不变且存活期间使用，不跨停止、重建或销毁缓存。场景修改和绘制沿用请求及 Host 驱动接口。

需要输出时，发送 `HostDataExportRequest`，填写 `outputPath` 目录、`sourceView` 和 `format`（`Raw/Ply/Stl/Obj`）；`HostSliceExportRequest` 则按指定切片视图向 `outputDir` 导出逐层 PNG，可带 `angleDeg`。两者都按接纳结果和完成回调处理，不把切片导出当作 Qt 控件截图。

## 6. 停止并销毁

先停止宿主继续发送业务请求，取消并收回宿主自己的后台任务，再在界面线程调用 `Stop()`。它会停止 Host 任务并卸载已挂载功能；返回 false 时保留 Session、功能对象和全部窗口，留到后续事件重试，不能提前析构。

```cpp
void Viewer::closeEvent(QCloseEvent* event) {
    closing_ = true;
    setEnabled(false);
    if (session_ && !session_->Stop()) {
        event->ignore();
        statusBar()->showMessage(QStringLiteral("正在停止会话"));
        QTimer::singleShot(50, this, [this] { close(); });
        return;
    }
    wake_->stopped.store(true);
    built_ = false;
    session_.reset();
    pending_.clear();
    for (auto& view : views_) {
        view.widget->setRenderWindow(static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
        view.window = nullptr;
    }
    event->accept();
}

int main(int argc, char** argv) {
    QLocale::setDefault(QLocale::c());
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    Viewer viewer;
    viewer.resize(1200, 800);
    viewer.show();
    return app.exec();
}
```

`QApplication` 比 Session 活得更久。应用退出入口也应先关闭 `Viewer`，等 `Stop()` 成功后再退出事件循环。示例未启动宿主后台任务；加入自己的任务后，先完成取消和回收，再进入这段关闭代码。停止迟迟未完成时，结合 `GetStopState()` 和 `sendDiagnostic` 排查，不把等待状态当作已停止。

## 接口与示例位置

公开头的安装路径由 `MVVCVTK/CMakeLists.txt` 映射；下表路径相对开发包的 `include/`。

| 用途 | 公开头 |
| --- | --- |
| 会话入口 | `MVVCVTK/API/Host/VtkAppHostSession.h` |
| 请求、结果、输入端点 | `MVVCVTK/API/Host/Types/HostRequestTypes.h` |
| 构建配置、更新/绘制结果、状态快照 | `MVVCVTK/API/Host/Types/HostSessionTypes.h` |
| 渲染参数、几何、视图选择 | `MVVCVTK/API/Host/Types/HostValueTypes.h`、`MVVCVTK/API/Host/Types/HostViewTypes.h` |
| 图像描述与值读取 | `MVVCVTK/API/Data/ImageReadTypes.h` |
| 导航与绑定 | `MVVCVTK/API/Interaction/InteractionController.h`、`ViewNavigation.h`、`NavigationTypes.h`、`InputBindings.h`（同目录） |

仓内窗口嵌入和按需驱动示例在 `test/QtHost/Manual/App/TestWindow.cpp`、`QtHostPump.cpp`；首帧缓冲初始化见 `CT/MVVCVTK/c_ui/viewport/ViewportGather.cpp`。这些是宿主参考代码，不是需要链接或交付的 SDK 组件。
