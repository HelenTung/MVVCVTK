# Qt 上位机 API 接入文档

本文按“建立四视图 → 加载真实文件 → 调用业务 → 读取结果 → 关闭”接入。代码使用 `MVVCVTK/API/` 入口，适用于 Windows x64、MSVC v145、Qt 5.14.2 Widgets；SDK 使用 C++17，Debug 使用 `/MDd`，Release 使用 `/MD`。Release 运行 CPU 需支持 AVX2。

## 1. 先建立这些对象

| 对象 | 谁创建、谁管理 | 组合关系与用途 |
| --- | --- | --- |
| `QApplication` | 上位机 `main()` 创建，最后销毁 | 提供 GUI 线程和事件循环；GUI 线程也是本 Session 的 owner thread |
| `CtWindow` | 上位机创建 | 示例的上位机窗口，组合下面的视图、Session、业务句柄和调度器 |
| 4 个 `CtView` | `CtWindow` 创建，Qt parent 管理 | 每个控件显示一个视图；继承 `QVTKOpenGLNativeWidget` |
| 4 个 `vtkGenericOpenGLRenderWindow` | 上位机用 `vtkSmartPointer` 创建 | 与 4 个控件一一绑定；同一实例传入 Session 配置；必须保留到 `Stop()` 成功 |
| 1 个 `VtkAppHostSession` | 上位机用 `unique_ptr` 持有 | 组合数据、视图和已挂载业务；上位机通过它加载、更新、绘制、查询和停止 |
| 1 个 `CropHostFeature` | 上位机用 `shared_ptr` 创建，调用 `AttachFeature` | 管理裁切文档、历史、交互预览和构建结果；Session 持有已挂载句柄 |
| 1 个 `GapHostFeature` | 上位机用 `shared_ptr` 创建，调用 `AttachFeature` | 管理孔隙分析、统计、结果显示和退出；Session 持有已挂载句柄 |
| `InteractionController` / `ViewNavigation` | 从 Session 获取 | 管理输入绑定、动作和单视图导航；使用会话返回的句柄 |
| 请求、结果、状态、`DataRevisionRef` | 上位机保存值副本 | 请求描述动作；结果描述终态；状态用于查询。修订标识不是体素、表格或网格本身 |
| `QTimer` | 上位机创建和停止 | 示例每 33 ms 在 GUI 线程执行更新与按需绘制；停止成功后再停 timer、释放对象 |

固定使用以下视图 ID，后续请求使用同一组 ID：

| ID | role | 初始 mode |
| --- | --- | --- |
| `primary-3d` | `Primary3D` | `CompositeIsoSurface` |
| `slice-top-down` | `TopDownSlice` | `SliceTopDown` |
| `slice-front-back` | `FrontBackSlice` | `SliceFrontBack` |
| `slice-left-right` | `LeftRightSlice` | `SliceLeftRight` |

单视图用 `HostViewTarget{"primary-3d"}`；多视图用 `HostViewTargets::viewIds`。ID 必须唯一且存在；空目标集合不会自动选择全部视图。

## 2. 配置工程并部署 DLL

准备 SDK 解压目录和 Qt 安装目录。`SDK_ROOT` 是直接包含 `include`、`lib`、`deps` 的目录；头文件、Debug/Release 库与 runtime 使用同一份 SDK。更新 SDK 时整体替换本次使用的包，并重新编译上位机。

新建目录，放入本节的 `CMakeLists.txt`、`QtStlCompat.h` 和第 3 节的 `main.cpp`。

### 2.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.24)
project(CtApiExample LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
find_package(Qt5 5.14.2 REQUIRED COMPONENTS Core Gui Widgets OpenGL)
find_package(MVVCVTK CONFIG REQUIRED COMPONENTS Host OrthogonalCrop GapAnalysis)
find_package(VTK 9.4.2 EXACT REQUIRED COMPONENTS GUISupportQt RenderingOpenGL2 RenderingVolumeOpenGL2 InteractionStyle)
add_executable(CtApiExample WIN32 main.cpp)
target_compile_options(CtApiExample PRIVATE /utf-8 /FI"${CMAKE_CURRENT_SOURCE_DIR}/QtStlCompat.h")
target_link_libraries(CtApiExample PRIVATE Qt5::Widgets Qt5::OpenGL VTK::GUISupportQt
    MVVCVTK::Host MVVCVTK::OrthogonalCrop MVVCVTK::GapAnalysis)
vtk_module_autoinit(TARGETS CtApiExample MODULES VTK::RenderingOpenGL2 VTK::RenderingVolumeOpenGL2 VTK::InteractionStyle)
```

### 2.2 QtStlCompat.h

Qt 5.14 与 v145 配套编译时，将此文件强制包含到每个翻译单元；上面的 CMake 已设置 `/FI`。

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

### 2.3 编译和复制运行依赖

在 VS 2026 Developer PowerShell 中进入示例目录。修改前两行路径，然后执行：

```powershell
$SDK_ROOT = 'F:\SDK\MVVCVTK'
$QT_ROOT = 'D:\qt\5.14.2\msvc2017_64'
cmake -S . -B build -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_PREFIX_PATH=$SDK_ROOT;$QT_ROOT"
cmake --build build --config Debug
cmake --build build --config Release
```

成功：生成 `build/Debug/CtApiExample.exe` 和 `build/Release/CtApiExample.exe`。失败：根据 CMake/MSBuild 错误修正 SDK 根、v145、Qt 或缺失依赖；编译失败时不进入运行步骤。

分别对两种配置执行下面的部署命令：

```powershell
foreach ($config in @('Debug', 'Release')) {
    $run = Join-Path $PWD "build/$config"
    foreach ($dll in Get-ChildItem "$SDK_ROOT/deps/official/vtk/bin/vtk*.dll") {
        if (($dll.BaseName.EndsWith('d')) -eq ($config -eq 'Debug')) {
            Copy-Item -LiteralPath $dll.FullName -Destination $run
        }
    }
    foreach ($dll in Get-ChildItem "$SDK_ROOT/deps/official/opencv/x64/vc16/bin/opencv_*.dll") {
        if (($dll.BaseName.EndsWith('d')) -eq ($config -eq 'Debug')) {
            Copy-Item -LiteralPath $dll.FullName -Destination $run
        }
    }
    Copy-Item -LiteralPath "$SDK_ROOT/deps/third_party/defx/bin/$config/GapKernel.dll" -Destination $run
    Copy-Item -LiteralPath "$SDK_ROOT/deps/third_party/defx/bin/$config/DefXAnalysis.dll" -Destination $run
    & "$QT_ROOT/bin/windeployqt.exe" "--$($config.ToLowerInvariant())" "$run/CtApiExample.exe"
}
```

成功：对应配置的 VTK/OpenCV DLL、`GapKernel.dll`、`DefXAnalysis.dll` 及 Qt platform plugin 位于程序目录。Debug/Release 分别部署，不能混用；上述两个 Gap runtime 文件在两种配置下同名，按来源目录区分。失败：文件不存在或部署命令报错时先补齐依赖。若机器设置了 `MVVCVTK_GAP_RUNTIME_DIR`，它必须指向相同配置的有效 Gap runtime 目录；它会优先于 exe 同目录查找，目录无效时不会自动改用 exe 同目录。

## 3. main.cpp：建立 Session、四视图和关闭流程

这份程序包含以下 8 个动作：设置 Qt 数值格式和 OpenGL 格式、创建四视图、`BuildSession`、挂载 Crop/Gap、启动 GUI 调度、发送加载请求、更新/绘制、停止并释放。

按以下顺序初始化：

| 顺序 | 上位机执行的动作 | 检查结果 |
| --- | --- | --- |
| 1 | 在创建 `QApplication` 和数值控件前，调用 `QLocale::setDefault(QLocale::c())`、设置 `QVTKOpenGLNativeWidget::defaultFormat()` | 尺寸、间距、ISO、均值控件使用 `0–9` 和小数点；已有窗口显式设置 locale 时也使用 C locale |
| 2 | 每个 `CtView` 绑定一个独立 render window；先设置 AlphaBitPlanes=1、MultiSamples=0，再 `setRenderWindow()`；把同一 window 传给 Session | 四个控件与四个视图 ID 一一对应 |
| 3 | 保留下面的 `CtView::initializeGL()`：先调用基类，再在 ReadyForRendering 时执行 window 的 `Start()/End()` | Qt 上下文建立后完成帧缓冲初始化；四个控件均使用此派生类 |
| 4 | `BuildSession`、挂载业务、启动 GUI pump，进入 `app.exec()` | 每次先 `SendUpdates()`，再按需 `SendRender()`；场景绘制统一走该流程 |

这里的 `window->Start()/End()` 是 render window 初始化。Qt 宿主不调用 Session 的 `Start()`；也不在按钮或 timer 中直接调用 render window 的 `Render()`。

```cpp
#include <MVVCVTK/API/Host/VtkAppHostSession.h>
#include <MVVCVTK/API/Features/OrthogonalCrop/Host/CropHostFeature.h>
#include <MVVCVTK/API/Features/GapAnalysis/Host/GapHostFeature.h>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QGridLayout>
#include <QLocale>
#include <QMainWindow>
#include <QPointer>
#include <QSurfaceFormat>
#include <QStatusBar>
#include <QTimer>
#include <array>
#include <cmath>
#include <memory>
#include <set>

class CtView final : public QVTKOpenGLNativeWidget {
public:
    using QVTKOpenGLNativeWidget::QVTKOpenGLNativeWidget;
protected:
    void initializeGL() override {
        QVTKOpenGLNativeWidget::initializeGL();
        auto* window = vtkGenericOpenGLRenderWindow::SafeDownCast(renderWindow());
        if (window && window->GetReadyForRendering()) {
            window->Start();
            window->End();
        }
    }
};

class CtWindow final : public QMainWindow {
public:
    // 上位机持有的业务值状态，跨按钮回调保存。
    std::shared_ptr<CropDocumentOutcome> cropDocumentState = std::make_shared<CropDocumentOutcome>();
    std::shared_ptr<CropBuildResult> cropBuildState = std::make_shared<CropBuildResult>();
    std::shared_ptr<GapHostResult> gapResultState = std::make_shared<GapHostResult>();
    CtWindow() {
        auto* panel = new QWidget(this);
        auto* grid = new QGridLayout(panel);
        setCentralWidget(panel);
        HostSessionConfig config;
        config.driveMode = HostDriveMode::HostDriven;
        config.sendOwnerTask = [](std::function<void()> task) {
            return task && QMetaObject::invokeMethod(qApp, std::move(task), Qt::QueuedConnection);
        };
        config.sendDiagnostic = [](const std::string& message) {
            qWarning().noquote() << QString::fromStdString(message);
        };
        const HostRenderViewRole roles[] = {HostRenderViewRole::Primary3D,
            HostRenderViewRole::TopDownSlice, HostRenderViewRole::FrontBackSlice,
            HostRenderViewRole::LeftRightSlice};
        const HostRenderMode modes[] = {HostRenderMode::CompositeIsoSurface,
            HostRenderMode::SliceTopDown, HostRenderMode::SliceFrontBack,
            HostRenderMode::SliceLeftRight};
        for (int i = 0; i < 4; ++i) {
            widgets_[i] = new CtView(panel);
            windows_[i] = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
            windows_[i]->SetAlphaBitPlanes(1);
            windows_[i]->SetMultiSamples(0);
            widgets_[i]->setRenderWindow(windows_[i]);
            grid->addWidget(widgets_[i], i / 2, i % 2);
            HostRenderViewConfig view;
            view.id = ids_[i];
            view.role = roles[i];
            view.window.viewInit.viewMode = modes[i];
            view.renderWindow = windows_[i];
            view.isEventLoopEnabled = false;
            config.renderViews.push_back(std::move(view));
        }
        session_ = std::make_unique<VtkAppHostSession>(std::move(config));
        const bool built = session_->BuildSession();
        crop_ = std::make_shared<CropHostFeature>();
        GapHostConfig gapConfig;
        gapConfig.inputViews = targets();
        gapConfig.defaultStart.targetViews = targets();
        gap_ = std::make_shared<GapHostFeature>(std::move(gapConfig));
        if (!built || !session_->AttachFeature(crop_) || !session_->AttachFeature(gap_)) {
            qCritical() << "Session creation or Feature attachment failed";
            closing_ = true;
        }
        connect(&timer_, &QTimer::timeout, this, [this] { pump(); });
        timer_.start(33);
        resize(1200, 800);
    }
    HostViewTargets targets() const { return {{ids_.begin(), ids_.end()}, {}}; }
    VtkAppHostSession& session() { return *session_; }
    CropHostFeature& crop() { return *crop_; }
    GapHostFeature& gap() { return *gap_; }
protected:
    void closeEvent(QCloseEvent* event) override {
        if (!session_) { event->accept(); return; }
        closing_ = true;
        event->ignore();
    }
private:
    void pump() {
        if (closing_) {
            if (!session_->Stop()) return;
            timer_.stop();
            gap_.reset(); crop_.reset(); session_.reset();
            for (int i = 0; i < 4; ++i) {
                widgets_[i]->setRenderWindow(static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
                windows_[i] = nullptr;
            }
            close();
            return;
        }
        const auto update = session_->SendUpdates();
        if (update.status == HostUpdateStatus::Failed) qWarning() << "SendUpdates failed";
        pending_.insert(update.renderViewIds.begin(), update.renderViewIds.end());
        HostRenderRequest request;
        for (int i = 0; i < 4; ++i)
            if (pending_.count(ids_[i]) && widgets_[i]->isVisible()
                && windows_[i]->GetReadyForRendering()) request.viewIds.push_back(ids_[i]);
        if (request.viewIds.empty()) return;
        const auto result = session_->SendRender(request);
        for (const auto& view : result.views) {
            if (view.status == HostRenderStatus::Rendered || view.status == HostRenderStatus::Unchanged)
                pending_.erase(view.viewId);
            else if (view.status == HostRenderStatus::Failed)
                qWarning() << "SendRender failed:" << QString::fromStdString(view.viewId);
        }
    }
    const std::array<std::string, 4> ids_{"primary-3d", "slice-top-down", "slice-front-back", "slice-left-right"};
    std::array<CtView*, 4> widgets_{};
    std::array<vtkSmartPointer<vtkGenericOpenGLRenderWindow>, 4> windows_;
    std::unique_ptr<VtkAppHostSession> session_;
    std::shared_ptr<CropHostFeature> crop_;
    std::shared_ptr<GapHostFeature> gap_;
    QTimer timer_;
    std::set<std::string> pending_;
    bool closing_ = false;
};

CropHostTarget MakeCropTarget(CtWindow& window) {
    CropHostTarget target;
    target.inputBinding = std::string(primaryVolumeBinding);
    target.referenceView = HostViewTarget{"primary-3d"};
    target.targetViews = window.targets();
    return target;
}

HostAdmission LoadVolume(CtWindow& window, const QString& path,
                         const HostVolumeGeometry& geometry) {
    HostLoadRequest request;
    request.filePath = path.toUtf8().toStdString();
    request.geometry = geometry;
    request.metadata.identity.datasetId = "inspection-001";
    request.metadata.source.kind = ImageSourceKind::RawFile;
    request.metadata.source.uri = request.filePath;
    QPointer<CtWindow> owner(&window);
    window.statusBar()->showMessage(QStringLiteral("正在加载数据……"));
    return window.session().SendRequest(std::move(request), [owner](HostResult result) {
        if (!owner) return;
        if (!result.isSucceeded) {
            owner->statusBar()->showMessage(QString::fromStdString(result.message));
            return;
        }
        const auto image = owner->session().GetImageDescriptor();
        owner->statusBar()->showMessage(image ? QStringLiteral("加载完成") : QStringLiteral("未取得图像描述"));
    });
}

int main(int argc, char** argv) {
    QLocale::setDefault(QLocale::c());
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    // 参数：文件路径 Nx Ny Nz Sx Sy Sz；spacing 单位 mm，origin 缺省为 LPS (0,0,0)。
    if (argc != 8) { qCritical() << "Usage: CtApiExample file.raw Nx Ny Nz Sx Sy Sz"; return 2; }
    HostVolumeGeometry geometry;
    for (int i = 0; i < 3; ++i) {
        bool valid = false;
        geometry.dimensions[i] = QString::fromLocal8Bit(argv[i + 2]).toInt(&valid);
        if (!valid || geometry.dimensions[i] <= 0) return 2;
        geometry.spacing[i] = QString::fromLocal8Bit(argv[i + 5]).toDouble(&valid);
        if (!valid || !std::isfinite(geometry.spacing[i]) || geometry.spacing[i] <= 0) return 2;
    }
    CtWindow window;
    window.show();
    const QString path = app.arguments()[1];
    QTimer::singleShot(0, &window, [&window, path, geometry] {
        const auto admission = LoadVolume(window, path, geometry);
        if (admission.status != HostAdmissionStatus::Accepted)
            window.statusBar()->showMessage(QString::fromStdString(admission.message));
    });
    return app.exec();
}
```

### 3.1 初始化成功和失败分别是什么

| 调用 | 输入 | 成功返回 | 失败返回 | 预期输出 / 下一步 |
| --- | --- | --- | --- | --- |
| `BuildSession()` | 含 4 个视图的 `HostSessionConfig` | `true` | `false` | 成功后才能挂载业务、发送请求 |
| `AttachFeature(crop/gap)` | 已创建的 `shared_ptr` | `true` | `false` | Session 管理已挂载业务；失败时进入 Stop 收口 |
| `SendUpdates()` | 无参数，GUI 线程调用 | `status=Completed`，附 `renderViewIds` | `Deferred/Failed/Stopped` | 收集待画 ID；`Deferred` 留到下一次 GUI 调度 |
| `SendRender(request)` | 非空、无重复的有效 `viewIds` | 每视图 `Rendered/Unchanged` | 每视图 `Deferred/Failed/Stopped` | 仅移除已完成的待画项；失败项保留并报告 |
| `Stop()` | GUI 线程调用同一 Session | `true`，`GetIsStopped()==true` | `false` | 保留全部对象并在后续事件循环重试；查询 `GetStopState()`，若有 `sendDiagnostic` 消息则一并记录；成功才释放 Feature、Session、window 和 widget |

`HostRenderRequest::desiredUpdateRate` 保持缺省值即可；33 ms 是宿主 timer 周期，两者不是同一个参数。所有 Host/Feature 请求、查询和窗口写入都在 GUI/owner thread。

### 3.2 按这个顺序更新界面状态

以下规则适用于 Host 和 Gap 的 admission 请求；Crop 各请求的返回类型见第 6 节。

1. **发送前**设置“正在加载/分析/退出”，禁用重复提交按钮，并准备好回调要保存的状态。
2. **同步拒绝**时结束本次 UI 等待，显示拒绝原因；不会再收到完成回调。
3. **Accepted** 对已提供的回调恰好兑现一次；继续 Qt 事件循环和 GUI pump。短请求可能在发送函数返回前已完成，返回后不能无条件再写“正在处理”。
4. **完成回调**先保存结果、刷新按钮，再显示终态消息。访问窗口使用 `QPointer`。定时刷新可以更新运行状态和按钮，不得覆盖已经收到的终态消息。

关闭整个窗口时直接进入第 3 节的 `Stop()` 流程；`Stop()==false` 期间继续事件循环，保留 Session、Feature、视图和 render window，直到返回 `true`。

## 4. 加载真实 RAW 文件

运行程序并传入文件路径、X/Y/Z 尺寸、X/Y/Z 间距，间距单位为 mm：

```powershell
& ./build/Release/CtApiExample.exe 'F:/data/ct/1000X1000X1000.raw' 1000 1000 1000 0.1537 0.1537 0.1537
```

示例输入为无文件头、float32、小端、X 最快的 RAW；尺寸乘积乘 4 必须等于文件字节数。程序缺省原点为 LPS `(0,0,0)`、方向为单位阵；有真实原点/方向时填写 `geometry.origin/direction`，方向矩阵按 3×3 行主序。

加载对话框按文件的实际元信息填写这些值。间距控件的小数位必须能保留输入精度，例如 `0.0389` 不能被控件舍入为 `0.039` 后再提交；体积统计使用实际提交的几何。首次接入先检查尺寸、间距、原点及孔隙参数的数字显示和读回值，再加载文件。

| 阶段 | 成功 | 失败 | 预期输出 |
| --- | --- | --- | --- |
| 同步发送 `HostLoadRequest` | `HostAdmission.status == Accepted` | `Rejected`；读取 `errorCode/message`，结束 UI 等待 | Accepted 只表示请求已接纳 |
| 完成回调 | `HostResult.isSucceeded == true` | `false`；读取 `errorCode/message` | 成功后取得图像描述，状态栏显示“加载完成” |
| `GetImageDescriptor()` | 返回有值的 `optional<ImageDescriptor>` | 空 optional | `dims`、`spacing`、`origin`、`direction`、`scalarRange`、`dataRevision`、`bindingRevision`；几何已转为内部 RAS |
| 后续 GUI 更新/绘制 | `SendUpdates()` 提交变化；目标视图 `SendRender()` 返回 `Rendered/Unchanged` | `Deferred` 保留待画项；`Failed` 报告视图 ID 并排查 | 完成回调后仍需继续 pump，不能立即停 timer |
| 逐视图读取 `GetSceneViewState({id})` | 有值、`isAvailable=true`，`presentation` 的数据修订匹配加载结果，`renderedEpoch >= sceneEpoch` | 空值、不可用、修订不匹配或画面尚未覆盖当前提交 | 对第 1 节的 4 个 ID 分别检查 |
| Qt 控件实际显示 | 3D 与三切片出现对应数据，窗口最大化/还原、最小化恢复后仍显示正常 | 黑屏、旧帧或恢复后空白 | 实际画面检查通过，才结束首帧等待 |

`Rendered` 表示 VTK 绘制结束，仍需确认 Qt 控件实际呈现该帧。四视图空白时依次检查第 3 节的初始化顺序、控件可见性及 ReadyForRendering、待画 ID 是否保留、每视图绘制返回和 Qt 画面；加载回调成功不能替代这些检查。

## 5. Host 有 12 种请求

以下请求均由 `session.SendRequest(std::move(request), callback)` 发送。同步成功为 `Accepted`，失败为 `Rejected + errorCode/message`；已接纳请求最终成功为 `HostResult.isSucceeded=true`，失败为 `false + errorCode/message`。

| 请求 | 必填输入 / 可选输入 | 成功后的预期输出 |
| --- | --- | --- |
| `HostLoadRequest` | UTF-8 `filePath`；`geometry`；`metadata` | 文件图像发布并选择为主输入 |
| `HostReloadRequest` | 拥有存储的 `voxels`；`geometry`；`metadata`；连续 float32，X 最快，元素数等于尺寸乘积 | 内存图像发布并选择为主输入 |
| `HostLoadActivationRequest` | 已接纳的 `dataRevision`；观察到的 `expectedBindingRevision` | 对该修订执行显示组激活，可用于激活失败后的重试 |
| `HostDataSelectRequest` | 已发布图像的 `dataRevision`；当前 `expectedBindingRevision` | 选择该图像，不重新生成数据；后续绘制显示选择结果 |
| `HostDataUnloadRequest` | 观察到的实体 head `dataRevision` | 退役该数据；仍有读者/业务使用时可失败，不删除源文件 |
| `HostViewSetRequest` | `targetView`；按需填写 `mode/iso/windowLevel/material/background/opacity` 等 optional | 只修改该视图提供的字段，未提供字段保留 |
| `HostSessionSetRequest` | 按需填写 `spacing` 或 `cursor` | 修改共享几何/世界游标；没有单视图 selector |
| `HostViewResetRequest` | `targetView` | 目标视图相机复位 |
| `HostToolSetRequest` | `targetView`、`toolMode` | 设置目标工具模式 |
| `HostToolSwitchRequest` | `targetView` | 切换目标既有工具模式 |
| `HostDataExportRequest` | UTF-8 输出目录 `outputPath`；可选 `sourceView`、`format=Raw/Ply/Stl/Obj` | 在目录内生成冻结数据文件，文件名由数据层生成 |
| `HostSliceExportRequest` | UTF-8 输出目录 `outputDir`；切片 `sourceView`；可选 `angleDeg` | 在目录内生成 PNG 切片 |

Host 错误码：`SessionNotReady` 检查初始化/停止状态；`WrongThread` 改到 GUI 线程；`RequestRejected` 检查目标和参数；`OperationFailed` 根据完成消息处理。成功错误码为 `None`。

## 6. 裁切：创建文档 → 编辑 → 构建 → 应用

Crop 通过组合接入，不继承 `CropHostFeature`。上位机至少保存当前 `documentId/rootNodeId`、构建结果和最后一次查询的版本；从最新 `GetHistory(documentId)` 取得 `stateRevision`，不要用节点数量当作 nodeId。

### 6.1 创建文档

以下每个 C++ 片段放入对应的 GUI 按钮处理函数中；`window` 是第 3 节的 `CtWindow&`。窗口成员保存裁切文档、裁切结果和孔隙结果，跨按钮调用复用；回调用 `QPointer` 检查窗口生命期。先加载成功，再创建文档。

```cpp
auto document = window.cropDocumentState;
const auto image = window.session().GetImageDescriptor();
if (!image) return; // UI 提示先加载数据。

const auto target = MakeCropTarget(window);
CropDocumentRequest create;
create.action = CropDocumentAction::CreateDocument;
create.requestId = CropHostFeature::CreateRequestId();
create.target = target;
create.sourceRevision = image->dataRevision;
// Create 的 documentId 和 expectedRevision 保持 0。
QPointer<CtWindow> owner(&window);
const auto admission = window.crop().SendRequest(create,
    [owner, document](CropDocumentOutcome result) {
        if (!owner) return;
        if (result.status != CropEditStatus::Succeeded) {
            owner->statusBar()->showMessage(QStringLiteral("创建裁切文档失败"));
            return;
        }
        *document = std::move(result); // 保存 documentId、rootNodeId、stateRevision。
    });
if (!admission.isAccepted) {
    // 读取 admission.failureReason；不继续启动 widget，也不等待回调。
}
```

预期输出：创建成功得到非零 `documentId/rootNodeId`。随后继续 GUI pump，检查 `crop.GetState(documentId).views` 中各目标的 `renderedHead==rootNodeId` 且 `isRenderPending==false`；Root 已显示后才启用裁切按钮。

### 6.2 启动 Box 并保留内部

```cpp
const auto target = MakeCropTarget(window);
CropHostRequest box;
box.action = CropHostAction::Box;
box.target = target; // 与创建文档相同的输入和视图。
if (!window.crop().SendRequest(box)) return;

CropHostRequest mode;
mode.action = CropHostAction::Mode;
mode.target = target;
mode.removalMode = CropRemovalMode::KeepInside;
if (!window.crop().SendRequest(mode)) return;
```

输入：Root 已显示的文档及相同 target。成功：两个调用返回 `true`，显示 Box，用户拖动后生成预览/历史。失败：返回 `false`，保持 UI 可报告失败，不进入构建等待。普通 widget 动作无业务完成回调，画面仍由 pump 绘制。

### 6.3 构建正式结果

拖拽结束后，确认 `GetState(documentId).history.isDragging==false`、`isPublishing==false`，且历史的 `appliedHead==renderedHead`，再执行：

```cpp
auto document = window.cropDocumentState;
auto output = window.cropBuildState;
QPointer<CtWindow> owner(&window);
const auto history = window.crop().GetHistory(document->documentId);
CropBuildRequest build;
build.documentId = document->documentId;
build.nodeId = history.appliedHead;
build.requestId = CropHostFeature::CreateRequestId();
build.expectedRevision = history.stateRevision;
// options 保持默认：自动 RAM 预算、meshTolerance=0.05。
const auto buildAdmission = window.crop().SendRequest(build,
    [owner, output](CropBuildResult result) {
        if (!owner) return;
        *output = std::move(result);
        owner->statusBar()->showMessage(output->isSucceeded
            ? QStringLiteral("裁切构建完成")
            : QString::fromStdString(output->message));
    });
if (!buildAdmission.isAccepted) {
    // 读取 buildAdmission.failureReason；没有后续完成回调。
}
```

成功回调：`isSucceeded=true`，得到 `outputRevision`、`sourceRevision`、`resultId`、`commitId`。失败回调：`isSucceeded=false`，读取 `failureReason/message/blockers`。构建结果独立保存，不会自动替换主图像。

### 6.4 选择构建出的图像

仅在构建成功、当前输入仍是该 `sourceRevision`，且 `GetImageDescriptor(outputRevision)` 有值时执行：

```cpp
auto output = window.cropBuildState;
QPointer<CtWindow> owner(&window);
const auto current = window.session().GetImageDescriptor();
const auto generated = window.session().GetImageDescriptor(output->outputRevision);
if (!output->isSucceeded || !current || !generated
    || current->dataRevision != output->sourceRevision) return;
HostDataSelectRequest select;
select.dataRevision = output->outputRevision;
select.expectedBindingRevision = current->bindingRevision;
const auto selected = window.session().SendRequest(std::move(select),
    [owner](HostResult result) {
        if (owner && !result.isSucceeded)
            owner->statusBar()->showMessage(QString::fromStdString(result.message));
    });
if (selected.status != HostAdmissionStatus::Accepted) {
    // 读取 selected.errorCode/message，结束选择等待。
}
```

预期输出：主输入选择为该图像修订，四视图随后刷新。非图像结果不走此图像选择流程。

### 6.5 Crop 动作和失败处理表

| 请求族 / 动作数 | 输入 | 成功 / 失败返回 | 预期输出 |
| --- | --- | --- | --- |
| 文档 5 个：`CreateDocument/ActivateDocument/RestoreDocument/ReturnToSource/CloseDocument` | `CropDocumentRequest`；已有文档操作填写 `documentId/requestId/expectedRevision`；Restore 另填 archive | `CropDocumentAdmission.isAccepted`；失败看 `failureReason`；回调看 `CropDocumentOutcome.status` | 建立、激活、恢复、返回源或关闭文档；Close 完成后 `documentStatus=Closed` |
| 编辑 4 个：`Append/Replace/Select/Prune` | `CropEditRequest` 的 document、唯一 request、当前版本、`nodeId`，及 operation/prune | `CropEditAdmission.isAccepted`；回调 `CropEditOutcome.status`，失败看 `failureReason` | 修改历史或选择精确节点；选择 Root 用 `kind=Select,nodeId=rootNodeId` |
| 正式构建 1 个 | 第 6.3 节的 `CropBuildRequest` | `CropBuildAdmission.isAccepted`；回调 `CropBuildResult.isSucceeded` | 独立正式结果及其修订 |
| widget 13 个：`Start/Box/Plane/Cylinder/Sphere/Mode/Previous/Next/BuildResult/SetPolyData/ClearPolyData/Exit/SaveRoi` | `CropHostRequest`；形状/模式使用既定 target；Mode 填 `None/KeepInside/RemoveInside`；SetPolyData 填 polyData | 普通动作返回 bool；`BuildResult` 必须提供构建回调；`SaveRoi` 的已接纳回调同步执行 | 交互、历史前后跳转、构建、附加几何或保存 ROI |

`Exit` 只退出交互；返回原图用 `ReturnToSource`，关闭文档用 `CloseDocument`，三者分别发送。文档/编辑/构建终态的成功状态为 `Succeeded`，失败状态为 `Failed/Cancelled`；`Queued` 仍需等待。

常见失败：`NoImage` 先加载；`Busy/PreviewNotReady` 等待拖拽/预览/发布完成；`StateVersionMismatch/VersionMismatch` 重新查询版本；`SourceMismatch` 核对源修订；`NoCropOperations` 先形成有效操作；`ResultInUse` 先处理占用并读取 blockers；`LowRam` 调整本次构建预算或释放其他结果。已接纳后不要用新 requestId 重复提交同一副作用动作，可按原 requestId 查询 outcome。

## 7. 孔隙分析：Start、显示控制和 Exit

Gap 有 **5 个有效动作**：`Start/Overlay/Exit/SetDisplay/Evaluate`。每个动作都调用 `gap.SendRequest(request, callback)`，返回 `GapAdmission`，不是 bool。

### 7.1 启动

先加载主图像，再填写以下输入：

| 输入 | 填写要求 | 不满足时 |
| --- | --- | --- |
| `targetViews` | 使用已建立的非空视图集合 | 拒绝请求 |
| `surface.isoMode` 和对应阈值 | `DataRangeRatio` 填 `[0,1]` 内的 `dataRangeRatio`，阈值为 `min+(max-min)*ratio`；`AbsoluteValue` 填输入体素灰度域的 `absoluteIsoValue` | 越界比例不会被自动钳制；无效数值拒绝请求 |
| `surface.backgroundMean/materialMean` | 输入体素灰度域的有限值，且背景均值不大于材料均值 | 拒绝请求；不要填写渲染背景色或材质参数 |
| `voidParams` | `isFilterEnabled` 控制体积过滤；`minVolumeMM3` 为非负有限值，且能表示为 float，单位 mm³ | 无效数值拒绝请求；关闭过滤时此阈值不参与筛选 |

```cpp
QPointer<CtWindow> owner(&window);
GapHostStartParams params;
params.targetViews = window.targets();
params.surface.isoMode = GapIsoMode::DataRangeRatio;
params.surface.dataRangeRatio = 0.5;
params.surface.backgroundMean = -0.197839f; // 本文 1000³ 文件的示例输入。
params.surface.materialMean = 0.198859f;
params.voidParams.isFilterEnabled = false;
params.voidParams.minVolumeMM3 = 0.0;

auto gapResult = window.gapResultState;
GapHostRequest request;
request.action = GapHostAction::Start;
request.start = params;
window.statusBar()->showMessage(QStringLiteral("正在分析孔隙……"));
const auto gapAdmission = window.gap().SendRequest(request,
    [owner, gapResult](GapHostResult result) {
        if (!owner) return;
        *gapResult = std::move(result);
        if (gapResult->status == GapResultStatus::Succeeded)
            owner->statusBar()->showMessage(QStringLiteral("孔隙分析完成，等待画面刷新"));
        else if (gapResult->status == GapResultStatus::SucceededWithDisplayFailure)
            owner->statusBar()->showMessage(QStringLiteral("孔隙结果已生成，显示失败：")
                + QString::fromStdString(gapResult->message));
        else
            owner->statusBar()->showMessage(QString::fromStdString(gapResult->message));
    });
if (gapAdmission.status != GapAdmissionStatus::Accepted) {
    // InvalidRequest/Busy/Stopping/Unavailable：不等回调，结束 UI 等待。
    window.statusBar()->showMessage(QStringLiteral("孔隙请求未接纳，检查参数和会话状态"));
    qWarning() << "Gap admission:" << static_cast<int>(gapAdmission.status);
}
```

示例均值只用于所示文件；换数据时填写对应输入。成功/失败按下面的两个阶段处理：

| 阶段 | 成功返回 | 失败返回 | 预期输出 |
| --- | --- | --- | --- |
| 接纳 | `GapAdmission.status=Accepted`，含 `requestId` | `InvalidRequest/Busy/Stopping/Unavailable`；拒绝不回调 | 已接纳后继续 GUI pump |
| 业务终态 | `GapHostResult.status=Succeeded` | `Failed/SourceChanged`，读取 `message` | 成功得到统计、`labelMap/voidTable/voidMesh/statisticsData/resultSet` 修订和 `sourceRevision/commitId` |
| 结果已产生、显示失败 | `SucceededWithDisplayFailure` | 此状态不能记为画面成功 | 保存已发布结果，报告显示失败；不要反报“没有分析结果” |

`GetState().analysisState` 为 `Idle/Running/Succeeded/Failed/Stale`；`Stale` 表示输入已变化，需要重新分析。`statistics` 包含物体/孔隙体素数、体积、`porosityRatio` 和 `isPorosityAvailable`；体积单位 mm³，比例不是百分数。

收到 `Succeeded` 或 `SucceededWithDisplayFailure` 后保存统计和全部结果修订。只有 `isPorosityAvailable=true` 时显示孔隙率，可用 `porosityRatio*100` 显示百分数。`GetDisplay(resultSet)` 返回当前显示参数，有值不等于 Qt 画面已经呈现；显示验收仍按第 4 节逐视图检查，并确认结果色带、显隐控制及窗口恢复后画面正常。`SucceededWithDisplayFailure` 单独提示显示错误，保留已生成结果。

计算、结果绘制和文件导出分别等待各自完成条件。GUI 线程保持事件循环，不用 `sleep`、忙循环或同步等待回调；也不要在回调中复制整卷或写大文件。等待时间按数据规模和 Debug/Release 分别设置，Debug 整卷分析可能耗时数十分钟；`Running` 本身不能判为宕机。应用等待超时时报告“超时，完成状态待确认”，保留会话、数据和已完成结果；需要关闭时走 `Stop()` 流程。

### 7.2 其余 4 个动作

| 动作 | 输入 | 成功返回 / 完成条件 | 失败返回 / 处理 | 预期输出 |
| --- | --- | --- | --- | --- |
| `Overlay` | 已有当前显示结果；只填写 action | admission Accepted；可查询 `isOverlayVisible` | 非 Accepted 时结束等待；不重启分析 | 切换孔隙 overlay 显隐 |
| `SetDisplay` | `displayResult=当前 resultSet`；`display=完整 GapDisplayParams`，可先 `GetDisplay(resultSet)` 取现值再修改 | admission Accepted；回调成功，可重新查询参数 | 非 Accepted 或回调失败；原显示参数保留 | 修改色带、透明度、图例，不重新检测 |
| `Evaluate` | `start` 参数、可选 evaluation/detection ROI 和 parentResult；目标视图仍非空 | admission Accepted，回调按 7.1 处理 | 非 Accepted 或 `Failed/SourceChanged` | 对已有结果评估并返回统计/新结果修订 |
| `Exit` | 只填写 action | admission Accepted 仅表示开始退出；回调成功后 `isViewActive=false,isExitPending=false` | 拒绝不回调；Stop/Detach 可使已接纳退出以失败终态收口 | 解绑显示并结束退出等待 |

退出调用示例：

```cpp
QPointer<CtWindow> owner(&window);
GapHostRequest exit;
exit.action = GapHostAction::Exit;
window.statusBar()->showMessage(QStringLiteral("正在退出孔隙分析……"));
const auto exitAdmission = window.gap().SendRequest(exit,
    [owner](GapHostResult result) {
        if (!owner) return;
        owner->statusBar()->showMessage(result.status == GapResultStatus::Succeeded
            ? QStringLiteral("孔隙已退出") : QString::fromStdString(result.message));
    });
if (exitAdmission.status != GapAdmissionStatus::Accepted) {
    // 结束 UI 退出等待；查询当前状态。
    window.statusBar()->showMessage(QStringLiteral("退出请求未接纳，请查询当前孔隙状态"));
}
```

退出完成以回调成功且 `isViewActive=false,isExitPending=false` 为准；`analysisState` 可以仍为 `Succeeded`，不要等待它变成 `Idle`。退出待处理期间继续 GUI pump，保留关联视图。若此时关闭整个宿主，按 Session `Stop()` 收口，不再单独等待 Gap Exit 成功。

普通 API 输出聚合统计和结果标识，**没有逐孔隙明细行读取或明细 CSV 导出方法**。`voidTable` 是标识，不是行集合。可把公开 statistics 保存为上位机自己的文件；不要编写不存在的 `Export` action 或 `GetVoidRegions()` 调用。

## 8. 读数据、导航和可以继承的对象

### 8.1 查询及读取的输入输出

| 调用 | 输入 | 成功输出 | 失败输出 |
| --- | --- | --- | --- |
| `GetRenderViewState(target)` | 有效单视图 selector | optional 中的显示属性、数据/绑定修订 | 空 optional |
| `GetSceneViewState(target)` | 有效 selector，owner thread | optional 中的场景、相机、`sceneEpoch/renderedEpoch` | 空 optional；同时检查 `isAvailable` |
| `GetImageDescriptor()` | 无；也可指定已知 image revision | 图像元信息，不复制体素 | 空 optional |
| `GetImageReadResult(request)` | 可选半开 region；`maxBytes=0` 为自动预算，非零为显式预算 | `error=None` 且 `state` 有值；只读字节与几何/类型 | `NoImage/InvalidData/UnsupportedType/InvalidRegion/TooLarge/CopyFailed/Cancelled/ResultRetired` |
| `GetImageReadChunk(request, offset)` | 同一 region；X-fast voxel offset | `error=None`、`state`、`nextVoxelOffset/isDone`；每块至多 8 MiB | 同一组 ImageReadError；停止本次读取 |
| `StartImageRead(request, callback)` | 请求与非空完成回调 | admission Accepted；后续 owner 更新兑现 `ImageReadResult` | `InvalidRequest/Busy/QueueFull/Stopping/Unavailable` |

读取结果按 `valueType/componentBytes/componentCount/byteOrder/tupleOrder` 解释。分块读取跨多次调用时保存起始数据修订，输入变化则结束并重新读取，避免混用不同数据。不要为元信息查询复制整卷。

导航先取 `auto controller=window.session().GetInteractionController()`，再取 `controller->GetNavigation(viewId)`；空句柄结束操作。`ZoomBy(factor)`、`PanByPixels(dx,dy)`、`ScrollSlices(count)`、`SetWindowLevel(width,center)`、`FitToView()` 返回 `NavigationStatus`：成功为 `Applied`；失败为 `Unbound/Invalid/Unsupported/Busy/WrongThread/Failed`。缩放倍率和窗宽为正有限值，切片步数是整数。默认 QVTK 原生输入不重复注入；显式选择 `HostInjected` 时，输入坐标使用 VTK 左下角 display 像素。

### 8.2 只有需要定制时才继承

| 可定制入口 | 实现什么 | 安装 / 传入什么对象 | 成功/失败处理 |
| --- | --- | --- | --- |
| `ViewNavigation` | 按需覆盖 `BuildZoom/BuildPan/BuildRotate/BuildSliceScroll/BuildWindowLevel`，均为 const 纯计算 | `controller->SetNavigation(viewId, shared_ptr<ViewNavigation>)` | 返回 `NavigationStatus`，`Applied` 成功；方法内不重入视图写入 |
| `IInputBindings` | `InputBindingOverride GetOverride(std::string_view, InputBinding&) const noexcept` | `controller->ImportBindings(bindings, ...)` | 返回 `BindingUpdateResult`：`status=Applied` 成功；`Invalid/Conflict/Unsupported/Busy/Failed` 失败，读取 diagnostics；一般直接用 ConfigurableInputBindings |
| `RoiArchiveReader` | 实现 `GetMetadata/GetMaskCount/GetVoxelCount/GetMaskChunk/GetVoxelChunk`；返回同一归档的元数据、计数和块 | `session.LoadRoiArchiveReader(reader, mappings)` | 返回 `RoiArchiveRestoreResult`：`error=None` 成功，输出 rois/revisionMapping；其他 RoiError 为失败；源 key 映射到精确源修订 |

`VtkAppHostSession`、`InteractionController`、`CropHostFeature`、`GapHostFeature`、`MetrologyAlignmentHostFeature` 均为 final，通过组合使用。`ConfigurableInputBindings` 是现成的绑定实现。

`HostRequest` 是内置请求的传输基类，派生自定义请求不会获得路由；`HostInputEndpoint` 使用 Session 返回的对象。`IDataPayload/DataGraphView/DataLifetimeAccess/DataResourceLease/DataChangeBatch` 是数据协议闭包，普通上位机没有通过 Session 注入这些实现的入口，不需要为接入实现它们。

## 9. 仓内 CT 工程的编译入口

CT 使用 `CT/MVVCVTK.sln`；SDK 路径和库/runtime 适配位于 `CT/MVVCVTK/MVVCVTK.Sdk.props`。在仓库根执行：

```powershell
$SDK_ROOT = 'F:\SDK\MVVCVTK'
$QT_ROOT = 'D:\qt\5.14.2\msvc2017_64'
foreach ($config in @('Debug', 'Release')) {
    MSBuild.exe CT/MVVCVTK.sln /t:Build /m:2 /p:Platform=x64 "/p:Configuration=$config" `
        "/p:MVVCVTKSdkRoot=$SDK_ROOT" "/p:QtInstall=$QT_ROOT" `
        "/p:QtMsBuild=$env:LOCALAPPDATA\QtMsBuild"
}
```

成功：生成 `CT/out/x64/Debug/MVVCVTK.exe`、`CT/out/x64/Release/GviewCT.exe` 并复制依赖。失败：MSBuild 给出缺头、缺库、runtime 或 Qt 配置错误；修正后重新编译。

SDK 根优先级：显式 `/p:MVVCVTKSdkRoot` → 环境变量 `MVVCVTK_SDK_ROOT` → `CT/out/ct-sdk.props` 的本机缺省值。属性表只链接 CT 使用的 `MVVCVTKHost.lib`、`OrthogonalCrop.lib`、`GapAnalysis.lib`，按 Debug/Release 分别检查和部署。CT 自有重建库继续从 `CT/libs` 提供。

## 附：按功能找 API 头

路径相对于 `<SDK>/include/MVVCVTK/API/`；通常包含产品入口即可取得其请求/结果声明。

| 功能 | 头文件 |
| --- | --- |
| Host | `Host/VtkAppHostSession.h`；`Host/Types/HostRequestTypes.h`、`Host/Types/HostSessionTypes.h`、`Host/Types/HostValueTypes.h`、`Host/Types/HostViewTypes.h` |
| 输入与导航 | `Interaction/InteractionController.h`、`Interaction/InputBindings.h`、`Interaction/ViewNavigation.h`、`Interaction/NavigationTypes.h` |
| 数据读取与 ROI | `Data/ImageReadTypes.h`、`Data/DataGraphTypes.h`、`Data/RoiTypes.h` |
| 裁切 | `Features/OrthogonalCrop/Host/CropHostFeature.h`；`Features/OrthogonalCrop/OrthogonalCropTypes.h` |
| 孔隙 | `Features/GapAnalysis/Host/GapHostFeature.h`、`Features/GapAnalysis/Host/GapHostTypes.h` |
| 可选计量组件 | `Features/MetrologyAlignment/Host/MetrologyAlignmentHostFeature.h`、`Features/MetrologyAlignment/Host/MetrologyAlignmentHostTypes.h`、`Features/MetrologyAlignment/Host/MetrologyReport.h`、`Features/MetrologyAlignment/Host/VoxelVolume.h`；使用时显式链接该组件 |
