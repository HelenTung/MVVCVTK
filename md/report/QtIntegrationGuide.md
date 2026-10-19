# Qt 宿主接入指南

Qt 宿主负责窗口、布局和事件循环，`VtkAppHostSession` 负责数据、视图和功能扩展（Feature）的组合。应用先建立会话，再加载或选择数据，最后按业务需要挂载功能。以下章节从宿主能调用的公开入口出发，每章都给出前置条件、参数、调用过程和结果处理方法。

会话在 Qt 界面线程创建，这个线程就是会话所属线程（owner thread）。会话构建、请求、功能挂载、导航、更新、绘制和显式停止都放在这个线程。完成回调的线程和重入规则按各章分别说明；后台线程只处理已经取得的不可变结果，不直接访问 Qt 控件或渲染对象。

正常生命周期是：创建控件和渲染窗口 → `BuildSession()` → 加载数据 → 挂载并使用功能 → 停止宿主自己的后台任务 → `Stop()` 成功 → 释放会话和窗口。Qt 使用自己的事件循环，不调用会话的 `Start()`。`Stop()` 返回 `false` 时，保留会话、功能对象和窗口，在后续界面事件中重试。

## 功能索引与可用范围

| 章节 | 接入内容 | 可用范围 |
| --- | --- | --- |
| [1. 基础会话与窗口嵌入](#1-基础会话与窗口嵌入) | 工程配置、四视图、按需更新、关闭 | Host |
| [2. 数据加载与切换](#2-数据加载与切换) | RAW、TIFF 序列、内存体素、激活、选择、卸载 | Host |
| [3. 视图模式](#3-视图模式) | 三向切片、体渲染、等值面、组合视图 | Host |
| [4. 窗宽窗位与显示参数](#4-窗宽窗位与显示参数) | 材质、传输函数、质量、标尺、光标 | Host |
| [5. 导航与输入](#5-导航与输入) | 缩放、平移、旋转、翻片、工具、绑定与事件注入 | Host |
| [6. 数据读取与导出](#6-数据读取与导出) | 元信息、图像和标签分块读取、模型和切片导出 | Host |
| [7. ROI](#7-roi) | 正式区域、布尔和形态操作、交互草稿、提交 | Host 数据接口；交互编辑器仅仓内构建 |
| [8. 裁切](#8-裁切) | 盒、平面、圆柱、球；历史、预览和正式结果 | OrthogonalCrop |
| [9. 表面测定与提取](#9-表面测定与提取) | 自动阈值、正式测定、预览、对象和几何读取 | 仅仓内构建 |
| [10. 零件分割与编辑](#10-零件分割与编辑) | 分割、对象目录、刷子、生长、拆分、合并和历史 | 仅仓内构建 |
| [11. 孔隙分析](#11-孔隙分析) | GapAnalysis 检测、区域评价、对象和显示 | GapAnalysis |
| [12. 壁厚](#12-壁厚) | 原始灰度场壁厚、统计、评价、显示和采样 | 仅仓内构建 |
| [13. 模型旋转](#13-模型旋转) | 数值旋转、交互旋转、取消和撤销 | 仅仓内构建 |
| [14. 计量对齐与尺寸评价](#14-计量对齐与尺寸评价) | 参考网格、配准、偏差、尺寸、体素体积和报告 | 可选 MetrologyAlignment 组件 |
| [15. 伪影抑制](#15-伪影抑制) | 扩散、环状伪影校正、候选审核和发布 | 仅仓内构建 |

SDK（Software Development Kit，开发包）安装面由 CMake 头映射和安装规则决定。当前 `find_package(MVVCVTK ...)` 可请求的产品组件是 `Host`、`OrthogonalCrop`、`GapAnalysis` 和可选的 `MetrologyAlignment`。其他功能具有仓内公共入口，可以由仓内 Qt 宿主组合，但不能据此假定开发包里也有相应头文件和库。

| 功能 | 链接目标 | 仓内总开关及默认值 |
| --- | --- | --- |
| Host | `MVVCVTK::Host` | Host 始终构建 |
| 裁切 | `MVVCVTK::OrthogonalCrop` | `MVVCVTK_BUILD_ORTHOGONAL_CROP=ON` |
| 孔隙分析 | `MVVCVTK::GapAnalysis` | `MVVCVTK_BUILD_GAP_ANALYSIS=ON` |
| ROI 编辑 | `MVVCVTK::RoiEditing` | `MVVCVTK_BUILD_ROI_EDITING=OFF` |
| 表面测定 | `MVVCVTK::SurfaceDetermination` | `MVVCVTK_BUILD_SURFACE_DETERMINATION=OFF` |
| 零件分割 | `MVVCVTK::PartSegmentation` | `MVVCVTK_BUILD_PART_SEGMENTATION=OFF` |
| 壁厚 | `MVVCVTK::WallThickness` | `MVVCVTK_BUILD_WALL_THICKNESS=OFF` |
| 模型旋转 | `MVVCVTK::ModelRotation` | `MVVCVTK_BUILD_MODEL_ROTATION=OFF` |
| 计量对齐 | `MVVCVTK::MetrologyAlignment` | `MVVCVTK_BUILD_METROLOGY_ALIGNMENT=OFF` |
| 伪影抑制 | `MVVCVTK::ArtifactReduction` | `MVVCVTK_BUILD_ARTIFACT_REDUCTION=OFF` |

安装包示例使用 `MVVCVTK/API/...` 头路径。仓内示例使用链接目标公开的源码形式头路径，例如 `"Host/SurfaceDeterminationHostFeature.h"`；这些路径由目标的包含目录或构建期头暂存目录提供，不需要自行添加内部源码目录。宿主通过 `AttachFeature(shared_ptr)` 组合已有功能，不直接调用功能的 `AttachHost()` 或 `OnHostTick()`。

各章包含创建和挂载的示例用于首次接入。宿主把返回的功能句柄保存为页面或会话成员，后续计算和参数更新复用该实例的请求入口，不在每次点击按钮时重复挂载同名功能。

以下 C++ 短代码是单次操作的 Qt 槽函数片段。session 是宿主持有的 `std::shared_ptr<VtkAppHostSession>`；roiEditor、crop、surface、parts、gap、wall、rotation、alignment、artifact 是各章挂载后保存的功能句柄。表格中的业务参数由对话框、参数面板或前一步结果提供。除工程配置、Qt 初始化和控件子类外，片段均在会话所属线程执行；异步回调捕获界面对象时须使用 QPointer 等生命周期保护。

## 1. 基础会话与窗口嵌入

链接 MVVCVTK::Host，包含安装头 MVVCVTK/API/Host/VtkAppHostSession.h 及 Host/Types 下的公开类型头。平台为 Windows x64、MSVC v145、C++17；仓内 Qt/VTK 组合是 Qt 5.14.2、VTK 9.4.2。

1. 配置工程，并为每个 Qt 控件创建独立渲染窗口。
2. 连接所属线程投递器及工作通知，再构建会话。
3. 按通知先更新后绘制，窗口隐藏时保留需求。
4. 停止宿主任务和会话，成功后释放窗口。

### 1.1 链接工程

最小宿主链接 Host 实现；仅链接 HostAPI 不能创建会话。其他组件按功能加入链接。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `Host` | `CMake 组件` | 提供会话实现。 |
| `Qt5::Widgets、Qt5::OpenGL、VTK::GUISupportQt` | `CMake 目标` | Qt 由宿主提供，版本须配套。 |

出参与返回：工程配置没有业务回调。CMake 缺包、版本不匹配或目标不存在时配置失败；成功后仍要分别构建 Debug/Release 并部署对应运行依赖。

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
target_compile_options(QtViewer PRIVATE /utf-8 "/FI${CMAKE_CURRENT_SOURCE_DIR}/QtStlCompat.h")
target_link_libraries(QtViewer PRIVATE MVVCVTK::Host
    Qt5::Widgets Qt5::OpenGL VTK::GUISupportQt)
# 静态 VTK 渲染模块须自动初始化。
vtk_module_autoinit(TARGETS QtViewer MODULES VTK::RenderingOpenGL2
    VTK::RenderingVolumeOpenGL2 VTK::InteractionStyle)
```

### 1.2 初始化 Qt

在 QApplication 前设置数值区域和 QVTK 表面格式，让控件取得正确 OpenGL 上下文。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `defaultFormat()` | `QSurfaceFormat` | 在控件创建前设为应用默认格式。 |

出参与返回：Qt 初始化没有 Host 状态枚举。主循环使用 app.exec()；Session 的 Start() 用于独立 VTK 事件循环，Qt 不调用。

```cpp
QLocale::setDefault(QLocale::c());
QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
// QApplication 必须比 Session 活得更久。
QApplication app(argc, argv);
QMainWindow viewer;
viewer.show();
return app.exec();
```

### 1.3 嵌入视图

把同一渲染窗口同时交给控件和会话。四视图重复本步骤：ID 使用 primary-3d、slice-top-down、slice-front-back、slice-left-right，角色分别为 Primary3D、TopDownSlice、FrontBackSlice、LeftRightSlice，模式与第 3 章对应。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `string` | 会话内唯一；后续请求用这个 ID。 |
| `role、viewMode` | `HostRenderViewRole、HostRenderMode` | 主三维、组合三维或对应切片角色及模式。 |
| `renderWindow` | `vtkSmartPointer<vtkRenderWindow>` | 对应控件的实际窗口。 |

出参与返回：本步骤构造 HostRenderViewConfig，没有完成回调。最终成功或失败由 BuildSession() 同步返回，重复 ID 或无效拓扑会构建失败。

```cpp
auto window = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
window->SetMultiSamples(0);
auto* widget = new QVTKOpenGLNativeWidget(panel);
widget->setRenderWindow(window); // 控件和 Session 使用同一窗口。
HostRenderViewConfig view;
view.id = "primary-3d";
view.role = HostRenderViewRole::Primary3D;
view.renderWindow = window;
view.window.viewInit.viewMode = HostRenderMode::CompositeIsoSurface;
config.renderViews.push_back(view); // config 是待构建的 HostSessionConfig。
```

### 1.4 准备首帧缓冲

当前 Qt/VTK 组合在控件初始化后建立缓冲附件，仓内 CT 控件使用相同写法。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `控件基类` | `QVTKOpenGLNativeWidget` | 先完成基类 initializeGL()。 |

出参与返回：这是控件生命周期回调，没有 Host 接纳结果。窗口就绪后执行的是 VTK 窗口 Start/End，不启动会话事件循环。

```cpp
class RenderWidget : public QVTKOpenGLNativeWidget {
public:
    using QVTKOpenGLNativeWidget::QVTKOpenGLNativeWidget;
protected:
    void initializeGL() override {
        QVTKOpenGLNativeWidget::initializeGL();
        auto* w = vtkGenericOpenGLRenderWindow::SafeDownCast(renderWindow());
        if (w && w->GetReadyForRendering()) {
            w->Start(); // 只建立附件，不派发 Host 业务回调。
            w->End();
        }
    }
};
```

### 1.5 连接工作通知

工作通知可来自任意线程，只负责排队。work 是宿主持有的 `shared_ptr<function<void()>>`，函数体在提交更新小节设置；queueWork 是宿主保存的排队函数。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `driveMode` | `HostDriveMode` | Qt 显式使用 HostDriven。 |
| `sendOwnerTask` | `function<bool(function<void()>)>` | 把任务排到所属线程。 |
| `onWorkAvailable` | `function<void()>` | 只排队，不同步调用 Session。 |

出参与返回：投递器同步返回 bool，真表示排队成功，假表示投递失败。工作通知返回 void，可能早于发送函数返回；work 和 QApplication 活到 Stop 成功，晚到通知通过弱引用忽略。

```cpp
config.driveMode = HostDriveMode::HostDriven;
config.sendOwnerTask = [](std::function<void()> task) {
    return task && QMetaObject::invokeMethod(qApp, std::move(task), Qt::QueuedConnection);
};
work = std::make_shared<std::function<void()>>(); // 宿主持有到 Stop 成功。
std::weak_ptr<std::function<void()>> weak = work;
queueWork = [weak] {
    QMetaObject::invokeMethod(qApp, [weak] {
        if (const auto task = weak.lock(); task && *task) (*task)();
    }, Qt::QueuedConnection); // 工作线程不直接访问 Session。
};
config.onWorkAvailable = queueWork;
config.sendDiagnostic = [](const std::string& message) {
    qWarning().noquote() << QString::fromStdString(message);
};
```

### 1.6 构建会话

窗口和回调填好后再构建，成功后复用会话。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `config` | `HostSessionConfig` | 非空视图集合和前面设置的线程回调。 |

出参与返回：BuildSession() 同步返回 bool：真为构建成功，假为配置或某个构建步骤失败，没有完成回调；诊断由 sendDiagnostic 消费。该入口幂等。

```cpp
session = std::make_shared<VtkAppHostSession>(std::move(config));
const bool built = session->BuildSession();
if (!built) {
    qWarning() << "BuildSession failed";
    return; // 不再发依赖 Running 状态的业务请求。
}
queueWork();
```

### 1.7 提交更新

把变化提交到场景。pending 是宿主保存的 `set<string>`；renderVisible 是下一小节的宿主绘制函数。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `session` | `会话句柄` | Running、HostDriven、所属线程。 |

出参与返回：SendUpdates() 同步返回 HostUpdateResult：Completed 已处理、Deferred 延后、Failed 失败、Stopped 已停止；renderViewIds 给出待绘制 ID。它不绘制，但可能在本次调用中派发已就绪的业务完成回调。

```cpp
*work = [this] {
    if (!session || session->GetIsStopped()) return;
    const auto update = session->SendUpdates();
    if (update.status == HostUpdateStatus::Stopped) return;
    if (update.status == HostUpdateStatus::Failed) qWarning() << "Update failed";
    pending.insert(update.renderViewIds.begin(), update.renderViewIds.end());
    renderVisible(); // 宿主函数，按下一小节筛选可见窗口。
};
```

### 1.8 绘制可见视图

只绘制可见且就绪的待绘制窗口。Show、Resize、Paint 后也调用同一排队入口，恢复被隐藏窗口的需求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `viewIds` | `vector<string>` | 精确 ID；空集合不绘制，未知或重复 ID 整批拒绝。 |
| `desiredUpdateRate` | `double` | VTK 时间预算率，不是 Qt 定时器周期。 |

出参与返回：SendRender() 同步返回 HostRenderResult。每视图 Rendered/Unchanged 可清除需求；Deferred 延后，Failed 失败，Stopped 已停止。它不隐式更新或派发业务回调，Rendered 不承诺显示器呈现。

```cpp
HostRenderRequest request;
request.viewIds = readyViewIds; // 从 pending 筛出可见且窗口已就绪的 ID。
if (request.viewIds.empty()) return;
const auto result = session->SendRender(request);
bool drew = false;
for (const auto& view : result.views) {
    drew = drew || view.status == HostRenderStatus::Rendered;
    if (view.status == HostRenderStatus::Rendered || view.status == HostRenderStatus::Unchanged)
        pending.erase(view.viewId);
    else if (view.status == HostRenderStatus::Failed) qWarning() << "Render failed";
}
if (drew) queueWork(); // 真实绘制后收口一次资源提交，空闲不自激。
```

### 1.9 挂载功能

由 Session 组合已有 Feature，并保存句柄；不直接调用其 AttachHost 或 OnHostTick。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `feature` | `shared_ptr<具体Feature>` | 相应组件已构建，不能重复挂载同名实例。 |

出参与返回：AttachFeature() 同步返回 bool：真为挂载完成，假为拒绝或失败，没有完成回调。更新/绘制期间拒绝拓扑变更。

```cpp
const bool attached = session->AttachFeature(feature);
if (!attached) {
    qWarning() << "Attach failed";
    return;
} // 保存 feature，后续按钮复用它。
```

### 1.10 移除功能

页面退出时在后续 Qt 事件移除，失败时继续保留功能和会话。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `feature` | `已挂载功能句柄` | 引用的是要卸载的同一对象。 |

出参与返回：DetachFeature(*feature) 同步返回 bool，真表示移除完成，假表示拒绝或清理未收口，没有独立完成回调。失败后在所属线程重试。

```cpp
const bool detached = session->DetachFeature(*feature);
if (detached) {
    feature.reset(); // 完成后才释放宿主持有的句柄。
} else {
    qWarning() << "Detach pending or failed";
}
```

### 1.11 停止会话

先停宿主请求和后台任务，再停止会话。失败时保留所有窗口，在后续关闭事件重试。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `会话与窗口` | `宿主成员` | QApplication 此时仍运行。 |

出参与返回：Stop() 同步返回 bool，真为停止完成，假为阶段未收口；GetStopState() 可读取状态，没有独立停止回调。dispatcher 失效时 Qt 所属线程可用 SendPendingStops() 补泵。

```cpp
if (session && !session->Stop()) {
    event->ignore(); // closeEvent 中不能先销毁窗口。
    QTimer::singleShot(50, this, [this] { close(); });
    return;
}
work.reset(); // 晚到通知的弱引用失效。
session.reset();
event->accept();
```

### 1.12 处理 Qt 5.14 编译兼容

当前 CT 对旧数组迭代器宏提供兼容头，同样的 Qt 5.14/v145 组合可沿用。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `QtStlCompat.h` | `强制包含头` | 在其他 Qt 头之前通过 /FI 包含。 |

出参与返回：编译设置没有 Host 回调。宏无法解析会编译失败；写法与 CT/MVVCVTK/c_ui/QtStlCompat.h 对照。

```cpp
#pragma once
#include <QtCore/qcompilerdetection.h>
#if defined(_MSC_VER)
// Qt 旧版本的数组迭代器宏改为直接使用指针。
#undef QT_MAKE_UNCHECKED_ARRAY_ITERATOR
#undef QT_MAKE_CHECKED_ARRAY_ITERATOR
#define QT_MAKE_UNCHECKED_ARRAY_ITERATOR(value) (value)
#define QT_MAKE_CHECKED_ARRAY_ITERATOR(value, size) (value)
#endif
```

## 2. 数据加载与切换

链接 Host，包含安装头 Host/Types/HostRequestTypes.h、HostValueTypes.h 和 Data/ImageReadTypes.h；这些路径位于 MVVCVTK/API/ 下。会话已构建并持续消费工作通知。

1. 取得真实几何和来源身份。
2. 分别处理接纳和完成，完成前不把输入记为正式结果。
3. 读取图像修订，更新、绘制并核对目标视图。

### 2.1 加载 RAW

RAW 入口读取连续 float32 单分量数据并核对大小，没有切换任意标量类型的字段。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `filePath` | `string` | UTF-8 输入路径。 |
| `geometry` | `HostVolumeGeometry` | XYZ 正尺寸、mm 间距、输入 LPS 原点和行主序方向矩阵；输出描述符转为 RAS。 |
| `metadata.source.kind` | `ImageSourceKind` | 文件来源必须显式设置，默认 Memory 不适用于文件。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostLoadRequest request;
request.filePath = path.toUtf8().toStdString();
request.geometry = geometry; // 使用实际采集几何，不填默认间距。
request.metadata.source.kind = ImageSourceKind::RawFile;
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << QString::fromStdString(result.message);
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Load rejected";
```

### 2.2 加载 TIFF 序列

TIFF 序列也要明确提供几何，不使用 RAW 文件名尺寸推断。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `filePath` | `string` | UTF-8 输入路径。 |
| `geometry` | `HostVolumeGeometry` | XYZ 正尺寸、mm 间距、输入 LPS 原点和行主序方向矩阵；输出描述符转为 RAS。 |
| `metadata.source.kind` | `ImageSourceKind` | 文件来源必须显式设置，默认 Memory 不适用于文件。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostLoadRequest request;
request.filePath = seriesPath.toUtf8().toStdString();
request.geometry = geometry; // 使用实际采集几何，不填默认间距。
request.metadata.source.kind = ImageSourceKind::TiffSeries;
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << QString::fromStdString(result.message);
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Load rejected";
```

### 2.3 加载内存体素

将自有解码或重建数组的存储所有权交给请求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `voxels` | `vector<float>` | XYZ 连续数组，X 最快，数量等于尺寸乘积。 |
| `geometry` | `HostVolumeGeometry` | 与数组一致的 LPS、mm 几何。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostReloadRequest request;
request.voxels = std::move(voxels); // 不借用短命外部缓冲。
request.geometry = geometry;
request.metadata.source.kind = ImageSourceKind::Memory;
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "Reload failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Reload rejected";
```

### 2.4 重试显示激活

对已接纳图像重试完整显示组事务，不重复读取文件。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `dataRevision` | `DataRevisionRef` | 明确的已接纳或已发布图像修订。 |
| `expectedBindingRevision` | `DataBindingRevision` | 观察到的绑定版本；0 只匹配尚不存在的绑定。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostLoadActivationRequest request;
request.dataRevision = selectedRevision;
request.expectedBindingRevision = observedBindingRevision;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "Image switch failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Switch rejected";
```

### 2.5 选择已发布图像

切换主图像绑定，不生成新数据或裁切历史。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `dataRevision` | `DataRevisionRef` | 明确的已接纳或已发布图像修订。 |
| `expectedBindingRevision` | `DataBindingRevision` | 观察到的绑定版本；0 只匹配尚不存在的绑定。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostDataSelectRequest request;
request.dataRevision = selectedRevision;
request.expectedBindingRevision = observedBindingRevision;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "Image switch failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Switch rejected";
```

### 2.6 卸载数据

先退出消费者并归还读取器，再退役实体及明确的派生结果；不删除源文件。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `dataRevision` | `DataRevisionRef` | 观察到的当前实体头修订，读取器、功能输入和非主绑定可能阻塞卸载。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostDataUnloadRequest request;
request.dataRevision = observedHead; // 防止卸载同一实体后来生成的修订。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << QString::fromStdString(result.message);
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Unload rejected";
```

## 3. 视图模式

链接 Host，使用 HostRequestTypes.h、HostValueTypes.h、HostViewTypes.h。HostViewTarget 按 ID 优先解析，未知 ID 不回退角色；多目标空集合不表示全选。

1. 确认目标 ID 和当前图像。
2. 发送只设置目标模式的 HostViewSetRequest。
3. 更新、绘制后从 GetRenderViewState() 核对。

### 3.1 体渲染

按传输函数积分显示体数据。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 实际存在的目标 ID。 |
| `mode` | `optional<HostRenderMode>` | 显式 Volume，其他未设置的显示字段保持当前值。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
request.mode = HostRenderMode::Volume;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "View mode failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Mode rejected";
```

### 3.2 等值面显示

按显示阈值绘制表面，不生成正式表面测定结果。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 实际存在的目标 ID。 |
| `mode` | `optional<HostRenderMode>` | 显式 IsoSurface，其他未设置的显示字段保持当前值。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
request.mode = HostRenderMode::IsoSurface;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "View mode failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Mode rejected";
```

### 3.3 体渲染与参考平面

在体渲染中加入三向参考平面。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 实际存在的目标 ID。 |
| `mode` | `optional<HostRenderMode>` | 显式 CompositeVolume，其他未设置的显示字段保持当前值。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
request.mode = HostRenderMode::CompositeVolume;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "View mode failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Mode rejected";
```

### 3.4 等值面与参考平面

在等值面中加入三向参考平面。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 实际存在的目标 ID。 |
| `mode` | `optional<HostRenderMode>` | 显式 CompositeIsoSurface，其他未设置的显示字段保持当前值。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
request.mode = HostRenderMode::CompositeIsoSurface;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "View mode failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Mode rejected";
```

### 3.5 上下方向切片

查看上下方向的正交切片。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 实际存在的目标 ID。 |
| `mode` | `optional<HostRenderMode>` | 显式 SliceTopDown，其他未设置的显示字段保持当前值。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"slice-top-down"};
request.mode = HostRenderMode::SliceTopDown;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "View mode failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Mode rejected";
```

### 3.6 前后方向切片

查看前后方向的正交切片。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 实际存在的目标 ID。 |
| `mode` | `optional<HostRenderMode>` | 显式 SliceFrontBack，其他未设置的显示字段保持当前值。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"slice-front-back"};
request.mode = HostRenderMode::SliceFrontBack;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "View mode failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Mode rejected";
```

### 3.7 左右方向切片

查看左右方向的正交切片。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 实际存在的目标 ID。 |
| `mode` | `optional<HostRenderMode>` | 显式 SliceLeftRight，其他未设置的显示字段保持当前值。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"slice-left-right"};
request.mode = HostRenderMode::SliceLeftRight;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "View mode failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Mode rejected";
```

### 3.8 复位视图

在当前数据范围内复位目标相机，适合加载和手动导航后的复位按钮。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 只复位明确的目标。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewResetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto admitted = session->SendRequest(std::move(request), [](HostResult result) {
    if (!result.isSucceeded) qWarning() << "Reset failed";
});
if (admitted.status != HostAdmissionStatus::Accepted) qWarning() << "Reset rejected";
```

## 4. 窗宽窗位与显示参数

链接 Host，包含安装头 HostRequestTypes.h 和 HostValueTypes.h。HostViewSetRequest 的字段可选，省略表示保持目标视图当前值；显示参数不改变业务计算或导出分辨率。

1. 确定目标视图和实际标量范围。
2. 只发送本次操作需要修改的字段。
3. 处理完成结果，读取 GetRenderViewState() 核对显示状态。

### 4.1 设置窗宽窗位

用于切片灰阶映射，宽度和中心都是原始标量域数值。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 要修改的视图。 |
| `windowWidth/windowCenter` | `double` | 宽度正且有限，中心有限；不是归一化 0～1 值。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"slice-top-down"};
HostWindowLevelParams level;
level.windowWidth = width;
level.windowCenter = center;
request.windowLevel = level;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Window level failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Request rejected";
```

### 4.2 设置等值面阈值

只调整目标等值面显示的阈值，不发布测定网格。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 等值面视图。 |
| `iso` | `optional<double>` | 实际标量域的有限阈值。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
request.iso = isoValue; // 与体素原始标量同量纲。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "ISO display failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Request rejected";
```

### 4.3 设置背景色

修改一个视图的背景，适合主题或截图样式设置。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `background` | `optional<HostBackgroundColor>` | RGB 分量均为 0～1。 |
| `targetView` | `HostViewTarget` | 仅影响该视图。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
request.background = HostBackgroundColor{0.08, 0.08, 0.08};
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Background failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Request rejected";
```

### 4.4 设置材质与透明度

调整光照和主显示对象透明度。预设可选 Soft、Dense、Glossy，具体参数也可直接提供。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `material` | `optional<HostMaterialParams>` | 宿主主动修改的光照参数和 isShadeOn。 |
| `opacity` | `optional<double>` | 0～1 的不透明度。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
HostMaterialParams material;
material.isShadeOn = true; // 体渲染开启光照着色。
request.material = material;
request.opacity = 0.8;
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Material failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Request rejected";
```

### 4.5 设置体渲染传输函数

提交完整的颜色和不透明度曲线，适合自有材料显示预设。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `colorNodes` | `vector<HostVolumeTransferFunction::ColorNode>` | 按实际标量坐标排列，RGB 为 0～1。 |
| `opacityNodes` | `vector<HostVolumeTransferFunction::OpacityNode>` | 实际标量坐标与 0～1 不透明度。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostVolumeTransferFunction curve;
curve.colorNodes = {{low, 0, 0, 0}, {high, 1, 1, 1}};
curve.opacityNodes = {{low, 0}, {high, 1}}; // low/high 须递增。
HostViewSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
request.volumeTransferFunction = curve;
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Transfer function failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Request rejected";
```

### 4.6 选择体渲染质量

向渲染层表达质量意图，实际采样和资源预算由内部资源链解析。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `volumeQuality` | `optional<HostVolumeQuality>` | Auto、Low、High、XHigh、Ultra；不改变计算分辨率。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
request.volumeQuality = HostVolumeQuality::High;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Quality change failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Request rejected";
```

### 4.7 控制参考平面、十字线和方向轴

显示开关彼此独立，可在同一视图请求中修改本次需要的项。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `visibility` | `optional<HostVisibilityParams>` | 可选 isPlanes3DVisible、isCrosshairVisible、isRulerVisible。 |
| `isAxesVisible` | `optional<bool>` | 世界方向轴标记开关。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
HostVisibilityParams visible;
visible.isPlanes3DVisible = true;
visible.isCrosshairVisible = true;
request.visibility = visible;
request.isAxesVisible = true;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Visibility failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Request rejected";
```

### 4.8 配置标尺

修改标尺单位、位置和尺寸；微米单位只转换标签，源几何仍为 mm。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `ruler.unit/position` | `HostRulerUnit、HostRulerPosition` | Auto/Millimeter/Micrometer 与四角位置。 |
| `targetPixels/fontSize` | `double、int` | 目标 40～2000 渲染像素；字号 8～64。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostViewSetRequest request;
request.targetView = HostViewTarget{"slice-top-down"};
HostRulerParams ruler;
ruler.unit = HostRulerUnit::Micrometer; // 只改显示标签，不改数据间距。
ruler.position = HostRulerPosition::BottomRight;
request.ruler = ruler;
HostVisibilityParams visible; visible.isRulerVisible = true;
request.visibility = visible;
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Ruler failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Request rejected";
```

### 4.9 移动共享光标

光标是 Session 状态，用于多视图联动，不是假装成单视图显示字段。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `cursor.world` | `array<double,3>` | VTK 世界坐标。 |
| `cursor.axis` | `int` | -1 写自由点，0/1/2 保持对应轴的当前联动位置。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostSessionSetRequest request;
HostCursorParams cursor;
cursor.world = worldPoint;
cursor.axis = -1; // 本次明确设置自由点。
request.cursor = cursor;
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Cursor change failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Request rejected";
```

### 4.10 校准数据间距

间距属于数据物理几何，只有业务明确需要校准时使用，不为切换标尺单位而改它。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `spacing` | `optional<array<double,3>>` | XYZ 物理间距，mm，正且有限。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostSessionSetRequest request;
request.spacing = calibratedSpacing; // 来自业务校准结果，单位 mm。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Spacing calibration failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Request rejected";
```

## 5. 导航与输入

包含 MVVCVTK/API/Interaction/ 下的 InteractionController.h、ViewNavigation.h、NavigationTypes.h、InputBindings.h。普通 QVTK 控件默认使用 NativeInteractor；导航句柄属于所属线程，不能跨停止或重建缓存。

1. 取得控制器和目标导航句柄。
2. 调用单次导航或修改绑定；检查同步状态。
3. 需要自转发事件时，在构建前选 HostInjected，再发送值事件。

### 5.1 倍率缩放

围绕锚点按相对倍率缩放。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `factor、anchor` | `double、ZoomAnchor` | factor 为正；锚点可选视口、指针、显示点、归一化点、十字线、世界点。 |

出参与返回：导航写方法同步返回 NavigationStatus，没有完成回调：Applied 已应用，Unbound 视图未绑定，Invalid 参数无效，Unsupported 视图不支持，Busy 忙，WrongThread 线程错误，Failed 执行失败。实际显示仍由更新和绘制推进。

```cpp
const auto controller = session->GetInteractionController();
const auto nav = controller ? controller->GetNavigation("primary-3d") : nullptr;
if (!nav) return;
const auto status = nav->ZoomBy(1.2); // 只操作该导航句柄对应的视图。
if (status != NavigationStatus::Applied)
    qWarning() << "Navigation failed:" << static_cast<int>(status);
```

### 5.2 设置绝对缩放

按宿主保存的绝对倍率恢复缩放。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `ratio、anchor` | `double、ZoomAnchor` | ratio 为正；默认锚点为视口中心。 |

出参与返回：导航写方法同步返回 NavigationStatus，没有完成回调：Applied 已应用，Unbound 视图未绑定，Invalid 参数无效，Unsupported 视图不支持，Busy 忙，WrongThread 线程错误，Failed 执行失败。实际显示仍由更新和绘制推进。

```cpp
const auto controller = session->GetInteractionController();
const auto nav = controller ? controller->GetNavigation("primary-3d") : nullptr;
if (!nav) return;
const auto status = nav->SetZoomRatio(2.0); // 只操作该导航句柄对应的视图。
if (status != NavigationStatus::Applied)
    qWarning() << "Navigation failed:" << static_cast<int>(status);
```

### 5.3 平移

按显示像素位移移动当前观察。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `deltaX/deltaY` | `double` | 像素位移，方向由导航坐标约定解释。 |

出参与返回：导航写方法同步返回 NavigationStatus，没有完成回调：Applied 已应用，Unbound 视图未绑定，Invalid 参数无效，Unsupported 视图不支持，Busy 忙，WrongThread 线程错误，Failed 执行失败。实际显示仍由更新和绘制推进。

```cpp
const auto controller = session->GetInteractionController();
const auto nav = controller ? controller->GetNavigation("primary-3d") : nullptr;
if (!nav) return;
const auto status = nav->PanByPixels(20, 0); // 只操作该导航句柄对应的视图。
if (status != NavigationStatus::Applied)
    qWarning() << "Navigation failed:" << static_cast<int>(status);
```

### 5.4 旋转观察方向

改变相机观察方向；模型自身姿态用模型旋转功能。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `yaw/pitch/roll、pivot` | `double、ZoomAnchor` | 角度为度，默认围绕视口中心。 |

出参与返回：导航写方法同步返回 NavigationStatus，没有完成回调：Applied 已应用，Unbound 视图未绑定，Invalid 参数无效，Unsupported 视图不支持，Busy 忙，WrongThread 线程错误，Failed 执行失败。实际显示仍由更新和绘制推进。

```cpp
const auto controller = session->GetInteractionController();
const auto nav = controller ? controller->GetNavigation("primary-3d") : nullptr;
if (!nav) return;
const auto status = nav->RotateBy(10, 0, 0); // 只操作该导航句柄对应的视图。
if (status != NavigationStatus::Applied)
    qWarning() << "Navigation failed:" << static_cast<int>(status);
```

### 5.5 翻片

在切片视图中前进或后退指定层数。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `count` | `int` | 有符号切片步数。 |

出参与返回：导航写方法同步返回 NavigationStatus，没有完成回调：Applied 已应用，Unbound 视图未绑定，Invalid 参数无效，Unsupported 视图不支持，Busy 忙，WrongThread 线程错误，Failed 执行失败。实际显示仍由更新和绘制推进。

```cpp
const auto controller = session->GetInteractionController();
const auto nav = controller ? controller->GetNavigation("slice-top-down") : nullptr;
if (!nav) return;
const auto status = nav->ScrollSlices(1); // 只操作该导航句柄对应的视图。
if (status != NavigationStatus::Applied)
    qWarning() << "Navigation failed:" << static_cast<int>(status);
```

### 5.6 适配视图

把当前数据范围适配到窗口。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `viewId` | `string` | 已有目标视图 ID。 |

出参与返回：导航写方法同步返回 NavigationStatus，没有完成回调：Applied 已应用，Unbound 视图未绑定，Invalid 参数无效，Unsupported 视图不支持，Busy 忙，WrongThread 线程错误，Failed 执行失败。实际显示仍由更新和绘制推进。

```cpp
const auto controller = session->GetInteractionController();
const auto nav = controller ? controller->GetNavigation("primary-3d") : nullptr;
if (!nav) return;
const auto status = nav->FitToView(); // 只操作该导航句柄对应的视图。
if (status != NavigationStatus::Applied)
    qWarning() << "Navigation failed:" << static_cast<int>(status);
```

### 5.7 调整导航灵敏度

修改当前视图滚轮、拖动或翻片的响应，保持其余选项不变。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `options` | `NavigationOptions` | 默认滚轮倍率 1.1、旋转 .5 度/像素、翻片每步 5 层；需要逐层翻片时主动改为 1。 |

出参与返回：导航写方法同步返回 NavigationStatus，没有完成回调：Applied 已应用，Unbound 视图未绑定，Invalid 参数无效，Unsupported 视图不支持，Busy 忙，WrongThread 线程错误，Failed 执行失败。实际显示仍由更新和绘制推进。

```cpp
const auto c = session->GetInteractionController();
const auto nav = c ? c->GetNavigation("slice-top-down") : nullptr;
if (!nav) return;
auto options = nav->GetOptions();
options.slice.countPerStep = 1; // 每次滚轮步进一层。
if (nav->SetOptions(options) != NavigationStatus::Applied)
    qWarning() << "Navigation options failed";
```

### 5.8 启用导航工具

通过 Host 请求切换目标视图的工具状态。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 已有视图。 |
| `toolMode` | `HostToolMode` | 本操作使用 Navigation。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostToolSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
request.toolMode = HostToolMode::Navigation;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Tool change failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Tool rejected";
```

### 5.9 启用模型变换工具

通过 Host 请求切换目标视图的工具状态。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetView` | `HostViewTarget` | 已有视图。 |
| `toolMode` | `HostToolMode` | 本操作使用 ModelTransform。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostToolSetRequest request;
request.targetView = HostViewTarget{"primary-3d"};
request.toolMode = HostToolMode::ModelTransform;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Tool change failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Tool rejected";
```

### 5.10 替换输入绑定

批量提交同一场景中的绑定变化，避免只改一侧产生冲突。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `bindingKey` | `string` | 由导航或所属 Feature 声明的键。 |
| `InputBinding` | `输入结构` | 触发种类、按钮、必需和禁止修饰键；Shift/Ctrl/Alt 标志为 1/2/4。 |
| `scope` | `BindingScope` | View(id) 指单视图，Slices() 指切片。 |

出参与返回：同步返回 BindingUpdateResult，没有完成回调。Applied 表示设置已应用；Conflict 表示绑定冲突，Invalid/Unsupported/Busy/Failed 分别表示配置无效、触发方式不支持、重入忙或应用失败；诊断读取 diagnostics。修改在所属线程进行。

```cpp
const auto c = session->GetInteractionController();
if (!c) return;
BindingPatch patch;
patch.Replace(std::string(NavigationBindingKeys::SliceForward),
    {InputTriggerKind::WheelForward, InputMouseButton::None, 0, 2},
    BindingScope::Slices()); // 禁止 Ctrl，给其他 Ctrl 滚轮操作留出空间。
const auto result = c->ApplyBindings(patch);
if (result.status != InputBindingStatus::Applied) qWarning() << "Binding failed";
```

### 5.11 禁用输入绑定

显式禁用一个范围内的绑定。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `bindingKey、scope` | `string、BindingScope` | 操作对应范围的既有绑定键。 |

出参与返回：同步返回 BindingUpdateResult，没有完成回调。Applied 表示设置已应用；Conflict 表示绑定冲突，Invalid/Unsupported/Busy/Failed 分别表示配置无效、触发方式不支持、重入忙或应用失败；诊断读取 diagnostics。修改在所属线程进行。

```cpp
const auto c = session->GetInteractionController();
if (!c) return;
BindingPatch patch;
patch.Disable(std::string(NavigationBindingKeys::ZoomWheelForward), BindingScope::Slices());
// 失败时不把宿主偏好记录标为已经生效。
const auto result = c->ApplyBindings(patch);
if (result.status != InputBindingStatus::Applied) qWarning() << "Binding failed";
```

### 5.12 恢复默认绑定

恢复默认值必须显式提交 UseDefault，不能只删宿主配置记录。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `bindingKey、scope` | `string、BindingScope` | 操作对应范围的既有绑定键。 |

出参与返回：同步返回 BindingUpdateResult，没有完成回调。Applied 表示设置已应用；Conflict 表示绑定冲突，Invalid/Unsupported/Busy/Failed 分别表示配置无效、触发方式不支持、重入忙或应用失败；诊断读取 diagnostics。修改在所属线程进行。

```cpp
const auto c = session->GetInteractionController();
if (!c) return;
BindingPatch patch;
patch.UseDefault(std::string(NavigationBindingKeys::ZoomWheelForward), BindingScope::Slices());
// 失败时不把宿主偏好记录标为已经生效。
const auto result = c->ApplyBindings(patch);
if (result.status != InputBindingStatus::Applied) qWarning() << "Binding failed";
```

### 5.13 保存与恢复交互设置

保存绑定覆盖和导航选项，适合宿主偏好设置。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `settings` | `InteractionSettings` | GetSettings() 的值快照，可由应用持久化。 |

出参与返回：同步返回 BindingUpdateResult，没有完成回调。Applied 表示设置已应用；Conflict 表示绑定冲突，Invalid/Unsupported/Busy/Failed 分别表示配置无效、触发方式不支持、重入忙或应用失败；诊断读取 diagnostics。修改在所属线程进行。

```cpp
const auto c = session->GetInteractionController();
if (!c) return;
const InteractionSettings saved = c->GetSettings();
const auto restored = c->LoadSettings(saved); // 恢复同一套业务配置。
if (restored.status != InputBindingStatus::Applied)
    qWarning() << "Interaction settings failed";
```

### 5.14 注入鼠标移动

宿主自行转发输入时使用 HostInjected。原生输入和注入不能对同一事件重复使用。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `viewId、kind` | `string、HostInputKind` | 构建时已设为 HostInjected 的视图。 |
| `displayPosition/displayScale` | `optional<array<double,2>>、double` | VTK 左下原点显示像素；Qt 坐标须换原点并乘设备像素比。 |

出参与返回：SendInput() 同步返回 HostInputResult，没有完成回调。isSucceeded 为处理成功，isHandled 表示命中动作，isDefaultSuppressed 表示默认处理被抑制；失败读 errorCode/message，包括 WrongThread、SessionNotReady 或 OperationFailed。

```cpp
auto* input = session->GetInputEndpoint();
if (!input) return;
const double scale = widget->devicePixelRatioF();
HostInputEvent event;
event.viewId = "primary-3d";
event.kind = HostInputKind::PointerMove;
event.displayPosition = std::array<double,2>{
    mouse.localPos().x() * scale,
    (widget->height() - 1.0 - mouse.localPos().y()) * scale};
event.displayScale = scale; // displayPosition 已是显示像素真值。
const auto result = input->SendInput(event);
if (!result.isSucceeded) qWarning() << QString::fromStdString(result.message);
```

### 5.15 注册宿主命令

让宿主命令参与公开交互控制，不派生内部路由。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `info.actionId/ownerId` | `string` | 非空，同一 owner 下不重复。 |
| `execute` | `function<ActionTriggerResult(const ActionContext&)>` | 所属线程同步执行；异步业务返回 Accepted 后沿原业务完成链继续。 |

出参与返回：RegisterAction() 同步返回 bool，无注册回调；真为完成，假为重复、参数、绑定或重入状态不合法。execute 在 Trigger 内同步调用，重入 Trigger 返回 Busy。

```cpp
const auto c = session->GetInteractionController();
if (!c) return;
ApplicationAction action;
action.info.actionId = "app.inspect"; action.info.ownerId = "qt-app";
action.execute = [](const ActionContext&) {
    qInfo() << "Inspect selection"; // 宿主命令在所属线程同步执行。
    return ActionTriggerResult{ActionTriggerStatus::Completed, 0};
};
if (!c->RegisterAction(std::move(action))) qWarning() << "Register failed";
```

### 5.16 触发宿主命令

工具栏使用和输入绑定相同的公开命令入口。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `actionId/context` | `string_view、ActionContext` | 命令 ID 和已有 viewId。 |

出参与返回：Trigger() 同步返回 ActionTriggerResult，无额外完成回调；Completed/Accepted 为同步完成/异步接纳，Unavailable/Invalid/Busy/Failed 为不可用、上下文、重入或执行失败。

```cpp
const auto c = session->GetInteractionController();
if (!c) return;
ActionContext context; context.viewId = "primary-3d";
const auto result = c->Trigger("app.inspect", context); // 区分完成和异步接纳。
if (result.status != ActionTriggerStatus::Completed && result.status != ActionTriggerStatus::Accepted)
    qWarning() << "Application action failed";
```

### 5.17 移除宿主命令

页面销毁前移除自己注册的回调。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `actionId` | `string_view` | 宿主命令 ID。 |

出参与返回：RemoveAction() 同步返回 bool，无回调。真为移除操作已执行，假为控制器或重入状态不允许；不能据真值推断之前一定存在该 ID。

```cpp
const auto c = session->GetInteractionController();
if (!c) return;
const bool removed = c->RemoveAction("app.inspect");
if (!removed) qWarning() << "Remove action rejected";
// 不再触发命令后再释放其界面依赖。
```

## 6. 数据读取与导出

链接 Host，包含 MVVCVTK/API/Data/ImageReadTypes.h、DataGraphTypes.h 及会话类型头。普通读取只返回值和几何，不提供可写体素指针；源图像描述符为 RAS、mm。

1. 先取描述符，确认修订、类型与区域。
2. 选择同步、分块或异步读取，按返回偏移续读。
3. 导出时明确视图、格式和目录，按完成回调确认。

### 6.1 读取图像元信息

用于填充数据面板和冻结后续业务输入，不复制整卷体素。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `revision` | `DataRevisionRef` | 历史查询时传精确修订；当前查询不传。 |

出参与返回：GetImageDescriptor() 或带 revision 的重载同步返回 `optional<ImageDescriptor>`，有值为可读图像，空表示无图像、不可用或修订类型不符，没有回调。dataRevision/bindingRevision 分别是数据和绑定版本。

```cpp
const auto image = session->GetImageDescriptor();
if (!image) {
    qWarning() << "No image";
    return;
}
const auto sourceRevision = image->dataRevision; // 后续业务固定到该修订。
qInfo() << "Voxel components:" << static_cast<qulonglong>(image->componentCount);
```

### 6.2 查询数据目录

列出已发布修订及其输入关系，适合数据树和来源检查。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `query` | `DataQuery` | 可省略，读取当前目录元信息。 |

出参与返回：GetDataCatalog() 同步返回 `optional<HostDataCatalog>`，空表示查询不可用；有值包含 commitId/data，没有回调。目录不保活数据载荷或资源租约。

```cpp
// 目录只复制元信息，不保活体素或网格。
const auto catalog = session->GetDataCatalog();
if (!catalog) return;
for (const auto& item : catalog->data) {
    const auto revision = item.revision;
    qInfo() << "Input dependencies:" << static_cast<qulonglong>(item.inputs.size());
}
```

### 6.3 查询场景和业务状态

用于判断视图用了哪代输入及业务运行到哪一步，查询本身不驱动更新。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target` | `HostViewTarget` | 单视图精确选择；全部视图用 GetSceneViewStates()。 |

出参与返回：GetSceneViewState()、GetStateSnapshot() 同步返回 optional 值，无值为不可用，没有回调。场景 isAvailable 表示可用性；presentationRevision 只覆盖显示参数，sceneEpoch/renderedEpoch 比较逻辑提交覆盖，不证明显示器呈现。

```cpp
// 查询不推进计算或绘制，读取当前值快照。
const auto scene = session->GetSceneViewState(HostViewTarget{"primary-3d"});
const auto state = session->GetStateSnapshot();
if (!scene || !state || !scene->isAvailable) return;
const bool caughtUp = scene->renderedEpoch >= scene->sceneEpoch;
qInfo() << "Scene caught up:" << caughtUp;
qInfo() << "Operations:" << static_cast<qulonglong>(state->operations.size());
```

### 6.4 同步读取图像区域

适合小区域数值检查，避免无意复制整卷。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `region` | `ImageReadRegion` | XYZ 相对源图像的半开区间 offset/size，显式 size 不得为 0。 |
| `maxBytes` | `size_t` | 主动设置复制预算；默认 0 不启用估算预算。 |

出参与返回：同步返回 ImageReadResult，error=None 且 state 有值为成功；NoImage/InvalidRegion/UnsupportedType/TooLarge/CopyFailed/Cancelled/ResultRetired 分别表示无图像、区域、类型、预算、复制、取消、退役问题，requiredBytes 给出所需字节数。没有完成回调。

```cpp
ImageReadRequest request;
request.region = ImageReadRegion{{0, 0, 0}, {16, 16, 16}};
request.maxBytes = 1024 * 1024; // 在分配前限制复制量。
const auto result = session->GetImageReadResult(request);
if (result.error != ImageReadError::None || !result.state) return;
const auto bytes = result.state->values; // 按 valueType 和 componentBytes 解码。
```

### 6.5 分块读取图像

大区域逐块消费，每次最多复制 8 MiB。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `request.region` | `optional<ImageReadRegion>` | 空表示整卷。 |
| `voxelOffset` | `size_t` | 区域内 X 最快的体素偏移，不是字节偏移。 |

出参与返回：返回 ImageReadChunkResult，错误同同步读取。成功用 nextVoxelOffset/isDone 续读；requiredBytes 是整个区域大小。没有回调，跨块应核验 dataRevision/bindingRevision 一致，防止混入用户后来选择的数据。

```cpp
ImageReadRequest request;
request.region = region;
const auto chunk = session->GetImageReadChunk(request, voxelOffset);
if (chunk.error != ImageReadError::None || !chunk.state) return;
voxelOffset = chunk.nextVoxelOffset; // 不按字节数自行推算。
if (chunk.isDone) qInfo() << "Region completed";
```

### 6.6 异步读取图像区域

把复制交给后台，所属线程只消费完成结果。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `request` | `ImageReadRequest` | 区域和主动预算。 |
| `onComplete` | `ImageReadCallback` | 必须非空，回调消费独立值快照。 |

出参与返回：同步返回 ImageReadAdmission：Accepted 接纳，InvalidRequest/Busy/QueueFull/Stopping/Unavailable 为请求、已有未回调读取、队列满、停止或不可用。拒绝不回调；结果在所属线程更新阶段一次交付，后台完成不直接调用 UI。回调前移除当前读取条目，可再发读取；没有公开单次取消句柄，Stop 会收口为 Cancelled。

```cpp
ImageReadRequest request;
request.region = region;
request.maxBytes = 8 * 1024 * 1024;
// 后台只复制，完成结果在所属线程更新阶段交付。
const auto a = session->StartImageRead(request, [](ImageReadResult result) {
    if (result.error != ImageReadError::None || !result.state) return;
    qInfo() << "Copied voxels:" << static_cast<qulonglong>(result.state->voxelCount);
});
if (a != ImageReadAdmission::Accepted) qWarning() << "Read rejected";
```

### 6.7 读取标签区域

先取标签描述符，再按精确修订读取，64 位标签不能通过 double 范围恢复身份。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `string` | GetLabelMapDescriptors() 返回的标签 ID。 |
| `expectedRevision` | `optional<DataRevisionRef>` | 描述符中的 dataRevision。 |
| `region/maxBytes` | `区域与预算` | 与图像读取相同的半开区域和复制预算。 |

出参与返回：同步返回 LabelMapReadResult。error=None 为成功；NotFound/VersionMismatch/GeometryMismatch/SourceMismatch/TooLarge 分别表示找不到、版本、几何、来源或预算错误；其他错误读 error。没有回调，精确整数从 values 字节解码。

```cpp
LabelMapReadRequest request;
request.id = labelDescriptor.id;
request.expectedRevision = labelDescriptor.dataRevision;
request.region = region;
request.maxBytes = 1024 * 1024;
const auto result = session->GetLabelMapReadResult(request);
if (result.error != LabelMapError::None || !result.state) return;
// 不用 descriptor.scalarRange 代替精确整数标签。
```

### 6.8 分块读取标签

逐块消费大标签图，区域和偏移规则与图像分块一致。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `request` | `LabelMapReadRequest` | 明确 ID、期望修订及区域。 |
| `voxelOffset` | `size_t` | 区域内体素偏移。 |

出参与返回：同步返回 LabelMapReadChunkResult，没有回调。None 为成功，VersionMismatch/NotFound/InvalidRegion/TooLarge/ResultRetired 为版本、对象、区域、预算或退役问题。按 nextVoxelOffset/isDone 续读。

```cpp
// 按返回体素偏移续读，不把它当字节偏移。
const auto chunk = session->GetLabelMapReadChunk(request, voxelOffset);
if (chunk.error != LabelMapError::None || !chunk.state) {
    qWarning() << "Label chunk failed";
    return;
}
voxelOffset = chunk.nextVoxelOffset;
if (chunk.isDone) qInfo() << "Labels completed";
```

### 6.9 导出 RAW

通过目标视图的数据出口导出，先确认视图模式和数据修订支持本次格式。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `outputPath` | `string` | UTF-8 输出目录，文件名由冻结数据生成。 |
| `sourceView` | `HostViewTarget` | 明确选择来源视图。 |
| `format` | `HostDataExportFormat` | 本操作显式 Raw；省略格式时体渲染默认 RAW、等值面默认 PLY。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostDataExportRequest request;
request.outputPath = directory.toUtf8().toStdString();
request.sourceView = HostViewTarget{"primary-3d"};
request.format = HostDataExportFormat::Raw;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Export failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Export rejected";
```

### 6.10 导出 PLY

通过目标视图的数据出口导出，先确认视图模式和数据修订支持本次格式。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `outputPath` | `string` | UTF-8 输出目录，文件名由冻结数据生成。 |
| `sourceView` | `HostViewTarget` | 明确选择来源视图。 |
| `format` | `HostDataExportFormat` | 本操作显式 Ply；省略格式时体渲染默认 RAW、等值面默认 PLY。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostDataExportRequest request;
request.outputPath = directory.toUtf8().toStdString();
request.sourceView = HostViewTarget{"primary-3d"};
request.format = HostDataExportFormat::Ply;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Export failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Export rejected";
```

### 6.11 导出 STL

通过目标视图的数据出口导出，先确认视图模式和数据修订支持本次格式。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `outputPath` | `string` | UTF-8 输出目录，文件名由冻结数据生成。 |
| `sourceView` | `HostViewTarget` | 明确选择来源视图。 |
| `format` | `HostDataExportFormat` | 本操作显式 Stl；省略格式时体渲染默认 RAW、等值面默认 PLY。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostDataExportRequest request;
request.outputPath = directory.toUtf8().toStdString();
request.sourceView = HostViewTarget{"primary-3d"};
request.format = HostDataExportFormat::Stl;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Export failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Export rejected";
```

### 6.12 导出 OBJ

通过目标视图的数据出口导出，先确认视图模式和数据修订支持本次格式。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `outputPath` | `string` | UTF-8 输出目录，文件名由冻结数据生成。 |
| `sourceView` | `HostViewTarget` | 明确选择来源视图。 |
| `format` | `HostDataExportFormat` | 本操作显式 Obj；省略格式时体渲染默认 RAW、等值面默认 PLY。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostDataExportRequest request;
request.outputPath = directory.toUtf8().toStdString();
request.sourceView = HostViewTarget{"primary-3d"};
request.format = HostDataExportFormat::Obj;
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Export failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Export rejected";
```

### 6.13 导出切片 PNG

按数据层导出逐层 PNG；它不等于 Qt 控件截图。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `outputDir` | `string` | UTF-8 输出目录。 |
| `sourceView` | `HostViewTarget` | 指定切片方向的视图。 |
| `angleDeg` | `optional<double>` | 可选平面内旋转角，度；省略保持当前方向。 |

出参与返回：同步返回 HostAdmission：Accepted 只表示接纳，Rejected 表示拒绝；errorCode 的 WrongThread/SessionNotReady/RequestRejected/OperationFailed 分别表示线程、会话、请求条件或处理异常。拒绝不回调；接纳后所属线程回调一次 HostResult，可能在发送函数返回前触发。完成看 isSucceeded，失败读 errorCode/message；回调可查询或再次请求。

```cpp
HostSliceExportRequest request;
request.outputDir = directory.toUtf8().toStdString();
request.sourceView = HostViewTarget{"slice-top-down"};
// 返回只说明接纳，业务成功还要看所属线程完成回调。
const auto a = session->SendRequest(std::move(request), [](HostResult r) {
    if (!r.isSucceeded) qWarning() << "Slice export failed";
});
if (a.status != HostAdmissionStatus::Accepted) qWarning() << "Export rejected";
```

## 7. ROI

正式区域使用 Host 的 MVVCVTK/API/Data/RoiTypes.h；交互编辑器需仓内打开 MVVCVTK_BUILD_ROI_EDITING、链接 MVVCVTK::RoiEditing，包含 Host/RoiEditingHostFeature.h。ROI 绑定精确源修订，源图像坐标是 RAS、mm，不能填输入 LPS 原点或把体素下标当物理点。

1. 选择源图像，以源修订构造正式定义或编辑草稿。
2. 直接事务用 SetRoi；鼠标预览通过编辑器，Commit 后才成为正式区域。
3. 把成功返回的 ROI 修订交给消费者；修改和删除带上观察到的版本。

### 7.1 创建正式盒区域

按数值创建并保存盒区域，适合已有坐标和尺寸的宿主参数面板。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `definition.source` | `DataRevisionRef` | 当前或显式源图像修订。 |
| `localToSource` | `array<double,16>` | 局部 [-1,1]³ 到源空间的仿射矩阵，半长为正；不能把单位阵当整卷。 |
| `metadata.name` | `string` | 非空，最多 256 字节。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiPrimitive box;
box.shape = RoiShape::Box;
box.localToSource = {hx,0,0,cx, 0,hy,0,cy, 0,0,hz,cz, 0,0,0,1};
RoiRequest request;
request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, box}};
request.metadata.name = "检测区域";
const auto result = session->SetRoi(request);
if (result.error != RoiError::None || !result.roi) return;
const auto roiRevision = result.roi->revision; // 保存精确修订，后续不按名称查身份。
```

### 7.2 创建球区域

使用正式 ROI 事务保存这一种几何；源修订和名称沿用创建盒区域的要求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceRevision、name` | `DataRevisionRef、string` | 宿主明确提供的来源和区域名称。 |
| `origin/radius` | `array<double,3>、double` | 源空间中心和正半径，mm；球保持零高度和默认法线。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiPrimitive p;
p.shape = RoiShape::Sphere;
p.origin = center;
p.radius = radiusMm;
RoiRequest request; request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, p}};
request.metadata.name = name;
const auto saved = session->SetRoi(request); // 几何校验和正式提交在这里完成。
if (saved.error != RoiError::None || !saved.roi) qWarning() << "ROI creation failed";
```

### 7.3 创建圆柱区域

使用正式 ROI 事务保存这一种几何；源修订和名称沿用创建盒区域的要求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceRevision、name` | `DataRevisionRef、string` | 宿主明确提供的来源和区域名称。 |
| `origin/normal` | `array<double,3>` | 源空间中心和非零柱轴。 |
| `radius/height` | `double` | 正半径和正高度，mm。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiPrimitive p;
p.shape = RoiShape::Cylinder;
p.origin = center;
p.normal = axis;
p.radius = radiusMm; p.height = heightMm;
RoiRequest request; request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, p}};
request.metadata.name = name;
const auto saved = session->SetRoi(request); // 几何校验和正式提交在这里完成。
if (saved.error != RoiError::None || !saved.roi) qWarning() << "ROI creation failed";
```

### 7.4 创建半空间区域

使用正式 ROI 事务保存这一种几何；源修订和名称沿用创建盒区域的要求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceRevision、name` | `DataRevisionRef、string` | 宿主明确提供的来源和区域名称。 |
| `origin/normal` | `array<double,3>` | 源空间平面点和非零法线；默认保留法线正侧且含平面。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiPrimitive p;
p.shape = RoiShape::HalfSpace;
p.origin = planePoint;
p.normal = planeNormal;
RoiRequest request; request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, p}};
request.metadata.name = name;
const auto saved = session->SetRoi(request); // 几何校验和正式提交在这里完成。
if (saved.error != RoiError::None || !saved.roi) qWarning() << "ROI creation failed";
```

### 7.5 创建椭球区域

使用正式 ROI 事务保存这一种几何；源修订和名称沿用创建盒区域的要求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceRevision、name` | `DataRevisionRef、string` | 宿主明确提供的来源和区域名称。 |
| `localToSource` | `array<double,16>` | 把局部单位球映射到源空间的可逆仿射矩阵。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiPrimitive p;
p.shape = RoiShape::Ellipsoid;
p.localToSource = ellipsoidToSource;
RoiRequest request; request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, p}};
request.metadata.name = name;
const auto saved = session->SetRoi(request); // 几何校验和正式提交在这里完成。
if (saved.error != RoiError::None || !saved.roi) qWarning() << "ROI creation failed";
```

### 7.6 创建椭圆柱区域

使用正式 ROI 事务保存这一种几何；源修订和名称沿用创建盒区域的要求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceRevision、name` | `DataRevisionRef、string` | 宿主明确提供的来源和区域名称。 |
| `localToSource` | `array<double,16>` | 局部单位圆到源空间的可逆仿射矩阵。 |
| `depth` | `array<double,2>` | 局部 Z 的非空递增区间。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiPrimitive p;
p.shape = RoiShape::EllipticCylinder;
p.localToSource = cylinderToSource;
p.depth = localDepth;
RoiRequest request; request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, p}};
request.metadata.name = name;
const auto saved = session->SetRoi(request); // 几何校验和正式提交在这里完成。
if (saved.error != RoiError::None || !saved.roi) qWarning() << "ROI creation failed";
```

### 7.7 创建轮廓区域

使用正式 ROI 事务保存这一种几何；源修订和名称沿用创建盒区域的要求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceRevision、name` | `DataRevisionRef、string` | 宿主明确提供的来源和区域名称。 |
| `contours` | `vector<vector<array<double,2>>>` | 局部 XY 多环，按奇偶填充。 |
| `depth/localToSource` | `区间与矩阵` | 局部 Z 厚度及局部到源空间定位。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiPrimitive p;
p.shape = RoiShape::Contour;
p.contours = rings;
p.depth = localDepth; p.localToSource = contourToSource;
RoiRequest request; request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, p}};
request.metadata.name = name;
const auto saved = session->SetRoi(request); // 几何校验和正式提交在这里完成。
if (saved.error != RoiError::None || !saved.roi) qWarning() << "ROI creation failed";
```

### 7.8 创建投影轮廓区域

使用正式 ROI 事务保存这一种几何；源修订和名称沿用创建盒区域的要求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceRevision、name` | `DataRevisionRef、string` | 宿主明确提供的来源和区域名称。 |
| `contours` | `二维多环` | 归一化设备坐标轮廓。 |
| `sourceToView/viewToClip` | `array<double,16>` | 冻结绘制时的源到视图、视图到裁剪变换。 |
| `depth` | `array<double,2>` | 正视深度区间，0≤低端<高端。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiPrimitive p;
p.shape = RoiShape::ProjectedContour;
p.contours = ndcRings;
p.sourceToView = sourceToView; p.viewToClip = viewToClip;
p.depth = viewDepth;
RoiRequest request; request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, p}};
request.metadata.name = name;
const auto saved = session->SetRoi(request); // 几何校验和正式提交在这里完成。
if (saved.error != RoiError::None || !saved.roi) qWarning() << "ROI creation failed";
```

### 7.9 创建画笔区域

使用正式 ROI 事务保存这一种几何；源修订和名称沿用创建盒区域的要求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceRevision、name` | `DataRevisionRef、string` | 宿主明确提供的来源和区域名称。 |
| `path/radius` | `vector<array<double,3>>、double` | 源物理路径和正的 mm 半径。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiPrimitive p;
p.shape = RoiShape::Brush;
p.path = sourcePath;
p.radius = radiusMm;
RoiRequest request; request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, p}};
request.metadata.name = name;
const auto saved = session->SetRoi(request); // 几何校验和正式提交在这里完成。
if (saved.error != RoiError::None || !saved.roi) qWarning() << "ROI creation failed";
```

### 7.10 创建掩码引用区域

使用正式 ROI 事务保存这一种几何；源修订和名称沿用创建盒区域的要求。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceRevision、name` | `DataRevisionRef、string` | 宿主明确提供的来源和区域名称。 |
| `mask` | `optional<DataRevisionRef>` | 同源同网格的正式掩码，非零为前景；不接受额外空间变换。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiPrimitive p;
p.shape = RoiShape::MaskReference;
p.mask = maskRevision;
RoiRequest request; request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, p}};
request.metadata.name = name;
const auto saved = session->SetRoi(request); // 几何校验和正式提交在这里完成。
if (saved.error != RoiError::None || !saved.roi) qWarning() << "ROI creation failed";
```

### 7.11 创建体素集合区域

外部离散选区通过分块构建器生成不可变载荷，不需要一次展开所有体素。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `runs` | `vector<RoiVoxelRun>` | 同一 Y/Z 行的闭 X 区间，first[0]≤last。 |
| `labels` | `vector<uint64_t>` | 可选固定标签，空集合选全部非零标签。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiVoxelBuilder builder;
builder.AppendRuns(runs); // 一次构建只用 runs 或 voxels 一种输入方式。
RoiPrimitive p; p.shape = RoiShape::VoxelSet;
p.voxels = builder.Finish();
RoiRequest request; request.definition.source = sourceRevision;
request.definition.nodes = {{RoiNodeKind::Primitive, p}};
request.metadata.name = name;
const auto saved = session->SetRoi(request);
if (saved.error != RoiError::None || !saved.roi) qWarning() << "Voxel ROI failed";
```

### 7.12 修改正式几何

对既有区域创建新的几何代次，保留旧修订供已有读者使用。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedRoi` | `optional<DataRevisionRef>` | 观察到的 ROI 修订，修改时必填。 |
| `definition` | `RoiDefinition` | 与原区域同源的完整新定义。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiRequest request;
request.action = RoiAction::SetGeometry;
request.expectedRoi = descriptor.revision;
request.definition = changedDefinition; // 保持同一 source。
request.metadata = descriptor.metadata;
const auto result = session->SetRoi(request);
if (result.error == RoiError::RevisionConflict) qWarning() << "Refresh ROI before editing";
```

### 7.13 修改区域元信息

改名、分组或描述不产生新的几何修订。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedRoi` | `optional<DataRevisionRef>` | 要修改的精确 ROI。 |
| `metadata` | `RoiMetadata` | 名称非空；name/group/description 上限为 256/256/4096 字节。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiRequest request;
request.action = RoiAction::SetMetadata;
request.expectedRoi = descriptor.revision;
request.metadata = descriptor.metadata;
request.metadata.name = newName; // 不在这个动作里附带 geometry。
const auto result = session->SetRoi(request);
if (result.error != RoiError::None) qWarning() << "ROI metadata failed";
```

### 7.14 复制区域

以一个正式 ROI 为来源创建独立目录项。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `copyFrom` | `optional<DataRevisionRef>` | 来源 ROI 修订。 |
| `metadata` | `RoiMetadata` | 新区域名称等信息；不填 expectedRoi。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiRequest request;
request.action = RoiAction::Copy;
request.copyFrom = descriptor.revision;
request.metadata = descriptor.metadata;
request.metadata.name = copyName;
// 失败时不覆盖宿主已保存的正式区域引用。
const auto result = session->SetRoi(request);
if (result.error != RoiError::None || !result.roi) qWarning() << "ROI copy failed";
```

### 7.15 删除区域

从正式目录移除区域，不删除源图像文件。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedRoi` | `optional<DataRevisionRef>` | 要删除的修订。 |
| `expectedCatalogRevision` | `optional<DataBindingRevision>` | 可选目录版本检查，取 descriptor.catalogRevision。 |

出参与返回：同步返回 RoiResult，没有完成回调。error=None 表示事务成功，创建或改几何时从可选 roi 描述符读 revision，删除只按 error 判断；WrongThread/InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/CommitFailed 分别表示线程、请求、几何、来源、版本或提交错误，诊断在 message。

```cpp
RoiRequest request;
request.action = RoiAction::Remove;
request.expectedRoi = descriptor.revision;
request.expectedCatalogRevision = descriptor.catalogRevision;
const auto result = session->SetRoi(request); // 不附带几何或元信息。
if (result.error != RoiError::None) qWarning() << "ROI remove failed";
```

### 7.16 区域并集

对同源正式 ROI 做布尔组合；表达式节点的二元引用只能指向更早节点，最后节点为根。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `source/target` | `DataRevisionRef、optional<DataRevisionRef>` | 源修订和要更新的目标 ROI。 |
| `operands` | `vector<DataRevisionRef>` | 同源参与区域。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request;
request.source = sourceRevision;
request.target = targetRoi;
RoiBooleanParams p; p.operation = RoiBooleanOperation::Union;
p.operands = operands;
request.params = p;
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI boolean failed";
```

### 7.17 区域交集

对同源正式 ROI 做布尔组合；表达式节点的二元引用只能指向更早节点，最后节点为根。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `source/target` | `DataRevisionRef、optional<DataRevisionRef>` | 源修订和要更新的目标 ROI。 |
| `operands` | `vector<DataRevisionRef>` | 同源参与区域。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request;
request.source = sourceRevision;
request.target = targetRoi;
RoiBooleanParams p; p.operation = RoiBooleanOperation::Intersection;
p.operands = operands;
request.params = p;
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI boolean failed";
```

### 7.18 区域差集

对同源正式 ROI 做布尔组合；表达式节点的二元引用只能指向更早节点，最后节点为根。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `source/target` | `DataRevisionRef、optional<DataRevisionRef>` | 源修订和要更新的目标 ROI。 |
| `operands` | `vector<DataRevisionRef>` | 同源参与区域。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request;
request.source = sourceRevision;
request.target = targetRoi;
RoiBooleanParams p; p.operation = RoiBooleanOperation::Difference;
p.operands = operands;
request.params = p;
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI boolean failed";
```

### 7.19 膨胀区域

在源网格上执行这一种形态操作，适合清理或调整选区。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target/source` | `DataRevisionRef` | 同源目标及其来源。 |
| `radius/unit` | `double、RoiRadiusUnit` | 半径；显式选择 Millimetres 或 Voxels，避免单位混用。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiMorphologyParams p; p.operation = RoiMorphologyOperation::Dilate;
p.radius = radius; p.unit = RoiRadiusUnit::Millimetres; // 本例按 mm。
request.params = p;
request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI morphology failed";
```

### 7.20 腐蚀区域

在源网格上执行这一种形态操作，适合清理或调整选区。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target/source` | `DataRevisionRef` | 同源目标及其来源。 |
| `radius/unit` | `double、RoiRadiusUnit` | 半径；显式选择 Millimetres 或 Voxels，避免单位混用。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiMorphologyParams p; p.operation = RoiMorphologyOperation::Erode;
p.radius = radius; p.unit = RoiRadiusUnit::Millimetres; // 本例按 mm。
request.params = p;
request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI morphology failed";
```

### 7.21 区域开运算

在源网格上执行这一种形态操作，适合清理或调整选区。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target/source` | `DataRevisionRef` | 同源目标及其来源。 |
| `radius/unit` | `double、RoiRadiusUnit` | 半径；显式选择 Millimetres 或 Voxels，避免单位混用。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiMorphologyParams p; p.operation = RoiMorphologyOperation::Open;
p.radius = radius; p.unit = RoiRadiusUnit::Millimetres; // 本例按 mm。
request.params = p;
request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI morphology failed";
```

### 7.22 区域闭运算

在源网格上执行这一种形态操作，适合清理或调整选区。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target/source` | `DataRevisionRef` | 同源目标及其来源。 |
| `radius/unit` | `double、RoiRadiusUnit` | 半径；显式选择 Millimetres 或 Voxels，避免单位混用。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiMorphologyParams p; p.operation = RoiMorphologyOperation::Close;
p.radius = radius; p.unit = RoiRadiusUnit::Millimetres; // 本例按 mm。
request.params = p;
request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI morphology failed";
```

### 7.23 移除小连通域

minimumVolume 为 mm³。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `connectivity` | `RoiConnectivity` | Faces 或 Full。 |
| `minimumVolume` | `double` | 最小保留体积，mm³。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiComponentParams p; p.operation = RoiComponentOperation::RemoveSmall;
p.connectivity = RoiConnectivity::Faces;
p.minimumVolume = minimumVolumeMm3;
request.params = p;
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI components failed";
```


### 7.24 保留最大连通域

按连通性保留最大区域。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `connectivity` | `RoiConnectivity` | Faces 或 Full。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiComponentParams p; p.operation = RoiComponentOperation::KeepLargest;
p.connectivity = RoiConnectivity::Faces;
request.params = p;
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI components failed";
```


### 7.25 保留选定连通域

labels 指运算连通域编号。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `connectivity` | `RoiConnectivity` | Faces 或 Full。 |
| `labels` | `vector<uint32_t>` | 要保留的连通域编号。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiComponentParams p; p.operation = RoiComponentOperation::KeepSelected;
p.connectivity = RoiConnectivity::Faces;
p.labels = selectedLabels;
request.params = p;
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI components failed";
```


### 7.26 拆分连通域

结果 rois 可能有多个正式区域。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `connectivity` | `RoiConnectivity` | Faces 或 Full。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiComponentParams p; p.operation = RoiComponentOperation::Split;
p.connectivity = RoiConnectivity::Faces;
request.params = p;
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI components failed";
```


### 7.27 填充孔洞

maximumHoleVolume 为 mm³，0 表示不限制孔洞大小。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `connectivity` | `RoiConnectivity` | Faces 或 Full。 |
| `maximumHoleVolume` | `double` | 孔洞体积上限，mm³；0 表示不限制。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiComponentParams p; p.operation = RoiComponentOperation::FillHoles;
p.connectivity = RoiConnectivity::Faces;
p.maximumHoleVolume = maximumHoleVolumeMm3;
request.params = p;
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI components failed";
```


### 7.28 阈值区域

按存储灰度闭区间筛选区域，普通阈值模式种子必须为空。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `range` | `array<double,2>` | 原始存储灰度区间。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiThresholdParams p; p.mode = RoiThresholdMode::Threshold;
p.range = grayRange;
// Threshold 保持 seeds 为空。
request.params = p;
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI threshold failed";
```


### 7.29 种子连通生长

从源物理坐标种子出发，在灰度区间内连通生长。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `range` | `array<double,2>` | 原始存储灰度区间。 |
| `seeds/connectivity` | `物理点集合、RoiConnectivity` | 生长模式的种子和 Faces/Full 连通性。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiThresholdParams p; p.mode = RoiThresholdMode::SeedGrow;
p.range = grayRange;
p.seeds = sourceSeeds; // 源物理坐标，不能传体素下标。
request.params = p;
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI threshold failed";
```

### 7.30 区域取反

可指定同源 domain，限定取反的范围。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `source/target` | `DataRevisionRef` | 目标区域及其源修订。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request;
request.source = sourceRevision; request.target = targetRoi;
request.params = RoiInvertParams{};
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI operation failed";
```

### 7.31 清空区域

保留区域事务流程，将结果几何清空。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `source/target` | `DataRevisionRef` | 目标区域及其源修订。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request;
request.source = sourceRevision; request.target = targetRoi;
request.params = RoiClearParams{};
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI operation failed";
```

### 7.32 变换区域

把区域映射到目标源；不能精确保留的几何需要主动选择体素化。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceToTarget/targetSource` | `矩阵、optional<DataRevisionRef>` | 源到目标仿射矩阵及目标数据修订。 |
| `rasterize` | `bool` | 掩码等不能精确保留的情况显式设 true。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiTransformParams p; p.sourceToTarget = transform;
p.targetSource = targetSource; p.rasterize = true; // 明确选择目标体素中心重采样。
request.params = p;
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "ROI transform failed";
```

### 7.33 由表面内部创建区域

用闭合正式网格及其到源空间变换定义内部区域；开放表面不能可靠定义内部。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `mesh/meshToSource` | `DataRevisionRef、矩阵` | 网格精确修订及源空间定位。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiMeshParams p; p.mesh = meshRevision; p.meshToSource = meshToSource;
p.mode = RoiMeshMode::Interior;
request.params = p;
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "Mesh ROI failed";
```


### 7.34 由表面层创建区域

用正式网格及其到源空间变换定义区域。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `mesh/meshToSource` | `DataRevisionRef、矩阵` | 网格精确修订及源空间定位。 |
| `inward/outward/hasOrientation` | `double、bool` | 向内、向外厚度 mm；有方向时区分内外并要求闭合，无方向取最大厚度。 |

出参与返回：SetRoiOperation() 同步返回 RoiOperationResult，没有完成回调；None 表示成功，正式区域在 rois。InvalidRequest/InvalidGeometry/SourceMismatch/RevisionConflict/TooLarge/Cancelled/DeadlineExceeded/CommitFailed 分别为参数、几何、来源、版本、资源、取消、截止时间或提交问题。长运算可改走编辑器异步候选。

```cpp
RoiOperationRequest request; request.source = sourceRevision;
request.target = targetRoi;
RoiMeshParams p; p.mesh = meshRevision; p.meshToSource = meshToSource;
p.mode = RoiMeshMode::Layer;
p.inward = inwardMm; p.outward = outwardMm;
p.hasOrientation = hasOrientation; // 有方向内外层要求闭合；无方向时按最大层厚双侧处理。
request.params = p;
// 只有正式事务成功后才使用 rois 中的新修订。
const auto result = session->SetRoiOperation(request);
if (result.error != RoiError::None) qWarning() << "Mesh ROI failed";
```

### 7.35 挂载交互编辑器

鼠标编辑需要可交互的参考视图，目标视图应包含参考视图并显示同一源修订。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `referenceView/targetViews` | `HostViewTarget、HostViewTargets` | 必须是已有视图，且引用同一输入。 |

出参与返回：AttachFeature() 同步返回 bool，无完成回调。真为挂载成功；假为依赖、视图或注册失败。功能用 shared_ptr 持有，后续动作复用此实例。

```cpp
RoiEditingConfig config;
config.referenceView = HostViewTarget{"primary-3d"};
config.targetViews.viewIds = {"primary-3d", "slice-top-down"};
roiEditor = std::make_shared<RoiEditingHostFeature>(config);
// 挂载失败停止后续操作，成功句柄由宿主持有。
if (!session->AttachFeature(roiEditor)) qWarning() << "ROI editor attach failed";
```

### 7.36 开始新建或编辑草稿

Begin 接受 Create 或 SetGeometry 草稿，草稿还不是正式数据。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `draft` | `RoiRequest` | 完整定义和元信息；编辑已有几何时填 expectedRoi。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiRequest draft;
draft.action = RoiAction::SetGeometry;
draft.expectedRoi = descriptor.revision;
draft.definition = descriptor.definition;
draft.metadata = descriptor.metadata;
RoiEditingRequest request; request.action = RoiEditingAction::Begin;
request.draft = draft;
// 草稿与正式提交分开，Pending 时继续等待 Host 更新。
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI begin failed";
```

### 7.37 修改草稿工具

用数值面板改变当前几何，也可选择替换、相加、相减或相交。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `primitive` | `optional<RoiPrimitive>` | 新的有效几何。 |
| `selection/output` | `RoiSelectionMode、RoiOutputMode` | Replace/Add/Subtract/Intersect 与 CreateNew/UpdateTarget；换形状时显式设 selection。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::SetDraft;
request.primitive = changedPrimitive;
request.selection = RoiSelectionMode::Replace; // 换形状时明确组合方式。
request.output = RoiOutputMode::CreateNew;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "Draft update failed";
```

### 7.38 绘制切片轮廓

在参考切片内绘制多边形轮廓。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `drawing` | `RoiDrawingMode` | 本操作使用 SliceContour；先有有效草稿。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::BeginDrawing;
request.drawing = RoiDrawingMode::SliceContour;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI drawing rejected";
// 鼠标输入仍通过已配置的 Host 输入链进入。
```

### 7.39 绘制切片画笔

在切片中刷选区域。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `drawing` | `RoiDrawingMode` | 本操作使用 SliceBrush；先有有效草稿。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::BeginDrawing;
request.drawing = RoiDrawingMode::SliceBrush;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI drawing rejected";
// 鼠标输入仍通过已配置的 Host 输入链进入。
```

### 7.40 绘制投影轮廓

在当前投影中圈选，创建时冻结投影。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `drawing` | `RoiDrawingMode` | 本操作使用 ProjectedContour；先有有效草稿。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::BeginDrawing;
request.drawing = RoiDrawingMode::ProjectedContour;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI drawing rejected";
// 鼠标输入仍通过已配置的 Host 输入链进入。
```

### 7.41 使用可见表面画笔

按当前可见场景的画笔交互建立路径。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `drawing` | `RoiDrawingMode` | 本操作使用 VisibleBrush；先有有效草稿。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::BeginDrawing;
request.drawing = RoiDrawingMode::VisibleBrush;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI drawing rejected";
// 鼠标输入仍通过已配置的 Host 输入链进入。
```

### 7.42 拾取生长种子

给已有阈值生长运算选择种子，须先建立该运算。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `drawing` | `RoiDrawingMode` | 本操作使用 Seed；先有有效草稿。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::BeginDrawing;
request.drawing = RoiDrawingMode::Seed;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI drawing rejected";
// 鼠标输入仍通过已配置的 Host 输入链进入。
```

### 7.43 绘制切片矩形

在参考切片内画矩形。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `drawing` | `RoiDrawingMode` | 本操作使用 SliceRectangle；先有有效草稿。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::BeginDrawing;
request.drawing = RoiDrawingMode::SliceRectangle;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI drawing rejected";
// 鼠标输入仍通过已配置的 Host 输入链进入。
```

### 7.44 绘制切片椭圆

在参考切片内画椭圆。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `drawing` | `RoiDrawingMode` | 本操作使用 SliceEllipse；先有有效草稿。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::BeginDrawing;
request.drawing = RoiDrawingMode::SliceEllipse;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI drawing rejected";
// 鼠标输入仍通过已配置的 Host 输入链进入。
```

### 7.45 绘制切片圆

在参考切片内画圆。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `drawing` | `RoiDrawingMode` | 本操作使用 SliceCircle；先有有效草稿。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::BeginDrawing;
request.drawing = RoiDrawingMode::SliceCircle;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI drawing rejected";
// 鼠标输入仍通过已配置的 Host 输入链进入。
```

### 7.46 编辑控制点

编辑已有工具的可用控制点，工具必须支持控制点。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `drawing` | `RoiDrawingMode` | 本操作使用 EditControlPoint；先有有效草稿。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::BeginDrawing;
request.drawing = RoiDrawingMode::EditControlPoint;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI drawing rejected";
// 鼠标输入仍通过已配置的 Host 输入链进入。
```

### 7.47 结束绘制

退出当前绘制交互，后续仍须明确提交草稿。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `RoiEditingAction` | EndDrawing |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::EndDrawing;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI action failed";
const auto state = roiEditor->GetState(); // 读取动作后的草稿状态。
```

### 7.48 应用当前选择

把当前选择候选纳入草稿；计算中不能应用。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `RoiEditingAction` | ApplySelection |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::ApplySelection;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI action failed";
const auto state = roiEditor->GetState(); // 读取动作后的草稿状态。
```

### 7.49 撤销草稿编辑

回退草稿编辑，不改已经保存的历史修订。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `RoiEditingAction` | Undo |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::Undo;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI action failed";
const auto state = roiEditor->GetState(); // 读取动作后的草稿状态。
```

### 7.50 重做草稿编辑

重做草稿编辑，用状态 undoCount/redoCount 控制按钮。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `RoiEditingAction` | Redo |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::Redo;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI action failed";
const auto state = roiEditor->GetState(); // 读取动作后的草稿状态。
```

### 7.51 取消草稿

放弃未提交草稿，保留正式区域。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `RoiEditingAction` | Cancel |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::Cancel;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI action failed";
const auto state = roiEditor->GetState(); // 读取动作后的草稿状态。
```

### 7.52 取消待保存动作

取消等待中的保存意图，避免后台完成后继续保存。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `RoiEditingAction` | CancelSave |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::CancelSave;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI action failed";
const auto state = roiEditor->GetState(); // 读取动作后的草稿状态。
```

### 7.53 提交草稿

只有 Commit 成功才取得正式 ROI；后台计算未结束时继续观察状态。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `RoiEditingAction` | Commit；需要改输出方式时主动提供 output。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request; request.action = RoiEditingAction::Commit;
const auto result = roiEditor->SendRequest(request);
if (result.error == RoiError::None && result.roi) {
    const auto saved = result.roi->revision; // 可交给表面、裁切等消费者。
} else if (result.error == RoiError::Pending) {
    const auto state = roiEditor->GetState(); // 后续 Host 更新后看 isSaving/committedRoi。
} else qWarning() << "ROI commit failed";
```

### 7.54 建立运算候选

通过编辑器后台计算区域运算，适合较大的阈值、形态或连通域操作。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `operation` | `RoiOperationRequest` | 和前面的正式运算相同参数，绑定当前草稿源。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::BeginOperation;
request.operation = operation;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::Pending && result.error != RoiError::None)
    qWarning() << "ROI operation rejected";
// 后续更新后读取 GetState().operation，不阻塞界面等待。
```

### 7.55 更新运算候选

通过编辑器后台计算区域运算，适合较大的阈值、形态或连通域操作。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `operation` | `RoiOperationRequest` | 和前面的正式运算相同参数，绑定当前草稿源。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::SetOperation;
request.operation = operation;
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::Pending && result.error != RoiError::None)
    qWarning() << "ROI operation rejected";
// 后续更新后读取 GetState().operation，不阻塞界面等待。
```

### 7.56 应用运算候选

只处理当前区域运算候选；应用之前应等待 isComputing=false 并检查 operation.error。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `RoiEditingAction` | ApplyOperation |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::ApplyOperation;
// 草稿与正式提交分开，Pending 时继续等待 Host 更新。
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI operation failed";
const auto state = roiEditor->GetState();
```

### 7.57 取消运算候选

只处理当前区域运算候选；应用之前应等待 isComputing=false 并检查 operation.error。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `RoiEditingAction` | CancelOperation |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::CancelOperation;
// 草稿与正式提交分开，Pending 时继续等待 Host 更新。
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "ROI operation failed";
const auto state = roiEditor->GetState();
```

### 7.58 显示正式区域

可显示已有区域而不把它重新保存。几何保存和显示准备有独立状态。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `roi/isVisible` | `DataRevisionRef、bool` | 要显示的正式修订及可见性。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::ShowRoi;
request.roi = roiRevision; request.isVisible = true;
const auto result = roiEditor->SendRequest(request);
const auto state = roiEditor->GetState();
if (state.displayStatus == RoiDisplayStatus::Failed) qWarning() << "ROI display failed";
// 显示失败不自动表示正式几何保存失败。
```

### 7.59 读取正式区域

从正式修订读取定义和目录元信息，不从草稿反推。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `revision` | `DataRevisionRef` | 成功事务返回的 ROI 修订。 |

出参与返回：GetRoiDescriptor() 同步返回 `optional<RoiDescriptor>`，空为不可读；GetRoiDescriptors() 返回目录值集合，无回调。revision 是请求几何版本，currentRevision 是当前几何版本。

```cpp
const auto descriptor = session->GetRoiDescriptor(roiRevision);
if (!descriptor) {
    qWarning() << "ROI unavailable";
    return;
}
const auto source = descriptor->definition.source; // 消费者必须同源。
const auto list = session->GetRoiDescriptors();
```

### 7.60 控制草稿显示

只改显示意图，正式保存状态独立。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `isVisible` | `optional<bool>` | 必须显式提供。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiEditingRequest request;
request.action = RoiEditingAction::SetVisible; request.isVisible = isVisible;
const auto result = roiEditor->SendRequest(request); // 显示与保存分开判断。
if (result.error != RoiError::None) qWarning() << "Visibility failed";
const auto state = roiEditor->GetState();
```

### 7.61 修改草稿元信息

提交前更新名称、分组和描述。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `draft.metadata` | `RoiMetadata` | 名称非空的完整元信息。 |

出参与返回：同步返回 RoiResult，编辑器没有用户完成回调。None 表示当前动作完成，Pending 表示仍要等待 Host 更新；WrongThread/Unavailable/InvalidRequest/SourceMismatch 分别表示线程错误、编辑器不可用、动作字段不符、来源不符。后续用 GetState() 读取状态。

```cpp
RoiRequest metadataOnly; metadataOnly.metadata = metadata;
RoiEditingRequest request;
request.action = RoiEditingAction::SetMetadata;
request.draft = metadataOnly; // 只更新当前草稿元信息。
const auto result = roiEditor->SendRequest(request);
if (result.error != RoiError::None) qWarning() << "Metadata failed";
```

## 8. 裁切

安装头为 MVVCVTK/API/Features/OrthogonalCrop/Host/CropHostFeature.h 和同组件 OrthogonalCropTypes.h，链接 MVVCVTK::OrthogonalCrop。输入模型坐标先于模型到世界显示变换；图像模型长度为 mm，网格沿自身模型单位。

1. 挂载 CropHostFeature，以精确源修订创建文档。
2. 选择盒、平面、圆柱或球工具，或直接提交数学操作。
3. 等待编辑和预览提交，按需要构建正式结果。
4. 返回源或关闭文档，收口完成后再卸载功能。

### 8.1 挂载裁切功能

一个实例管理裁切文档和操作，后续形状按钮复用该句柄。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `crop` | `shared_ptr<CropHostFeature>` | 所属线程创建并持有。 |

出参与返回：AttachFeature() 同步返回 bool，无完成回调；真为挂载完成，假为依赖、重复挂载或拓扑状态不允许。

```cpp
crop = std::make_shared<CropHostFeature>();
// 挂载失败停止后续操作，成功句柄由宿主持有。
if (!session->AttachFeature(crop)) {
    qWarning() << "Crop attach failed";
    return;
}
const auto state = crop->GetState();
```

### 8.2 创建裁切文档

文档固定来源，不会跟随用户后来选择另一卷数据。最多 32 个活动文档，可用 GetDocuments() 列出；超过时返回 ResourceLimit。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target` | `optional<CropHostTarget>` | inputBinding、referenceView、targetViews 均明确；主卷用 primaryVolumeBinding。 |
| `sourceRevision/requestId` | `DataRevisionRef、CropRequestId` | 源修订；请求 ID 用 CreateRequestId()。 |

出参与返回：同步返回 CropDocumentAdmission，isAccepted 说明接纳，失败读 failureReason；SourceMismatch/StateVersionMismatch/Busy/PreviewNotReady/ResourceLimit 为来源、版本、忙、就绪或文档数量问题。本例不传回调，通过 Host 更新后 GetDocumentOutcome(documentId,requestId) 读取 Succeeded/Failed/Cancelled，Queued 尚未完成；此路径不提供同步完成保证。

```cpp
CropHostTarget target;
target.inputBinding = std::string(primaryVolumeBinding);
target.referenceView = HostViewTarget{"primary-3d"};
target.targetViews.viewIds = {"primary-3d", "slice-top-down"};
CropDocumentRequest request;
request.action = CropDocumentAction::CreateDocument;
request.target = target; request.sourceRevision = sourceRevision;
request.requestId = CropHostFeature::CreateRequestId();
const auto a = crop->SendRequest(request); // documentId/expectedRevision 保持 0。
if (!a.isAccepted) qWarning() << static_cast<int>(a.failureReason);
```

### 8.3 盒裁切

创建文档成功且源已显示后，选择这一种交互工具。数值面板使用下面的同形状数学操作字段。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target` | `CropHostTarget` | 当前文档建立的同一目标。 |
| `boxToInputModelMatrix` | `CropMatrixDouble16Array` | 标准 [-1,1]³ 到输入模型的有效仿射矩阵。 |

出参与返回：SendRequest(CropHostRequest) 同步返回 bool；真为该工具命令已接纳，假为目标、来源、状态或字段不符。工具动作不传完成回调，状态从 GetState()/GetHistory() 读取；它不表示正式结果已构建。

```cpp
CropHostRequest request;
request.action = CropHostAction::Box;
request.target = target; // 不用工具动作代替创建或切换文档。
if (!crop->SendRequest(request)) qWarning() << "Crop widget rejected";
const auto state = crop->GetState();
```

### 8.4 平面裁切

创建文档成功且源已显示后，选择这一种交互工具。数值面板使用下面的同形状数学操作字段。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target` | `CropHostTarget` | 当前文档建立的同一目标。 |
| `planeCenterInInputModel/planeNormalInInputModel` | `CropVectorDouble3Array` | 输入模型中的平面点、非零法线，内部为法线正侧。 |

出参与返回：SendRequest(CropHostRequest) 同步返回 bool；真为该工具命令已接纳，假为目标、来源、状态或字段不符。工具动作不传完成回调，状态从 GetState()/GetHistory() 读取；它不表示正式结果已构建。

```cpp
CropHostRequest request;
request.action = CropHostAction::Plane;
request.target = target; // 不用工具动作代替创建或切换文档。
if (!crop->SendRequest(request)) qWarning() << "Crop widget rejected";
const auto state = crop->GetState();
```

### 8.5 圆柱裁切

创建文档成功且源已显示后，选择这一种交互工具。数值面板使用下面的同形状数学操作字段。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target` | `CropHostTarget` | 当前文档建立的同一目标。 |
| `centerInInputModel/axisInInputModel` | `CropVectorDouble3Array` | 模型中心与非零柱轴。 |
| `radius/height` | `double` | 正半径和高度，模型长度单位。 |

出参与返回：SendRequest(CropHostRequest) 同步返回 bool；真为该工具命令已接纳，假为目标、来源、状态或字段不符。工具动作不传完成回调，状态从 GetState()/GetHistory() 读取；它不表示正式结果已构建。

```cpp
CropHostRequest request;
request.action = CropHostAction::Cylinder;
request.target = target; // 不用工具动作代替创建或切换文档。
if (!crop->SendRequest(request)) qWarning() << "Crop widget rejected";
const auto state = crop->GetState();
```

### 8.6 球裁切

创建文档成功且源已显示后，选择这一种交互工具。数值面板使用下面的同形状数学操作字段。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target` | `CropHostTarget` | 当前文档建立的同一目标。 |
| `centerInInputModel/radius` | `三分量、double` | 模型中心及正半径。 |

出参与返回：SendRequest(CropHostRequest) 同步返回 bool；真为该工具命令已接纳，假为目标、来源、状态或字段不符。工具动作不传完成回调，状态从 GetState()/GetHistory() 读取；它不表示正式结果已构建。

```cpp
CropHostRequest request;
request.action = CropHostAction::Sphere;
request.target = target; // 不用工具动作代替创建或切换文档。
if (!crop->SendRequest(request)) qWarning() << "Crop widget rejected";
const auto state = crop->GetState();
```

### 8.7 保留内部

保留盒内部、平面法线正侧或曲面内部。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target/removalMode` | `CropHostTarget、CropRemovalMode` | 当前目标和本小节模式。 |

出参与返回：SendRequest(CropHostRequest) 同步返回 bool；真为该工具命令已接纳，假为目标、来源、状态或字段不符。工具动作不传完成回调，状态从 GetState()/GetHistory() 读取；它不表示正式结果已构建。

```cpp
CropHostRequest request;
request.action = CropHostAction::Mode;
request.target = target;
request.removalMode = CropRemovalMode::KeepInside;
// 工具命令不表示已经构建正式裁切数据。
if (!crop->SendRequest(request)) qWarning() << "Crop removal mode rejected";
```

### 8.8 移除内部

移除定义的内部，保留其余区域。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target/removalMode` | `CropHostTarget、CropRemovalMode` | 当前目标和本小节模式。 |

出参与返回：SendRequest(CropHostRequest) 同步返回 bool；真为该工具命令已接纳，假为目标、来源、状态或字段不符。工具动作不传完成回调，状态从 GetState()/GetHistory() 读取；它不表示正式结果已构建。

```cpp
CropHostRequest request;
request.action = CropHostAction::Mode;
request.target = target;
request.removalMode = CropRemovalMode::RemoveInside;
// 工具命令不表示已经构建正式裁切数据。
if (!crop->SendRequest(request)) qWarning() << "Crop removal mode rejected";
```

### 8.9 仅定位工具

只移动工具，不生成或改写裁切历史。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target/removalMode` | `CropHostTarget、CropRemovalMode` | 当前目标和本小节模式。 |

出参与返回：SendRequest(CropHostRequest) 同步返回 bool；真为该工具命令已接纳，假为目标、来源、状态或字段不符。工具动作不传完成回调，状态从 GetState()/GetHistory() 读取；它不表示正式结果已构建。

```cpp
CropHostRequest request;
request.action = CropHostAction::Mode;
request.target = target;
request.removalMode = CropRemovalMode::None;
// 工具命令不表示已经构建正式裁切数据。
if (!crop->SendRequest(request)) qWarning() << "Crop removal mode rejected";
```

### 8.10 数值盒操作

从数值面板追加这一个数学节点，避免用鼠标事件模拟几何参数。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `documentId/nodeId/expectedRevision` | `ID 与版本` | 最新历史的文档、明确父节点和 stateRevision。 |
| `boxToInputModelMatrix` | `4×4 矩阵` | 局部标准盒到输入模型；须可逆、有限。 |

出参与返回：同步返回 CropEditAdmission，isAccepted 只说明接纳。拒绝从 failureReason 读取：BadInput/NodeNotFound/StateVersionMismatch/Busy/PreviewNotReady 分别表示几何、节点、版本、正在处理或视图就绪问题。本例不传回调，通过后续 GetOutcome(documentId,requestId) 读取 Queued/Succeeded/Failed/Cancelled。

```cpp
const auto h = crop->GetHistory();
CropEditRequest request;
request.documentId = h.documentId; request.nodeId = h.requestedHead;
request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
request.kind = CropEditKind::Append;
request.operation.geometryType = CropShape::Box;
request.operation.boxToInputModelMatrix = boxToInputModel;
const auto a = crop->SendRequest(request); // 其余几何字段保持有效默认值。
if (!a.isAccepted) qWarning() << static_cast<int>(a.failureReason);
```

### 8.11 数值平面操作

从数值面板追加这一个数学节点，避免用鼠标事件模拟几何参数。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `documentId/nodeId/expectedRevision` | `ID 与版本` | 最新历史的文档、明确父节点和 stateRevision。 |
| `center/normal` | `三分量` | 输入模型坐标点和非零法线。 |

出参与返回：同步返回 CropEditAdmission，isAccepted 只说明接纳。拒绝从 failureReason 读取：BadInput/NodeNotFound/StateVersionMismatch/Busy/PreviewNotReady 分别表示几何、节点、版本、正在处理或视图就绪问题。本例不传回调，通过后续 GetOutcome(documentId,requestId) 读取 Queued/Succeeded/Failed/Cancelled。

```cpp
const auto h = crop->GetHistory();
CropEditRequest request;
request.documentId = h.documentId; request.nodeId = h.requestedHead;
request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
request.kind = CropEditKind::Append;
request.operation.geometryType = CropShape::Plane;
request.operation.planeCenterInInputModel = center;
request.operation.planeNormalInInputModel = normal;
const auto a = crop->SendRequest(request); // 其余几何字段保持有效默认值。
if (!a.isAccepted) qWarning() << static_cast<int>(a.failureReason);
```

### 8.12 数值圆柱操作

从数值面板追加这一个数学节点，避免用鼠标事件模拟几何参数。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `documentId/nodeId/expectedRevision` | `ID 与版本` | 最新历史的文档、明确父节点和 stateRevision。 |
| `center/axis/radius/height` | `三分量及 double` | 模型中心、非零轴和正长度。 |

出参与返回：同步返回 CropEditAdmission，isAccepted 只说明接纳。拒绝从 failureReason 读取：BadInput/NodeNotFound/StateVersionMismatch/Busy/PreviewNotReady 分别表示几何、节点、版本、正在处理或视图就绪问题。本例不传回调，通过后续 GetOutcome(documentId,requestId) 读取 Queued/Succeeded/Failed/Cancelled。

```cpp
const auto h = crop->GetHistory();
CropEditRequest request;
request.documentId = h.documentId; request.nodeId = h.requestedHead;
request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
request.kind = CropEditKind::Append;
request.operation.geometryType = CropShape::Cylinder;
request.operation.centerInInputModel = center;
request.operation.axisInInputModel = axis;
request.operation.radius = radius; request.operation.height = height;
const auto a = crop->SendRequest(request); // 其余几何字段保持有效默认值。
if (!a.isAccepted) qWarning() << static_cast<int>(a.failureReason);
```

### 8.13 数值球操作

从数值面板追加这一个数学节点，避免用鼠标事件模拟几何参数。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `documentId/nodeId/expectedRevision` | `ID 与版本` | 最新历史的文档、明确父节点和 stateRevision。 |
| `center/radius` | `三分量及 double` | 模型中心和正半径。 |

出参与返回：同步返回 CropEditAdmission，isAccepted 只说明接纳。拒绝从 failureReason 读取：BadInput/NodeNotFound/StateVersionMismatch/Busy/PreviewNotReady 分别表示几何、节点、版本、正在处理或视图就绪问题。本例不传回调，通过后续 GetOutcome(documentId,requestId) 读取 Queued/Succeeded/Failed/Cancelled。

```cpp
const auto h = crop->GetHistory();
CropEditRequest request;
request.documentId = h.documentId; request.nodeId = h.requestedHead;
request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
request.kind = CropEditKind::Append;
request.operation.geometryType = CropShape::Sphere;
request.operation.centerInInputModel = center;
request.operation.radius = radius;
const auto a = crop->SendRequest(request); // 其余几何字段保持有效默认值。
if (!a.isAccepted) qWarning() << static_cast<int>(a.failureReason);
```

### 8.14 选择历史节点

回退、前进和切分支都选择明确节点，不以节点数量当身份。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `nodeId` | `CropNodeId` | GetHistory() 中的目标节点。 |
| `expectedRevision` | `uint64_t` | 最新 stateRevision。 |

出参与返回：同步返回 CropEditAdmission，isAccepted 只说明接纳。拒绝从 failureReason 读取：BadInput/NodeNotFound/StateVersionMismatch/Busy/PreviewNotReady 分别表示几何、节点、版本、正在处理或视图就绪问题。本例不传回调，通过后续 GetOutcome(documentId,requestId) 读取 Queued/Succeeded/Failed/Cancelled。

```cpp
const auto h = crop->GetHistory();
CropEditRequest request;
request.documentId = h.documentId; request.nodeId = selectedNode;
request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
request.kind = CropEditKind::Select;
// 接纳后继续从 outcome 获取编辑或文档终态。
const auto a = crop->SendRequest(request);
if (!a.isAccepted) qWarning() << "History selection rejected";
```

### 8.15 替换历史操作

Replace 指定一个已有操作节点，以新数学参数产生对应替换路径。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `nodeId/operation` | `CropNodeId、CropOpItem` | 目标节点及完整有效的新操作。 |

出参与返回：同步返回 CropEditAdmission，isAccepted 只说明接纳。拒绝从 failureReason 读取：BadInput/NodeNotFound/StateVersionMismatch/Busy/PreviewNotReady 分别表示几何、节点、版本、正在处理或视图就绪问题。本例不传回调，通过后续 GetOutcome(documentId,requestId) 读取 Queued/Succeeded/Failed/Cancelled。

```cpp
const auto h = crop->GetHistory();
CropEditRequest request;
request.documentId = h.documentId; request.nodeId = selectedNode;
request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
request.kind = CropEditKind::Replace; request.operation = changedOperation;
// 接纳后继续从 outcome 获取编辑或文档终态。
const auto a = crop->SendRequest(request);
if (!a.isAccepted) qWarning() << "Replace rejected";
```

### 8.16 裁剪历史分支

先查询删除影响，再提交 Prune，已发布结果可能阻塞删除。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `prune` | `CropPruneRequest` | 明确节点集合、范围和回退策略。 |

出参与返回：同步返回 CropEditAdmission，isAccepted 只说明接纳。拒绝从 failureReason 读取：BadInput/NodeNotFound/StateVersionMismatch/Busy/PreviewNotReady 分别表示几何、节点、版本、正在处理或视图就绪问题。本例不传回调，通过后续 GetOutcome(documentId,requestId) 读取 Queued/Succeeded/Failed/Cancelled。

```cpp
const auto h = crop->GetHistory();
const auto impact = crop->GetPruneImpact(h.documentId, prune);
if (impact.failureReason != CropFailure::None || !impact.blockers.empty()) return;
CropEditRequest request;
request.documentId = h.documentId; request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
request.kind = CropEditKind::Prune; request.prune = prune;
// 接纳后继续从 outcome 获取编辑或文档终态。
const auto a = crop->SendRequest(request);
if (!a.isAccepted) qWarning() << "Prune rejected";
```

### 8.17 构建正式裁切结果

先等编辑终态，核对 requestedHead/appliedHead/renderedHead；预览没跟上时不要把接纳当构建完成。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `documentId/nodeId/expectedRevision` | `ID 与版本` | 明确构建节点和最新历史状态。 |
| `options` | `CropBuildOptions` | 必要时设置 RAM 预算、正 meshTolerance、maxDepth 1～128；默认容差 .05。 |

出参与返回：同步返回 CropBuildAdmission，isAccepted 只说明接纳；StateVersionMismatch/Busy/PreviewNotReady/NoCropOperations/LowRam/EmptyResult 为版本、忙、预览、缺操作、资源或空结果问题。构建完成回调由所属线程的 Host tick 兑现，结果是 CropBuildResult；isSucceeded 才表示正式构建成功，失败读 failureReason/message，正式输出在 outputRevision。

```cpp
const auto h = crop->GetHistory();
CropBuildRequest request;
request.documentId = h.documentId; request.nodeId = h.appliedHead;
request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
const auto a = crop->SendRequest(request, [](CropBuildResult result) {
    if (!result.isSucceeded) qWarning() << QString::fromStdString(result.message);
    // 成功后保存 result.outputRevision，不能用当前视图代替结果身份。
});
if (!a.isAccepted) qWarning() << static_cast<int>(a.failureReason);
```

### 8.18 由公共 ROI 构建裁切结果

显式使用同源 ROI 时，从文档根构建，不消费交互历史。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `inputRoi` | `optional<DataRevisionRef>` | 同源正式 ROI 修订。 |
| `nodeId` | `CropNodeId` | 必须为文档 rootNodeId。 |

出参与返回：同步返回 CropBuildAdmission，isAccepted 只说明接纳；StateVersionMismatch/Busy/PreviewNotReady/NoCropOperations/LowRam/EmptyResult 为版本、忙、预览、缺操作、资源或空结果问题。构建完成回调由所属线程的 Host tick 兑现，结果是 CropBuildResult；isSucceeded 才表示正式构建成功，失败读 failureReason/message，正式输出在 outputRevision。

```cpp
const auto h = crop->GetHistory();
CropBuildRequest request;
request.documentId = h.documentId; request.nodeId = h.rootNodeId;
request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
request.inputRoi = roiRevision; // 明确采用公共 ROI。
const auto a = crop->SendRequest(request, [](CropBuildResult r) {
    if (!r.isSucceeded) qWarning() << "ROI crop failed";
});
if (!a.isAccepted) qWarning() << "ROI crop rejected";
```

### 8.19 保存裁切为公共 ROI

只保存当前历史公式，不构建派生图像或网格。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `target/roiMetadata` | `CropHostTarget、RoiMetadata` | 当前目标及非空名称。 |
| `expectedCatalogRevision` | `DataBindingRevision` | 观察到的 ROI 目录版本。 |

出参与返回：工具请求同步返回 bool，拒绝不回调；SaveRoi 必须提供回调，已接纳回调会在发送函数返回前同步执行，所属线程。结果 CropBuildResult 的 isSucceeded 表示保存成功，recipeRevision 是正式 ROI；失败读 failureReason/message。

```cpp
CropHostRequest request;
request.action = CropHostAction::SaveRoi;
request.target = target; request.roiMetadata = metadata;
request.expectedCatalogRevision = catalogRevision;
const bool accepted = crop->SendRequest(request, [](CropBuildResult r) {
    if (!r.isSucceeded) qWarning() << "Save crop ROI failed";
    // r.recipeRevision 是 ROI；本动作没有派生图像输出。
});
if (!accepted) qWarning() << "Save crop ROI rejected";
```

### 8.20 激活已有文档

多文档切换时保持文档原来的源修订，不能用工具动作偷偷重绑源。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `documentId/expectedRevision` | `ID 与版本` | 已有文档及其最新 stateRevision。 |
| `target` | `CropHostTarget` | 明确新的参考和目标视图，不附 sourceRevision。 |

出参与返回：同步返回 CropDocumentAdmission，isAccepted 说明接纳，失败读 failureReason；SourceMismatch/StateVersionMismatch/Busy/PreviewNotReady/ResourceLimit 为来源、版本、忙、就绪或文档数量问题。本例不传回调，通过 Host 更新后 GetDocumentOutcome(documentId,requestId) 读取 Succeeded/Failed/Cancelled，Queued 尚未完成；此路径不提供同步完成保证。

```cpp
const auto h = crop->GetHistory(documentId);
CropDocumentRequest request;
request.action = CropDocumentAction::ActivateDocument;
request.documentId = documentId; request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
request.target = target;
// 接纳后继续从 outcome 获取编辑或文档终态。
const auto a = crop->SendRequest(request);
if (!a.isAccepted) qWarning() << "Document activation rejected";
```

### 8.21 返回源数据

收口预览或构建并恢复源数据，保留文档历史。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `documentId/expectedRevision/requestId` | `ID 与版本` | 最新文档和状态，不附 target/sourceRevision。 |

出参与返回：同步返回 CropDocumentAdmission，isAccepted 说明接纳，失败读 failureReason；SourceMismatch/StateVersionMismatch/Busy/PreviewNotReady/ResourceLimit 为来源、版本、忙、就绪或文档数量问题。本例不传回调，通过 Host 更新后 GetDocumentOutcome(documentId,requestId) 读取 Succeeded/Failed/Cancelled，Queued 尚未完成；此路径不提供同步完成保证。

```cpp
const auto h = crop->GetHistory();
CropDocumentRequest request;
request.action = CropDocumentAction::ReturnToSource;
request.documentId = h.documentId; request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
// 接纳后继续从 outcome 获取编辑或文档终态。
const auto a = crop->SendRequest(request);
if (!a.isAccepted) qWarning() << "Document action rejected";
```

### 8.22 关闭裁切文档

收口任务、绑定及结果释放后关闭文档，必要时查看 blockers。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `documentId/expectedRevision/requestId` | `ID 与版本` | 最新文档和状态，不附 target/sourceRevision。 |

出参与返回：同步返回 CropDocumentAdmission，isAccepted 说明接纳，失败读 failureReason；SourceMismatch/StateVersionMismatch/Busy/PreviewNotReady/ResourceLimit 为来源、版本、忙、就绪或文档数量问题。本例不传回调，通过 Host 更新后 GetDocumentOutcome(documentId,requestId) 读取 Succeeded/Failed/Cancelled，Queued 尚未完成；此路径不提供同步完成保证。

```cpp
const auto h = crop->GetHistory();
CropDocumentRequest request;
request.action = CropDocumentAction::CloseDocument;
request.documentId = h.documentId; request.expectedRevision = h.stateRevision;
request.requestId = CropHostFeature::CreateRequestId();
// 接纳后继续从 outcome 获取编辑或文档终态。
const auto a = crop->SendRequest(request);
if (!a.isAccepted) qWarning() << "Document action rejected";
```

### 8.23 退出交互工具

只结束当前工具；正式文档和结果收口应使用返回源或关闭文档。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `CropHostAction` | Exit，不附 target。 |

出参与返回：SendRequest(CropHostRequest) 同步返回 bool；真为该工具命令已接纳，假为目标、来源、状态或字段不符。工具动作不传完成回调，状态从 GetState()/GetHistory() 读取；它不表示正式结果已构建。

```cpp
CropHostRequest request;
request.action = CropHostAction::Exit;
// 工具命令不表示已经构建正式裁切数据。
if (!crop->SendRequest(request)) qWarning() << "Crop exit rejected";
const auto state = crop->GetState();
if (state.isPublishing) qWarning() << "Crop publication still active";
```

### 8.24 注册外部网格

公开 SetPolyData 注册网格，之后仍需明确源修订建立文档。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `polyData` | `vtkSmartPointer<vtkPolyData>` | 有效非空网格，本动作不附 target。 |

出参与返回：SendRequest(CropHostRequest) 同步返回 bool，无回调；真为网格注册事务成功，假为数据或发布失败，不自动切换已有文档源。

```cpp
CropHostRequest request;
request.action = CropHostAction::SetPolyData;
request.polyData = mesh; // 宿主文件读取入口提供。
const bool accepted = crop->SendRequest(request);
if (!accepted) qWarning() << "Mesh registration failed";
```

### 8.25 清除外部网格输入

清除外部输入绑定，与关闭文档不同。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `CropHostAction` | ClearPolyData，不附 target 或 polyData。 |

出参与返回：同步返回 bool，无回调；真为绑定清理完成，假为生命周期或提交失败；正式结果仍按文档和消费者关系处理。

```cpp
CropHostRequest request;
request.action = CropHostAction::ClearPolyData;
const bool cleared = crop->SendRequest(request); // 不代替 CloseDocument。
if (!cleared) qWarning() << "Clear mesh failed";
const auto state = crop->GetState();
```

## 9. 表面测定与提取

仓内打开 MVVCVTK_BUILD_SURFACE_DETERMINATION，链接 MVVCVTK::SurfaceDetermination，包含 Host/SurfaceDeterminationHostFeature.h、Host/SurfaceDeterminationHostTypes.h、SurfaceRecipe.h。它生成正式网格、对象和质量记录，与 Host 等值面显示不同。

1. 挂载功能并固定 sourceVolume、可选同源 analysisRoi。
2. 选 GlobalAutomatic 配方、用途和对象选择方式，再启动。
3. 正式结果按 dataRevision 读取，预览用 GetPreviewSnapshot()。
4. 取消或清理结果后卸载，显示失败和业务发布分开判断。

### 9.1 挂载表面功能

宿主保持一个实例，后续测定和对象显示复用它。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `SurfaceDeterminationConfig` | `构造配置` | 需要时主动设置默认 Start 参数和显示可见性。 |

出参与返回：AttachFeature() 同步返回 bool，没有完成回调；真为完成，假为依赖或拓扑条件不满足。

```cpp
surface = std::make_shared<SurfaceDeterminationHostFeature>();
// 挂载失败停止后续操作，成功句柄由宿主持有。
if (!session->AttachFeature(surface)) {
    qWarning() << "Surface attach failed";
    return;
}
const auto state = surface->GetState();
```

### 9.2 自动估计阈值并测定

GlobalAutomatic 是当前唯一计算方法。阈值和材料范围均省略时，从背景与材料峰估计，不从窗口显示阈值偷取参数。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `sourceVolume/analysisRoi` | `optional<DataRevisionRef>` | 源修订和可选同源区域。 |
| `targetViews/modelUnit` | `HostViewTargets、string` | 显示目标及几何长度单位，Host 图像填 mm；空单位表示未知。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
SurfaceDeterminationStartParams params;
params.sourceVolume = sourceRevision;
params.analysisRoi = analysisRoi;
params.targetViews.viewIds = {"primary-3d"}; params.modelUnit = "mm";
params.method = SurfaceDeterminationMethod::GlobalAutomatic;
// 接纳和计算完成分开，不能把 Accepted 当成已发布。
const auto a = surface->SendRequest(SurfaceStartRequest{params}, [](SurfaceDeterminationResult r) {
    if (r.status != SurfaceResultStatus::Succeeded) qWarning() << QString::fromStdString(r.message);
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Surface rejected";
```

### 9.3 指定初始阈值

仍使用 GlobalAutomatic，但由业务明确给出材料阈值和背景/材料范围。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `initialIsoValue/materialRange` | `optional<double>、optional<array<double,2>>` | 有限且 背景<阈值<材料；没有显式范围时取正式源范围。 |
| `seedFraction/sharpCornerAngleDeg` | `double` | 需要调配方时才设置：分别为 (0,1)、(0,180] 度。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
params.initialIsoValue = isoValue;
params.materialRange = materialRange;
const auto error = SurfaceRecipeCodec::GetError(params);
if (!error.empty()) { qWarning() << QString::fromStdString(error); return; }
// 接纳和计算完成分开，不能把 Accepted 当成已发布。
const auto a = surface->SendRequest(SurfaceStartRequest{params}, [](SurfaceDeterminationResult r) {
    if (r.status != SurfaceResultStatus::Succeeded) qWarning() << "Surface failed";
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Surface rejected";
```

### 9.4 执行预览

执行同一表面算法但保留临时候选，不把预览当成正式 DataGraph 修订。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `params` | `SurfaceDeterminationStartParams` | 已填写源、配方及显示目标。 |
| `purpose` | `SurfaceTaskPurpose` | Preview |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
params.purpose = SurfaceTaskPurpose::Preview;
// 接纳和计算完成分开，不能把 Accepted 当成已发布。
const auto a = surface->SendRequest(SurfaceStartRequest{params},
    [weak = std::weak_ptr<SurfaceDeterminationHostFeature>(surface)](SurfaceDeterminationResult result) {
        const auto surface = weak.lock();
        if (!surface || result.status != SurfaceResultStatus::Succeeded) return;
        const auto snapshot = surface->GetPreviewSnapshot();
        if (!snapshot) qWarning() << "Surface snapshot unavailable";
    });
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Surface rejected";
```

### 9.5 执行正式测定

发布可供对象读取、测量和下游使用的正式表面。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `params` | `SurfaceDeterminationStartParams` | 已填写源、配方及显示目标。 |
| `purpose` | `SurfaceTaskPurpose` | Determine |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
params.purpose = SurfaceTaskPurpose::Determine;
// 接纳和计算完成分开，不能把 Accepted 当成已发布。
const auto a = surface->SendRequest(SurfaceStartRequest{params},
    [weak = std::weak_ptr<SurfaceDeterminationHostFeature>(surface)](SurfaceDeterminationResult result) {
        const auto surface = weak.lock();
        if (!surface || result.status != SurfaceResultStatus::Succeeded) return;
        const auto snapshot = surface->GetSurfaceSnapshot(result.dataRevision);
        if (!snapshot) qWarning() << "Surface snapshot unavailable";
    });
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Surface rejected";
```

### 9.6 提取全部对象

保留符合配方的所有对象。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `componentSelection` | `SurfaceComponentSelection` | All |
| `minimumObjectVoxels` | `uint64_t` | 需要过滤小对象时主动设置最小体素数。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
params.componentSelection = SurfaceComponentSelection::All;
params.minimumObjectVoxels = minimumVoxels;

// 接纳和计算完成分开，不能把 Accepted 当成已发布。
const auto a = surface->SendRequest(SurfaceStartRequest{params}, [](SurfaceDeterminationResult r) {
    if (r.status != SurfaceResultStatus::Succeeded) qWarning() << "Extraction failed";
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Extraction rejected";
```

### 9.7 提取最大对象

选择最大连通对象。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `componentSelection` | `SurfaceComponentSelection` | Largest |
| `minimumObjectVoxels` | `uint64_t` | 需要过滤小对象时主动设置最小体素数。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
params.componentSelection = SurfaceComponentSelection::Largest;
params.minimumObjectVoxels = minimumVoxels;

// 接纳和计算完成分开，不能把 Accepted 当成已发布。
const auto a = surface->SendRequest(SurfaceStartRequest{params}, [](SurfaceDeterminationResult r) {
    if (r.status != SurfaceResultStatus::Succeeded) qWarning() << "Extraction failed";
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Extraction rejected";
```

### 9.8 提取种子对象

选择源模型点所在的对象，必须提供有限 seedModelPoint。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `componentSelection` | `SurfaceComponentSelection` | Seeded |
| `minimumObjectVoxels` | `uint64_t` | 需要过滤小对象时主动设置最小体素数。 |
| `seedModelPoint` | `optional<array<double,3>>` | 种子模型坐标。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
params.componentSelection = SurfaceComponentSelection::Seeded;
params.minimumObjectVoxels = minimumVoxels;
params.seedModelPoint = seedModelPoint; // 模型坐标，不是显示像素。
const auto a = surface->SendRequest(SurfaceStartRequest{params}, [](SurfaceDeterminationResult r) {
    if (r.status != SurfaceResultStatus::Succeeded) qWarning() << "Extraction failed";
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Extraction rejected";
```

### 9.9 从父表面重新选区

复用已验证完整父表面，只重选面片，不重新测量；源和配方要与父代一致。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `parentGeneration` | `optional<DataRevisionRef>` | 正式父表面 dataRevision。 |
| `analysisRoi` | `optional<DataRevisionRef>` | 新选区必须同源；不能借父代改变测量配方。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
params.parentGeneration = parentRevision;
params.analysisRoi = roiRevision;
params.purpose = SurfaceTaskPurpose::Determine;
// 接纳和计算完成分开，不能把 Accepted 当成已发布。
const auto a = surface->SendRequest(SurfaceStartRequest{params}, [](SurfaceDeterminationResult r) {
    if (r.status != SurfaceResultStatus::Succeeded) qWarning() << "Parent selection failed";
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Parent request rejected";
```

### 9.10 读取对象目录

目录与几何分页分开，避免每次刷新表格复制全部网格。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `generation` | `DataRevisionRef` | 完成结果的正式 dataRevision。 |
| `offset/count` | `uint64_t、size_t` | 对象偏移和数量，每页最多 4096。 |

出参与返回：GetSurfaceSnapshot() 返回 `shared_ptr<const SurfaceGenerationSnapshot>`，空表示结果不可读；GetObjectPage() 返回 `optional<SurfaceObjectPage>`，空表示无目录或参数不合法。没有回调，使用 next 续读。面积、体积要结合 areaValidity/volumeValidity，缺失可选值不能写成 0。

```cpp
const auto snapshot = surface->GetSurfaceSnapshot(generation);
if (!snapshot) return;
const auto page = snapshot->GetObjectPage(offset, 256);
if (!page) return;
offset = page->next; // 下一页从返回位置继续。
qInfo() << "Objects:" << static_cast<qulonglong>(page->total);
```

### 9.11 读取对象点

读取对象点记录及局部质量标志。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `objectIndex/offset/count` | `uint32_t、uint64_t、size_t` | 明确对象和页内偏移，每页最多 65536。 |

出参与返回：GetObjectPoints() 同步返回对应的 optional 页结构，空表示对象、范围或后备数据不可读，没有回调。成功读取 points 或 triangles，并按 next 续读。

```cpp
const auto snapshot = surface->GetSurfaceSnapshot(generation);
if (!snapshot) return;
const auto page = snapshot->GetObjectPoints(objectIndex, offset, 1024);
if (!page) { qWarning() << "Geometry page unavailable"; return; }
offset = page->next; // 不自行猜测下一页偏移。
```

### 9.12 读取对象三角形

读取正式三角形；顶点编号是全局正式点编号，不是页内编号。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `objectIndex/offset/count` | `uint32_t、uint64_t、size_t` | 明确对象和页内偏移，每页最多 65536。 |

出参与返回：GetObjectTriangles() 同步返回对应的 optional 页结构，空表示对象、范围或后备数据不可读，没有回调。成功读取 points 或 triangles，并按 next 续读。

```cpp
const auto snapshot = surface->GetSurfaceSnapshot(generation);
if (!snapshot) return;
const auto page = snapshot->GetObjectTriangles(objectIndex, offset, 1024);
if (!page) { qWarning() << "Geometry page unavailable"; return; }
offset = page->next; // 不自行猜测下一页偏移。
```

### 9.13 选择或隔离表面对象

显示已测定对象，不重新计算整个表面；对象 0 是合法编号。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `generation/objectIndex` | `DataRevisionRef、optional<uint32_t>` | 明确结果和对象；编号留空表示清除选择。 |
| `isIsolated/targetViews` | `bool、HostViewTargets` | 隔离方式及显示目标。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
SurfaceObjectSelectionRequest request;
request.generation = generation; request.objectIndex = objectIndex;
request.isIsolated = true; request.targetViews.viewIds = {"primary-3d"};
// 接纳和计算完成分开，不能把 Accepted 当成已发布。
const auto a = surface->SendRequest(request, [](SurfaceDeterminationResult r) {
    if (r.status != SurfaceResultStatus::Succeeded) qWarning() << "Object display failed";
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Object rejected";
```

### 9.14 启用表面对象拾取

把对象选择接到 Host 输入链。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `isEnabled` | `bool` | 鼠标拾取开关。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
const auto a = surface->SendRequest(SurfaceObjectPickModeRequest{true}, [](SurfaceDeterminationResult r) {
    if (r.status == SurfaceResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Surface action rejected";
const auto state = surface->GetState(); // 计算进度与显示准备分别读取。
```

### 9.15 设置表面可见性

只改变结果显示，不删除正式结果。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `isVisible` | `bool` | 是否显示叠加结果。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
const auto a = surface->SendRequest(SurfaceVisibilityRequest{isVisible}, [](SurfaceDeterminationResult r) {
    if (r.status == SurfaceResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Surface action rejected";
const auto state = surface->GetState(); // 计算进度与显示准备分别读取。
```

### 9.16 取消表面任务

请求协作取消并等待完成状态。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetRequestId` | `uint64_t` | 0 选当前最新请求，非零须匹配正在执行请求。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
const auto a = surface->SendRequest(SurfaceStopRequest{targetRequestId}, [](SurfaceDeterminationResult r) {
    if (r.status == SurfaceResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Surface action rejected";
const auto state = surface->GetState(); // 计算进度与显示准备分别读取。
```

### 9.17 清除表面预览

放弃预览，不把旧正式结果误认为新预览。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `SurfaceClearPreviewRequest` | 只清临时候选。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
const auto a = surface->SendRequest(SurfaceClearPreviewRequest{}, [](SurfaceDeterminationResult r) {
    if (r.status == SurfaceResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Surface action rejected";
const auto state = surface->GetState(); // 计算进度与显示准备分别读取。
```

### 9.18 清除正式表面

先收口计算和读者，再清理正式结果。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `SurfaceClearRequest` | 正式结果清理动作。 |

出参与返回：同步返回 SurfaceDeterminationAdmission；Accepted 才等待完成，InvalidRequest/UnsupportedRoi/Busy/Stopping/Unavailable/UnsupportedMode 分别表示请求、区域支持、忙、停止、可用性或模式问题。非空回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/Cancelled/Failed 为成功、取消、失败；failureReason 的 InvalidSource/InvalidGeometry/UnsupportedScalar/ThresholdUnreliable/NoSurface/SourceChanged/BudgetExceeded/DisplayFailed/PublishFailed 分别说明源、几何、标量支持、阈值、空表面、源改变、预算、显示或发布问题。发布另看 isPublished。

```cpp
const auto a = surface->SendRequest(SurfaceClearRequest{}, [](SurfaceDeterminationResult r) {
    if (r.status == SurfaceResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != SurfaceAdmissionStatus::Accepted) qWarning() << "Surface action rejected";
const auto state = surface->GetState(); // 计算进度与显示准备分别读取。
```

### 9.19 检查结果与显示有效性

历史表面可读不代表仍可测量；显示准备也可能晚于正式发布。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `generation` | `DataRevisionRef` | 要检查的正式表面修订。 |

出参与返回：GetResultValidity() 同步返回 SurfaceRestoreState，Current/Historical/MissingInput/IncompatibleRecipe/MissingResult 分别表示当前、历史、缺输入、配方不兼容、缺结果。按 canDisplay/canRecompute/canMeasure 决定用途；GetState() 的显示失败单独报告，无回调。

```cpp
// 能读历史结果不表示仍能作为当前测量输入。
const auto validity = surface->GetResultValidity(generation);
if (!validity.canMeasure) qWarning() << "Surface is not a current measurement input";
const auto state = surface->GetState();
if (state.hasDisplayFailure) qWarning() << QString::fromStdString(state.displayMessage);
qInfo() << "Computation progress:" << state.progress01;
```

## 10. 零件分割与编辑

仓内打开 MVVCVTK_BUILD_PART_SEGMENTATION，链接 MVVCVTK::PartSegmentation，包含 Host/PartSegmentationHostFeature.h 和 Host/PartSegmentationHostTypes.h。Start 从当前输入解析源；编辑必须绑定精确标签和目录版本。

1. 挂载功能，按原始灰度阈值分割。
2. 读取正式零件目录，按 PartBindingRef 选择对象。
3. 每次编辑先计算候选，再确认或丢弃。
4. 取消任务、清理结果后卸载。

### 10.1 挂载零件功能

由宿主持有一个实例，显示产品不等同于正式表面测定网格。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `PartSegmentationConfig` | `构造配置` | 需要时主动设置工作预算、编辑超时和历史上限。 |

出参与返回：AttachFeature() 同步返回 bool，无回调；真表示挂载完成，假表示依赖、重复挂载或拓扑失败。

```cpp
parts = std::make_shared<PartSegmentationHostFeature>();
// 挂载失败停止后续操作，成功句柄由宿主持有。
if (!session->AttachFeature(parts)) {
    qWarning() << "Part feature attach failed";
    return;
}
const auto state = parts->GetState();
```

### 10.2 启动分割

按当前源图像阈值生成标签、对象表和结果集，阈值不假定归一化。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `threshold/minPartVoxels` | `double、uint64_t` | 原始灰度阈值与最小对象体素数。 |
| `targetViews` | `HostViewTargets` | 明确叠加显示目标。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。

```cpp
PartSegmentationStartParams params;
params.threshold = threshold; params.minPartVoxels = minVoxels;
params.targetViews.viewIds = {"primary-3d", "slice-top-down"};
PartSegmentationRequest request; request.action = PartSegmentationAction::Start;
request.start = params;
// 是否接纳和业务终态分开处理。
const auto a = parts->SendRequest(request, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::Succeeded
        && r.status != PartResultStatus::SucceededWithDisplayFailure) qWarning() << "Segmentation failed";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Segmentation rejected";
```

### 10.3 读取零件目录

结果含源修订、对象身份和目录版本，不能用数组下标代替身份。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `parts` | `功能句柄` | 已完成分割的同一实例。 |

出参与返回：GetPartSetSnapshot() 同步返回 `shared_ptr<const PartSetSnapshot>`，空表示无可读结果，无回调。检查 isStale；对象体积为 physicalVolumeMM3，范围为含端点 voxelExtent。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog || catalog->isStale) return;
for (const auto& part : catalog->parts) {
    const auto binding = part.binding; // 对象身份和结果版本一起使用。
    qInfo() << "Volume mm3:" << part.metrics.physicalVolumeMM3;
}
```

### 10.4 修改零件状态

改名、审核、选择、显隐或透明度通过同一状态补丁提交。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `part/patch` | `PartBindingRef、PartStatePatch` | 用快照对象身份，只填主动改变的字段。 |
| `expectedCatalogRevision` | `uint64_t` | 来自同一目录快照。 |

出参与返回：SetPartState() 同步返回 PartMutationResult，无回调。Succeeded 为成功，InvalidRequest/NotFound/StaleReference/RevisionConflict/Busy/Unavailable/DisplayFailed 为参数、对象、引用、版本、忙、可用性或显示失败。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
PartStatePatch patch;
patch.name = newName; patch.isSelected = true;
// 同一快照的对象引用和目录版本参与条件更新。
const auto result = parts->SetPartState(binding, patch, catalog->catalogRevision);
if (result.status != PartMutationStatus::Succeeded) qWarning() << "Part state failed";
```

### 10.5 画笔涂抹

计算这一种标签编辑的候选；保护区由 scope.editRoi/protectionRoi/protectedParts 限定。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedLabelMap/expectedCatalogRevision` | `DataRevisionRef、uint64_t` | 分别冻结当前标签和目录版本。 |
| `sourcePoints/radiusMM` | `物理点集合、double` | 源物理坐标和 mm 半径；可指定切片平面。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。 SendEditRequest 拒绝不保留回调；计算只产生 PreviewReady，不发布正式标签。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
PartEditRequest request;
request.expectedLabelMap = parts->GetState().labelMap;
request.expectedCatalogRevision = catalog->catalogRevision;
PartBrushEdit edit;
edit.target = binding;
edit.radiusMM = radiusMm; edit.sourcePoints = sourcePoints;
request.operation = edit;
// 这里只生成候选，PreviewReady 后仍要用户确认。
const auto a = parts->SendEditRequest(request, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::PreviewReady) qWarning() << "Edit did not produce preview";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Edit rejected";
```

### 10.6 画笔擦除

计算这一种标签编辑的候选；保护区由 scope.editRoi/protectionRoi/protectedParts 限定。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedLabelMap/expectedCatalogRevision` | `DataRevisionRef、uint64_t` | 分别冻结当前标签和目录版本。 |
| `sourcePoints/radiusMM` | `物理点集合、double` | 源物理坐标和 mm 半径。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。 SendEditRequest 拒绝不保留回调；计算只产生 PreviewReady，不发布正式标签。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
PartEditRequest request;
request.expectedLabelMap = parts->GetState().labelMap;
request.expectedCatalogRevision = catalog->catalogRevision;
PartBrushEdit edit;
edit.target = binding; edit.isErase = true;
edit.radiusMM = radiusMm; edit.sourcePoints = sourcePoints;
request.operation = edit;
// 这里只生成候选，PreviewReady 后仍要用户确认。
const auto a = parts->SendEditRequest(request, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::PreviewReady) qWarning() << "Edit did not produce preview";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Edit rejected";
```

### 10.7 填充零件

计算这一种标签编辑的候选；保护区由 scope.editRoi/protectionRoi/protectedParts 限定。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedLabelMap/expectedCatalogRevision` | `DataRevisionRef、uint64_t` | 分别冻结当前标签和目录版本。 |
| `seed` | `array<int,3>` | 源网格下标，不是物理点。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。 SendEditRequest 拒绝不保留回调；计算只产生 PreviewReady，不发布正式标签。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
PartEditRequest request;
request.expectedLabelMap = parts->GetState().labelMap;
request.expectedCatalogRevision = catalog->catalogRevision;
PartFillEdit edit;
edit.target = binding; edit.seed = seed;
request.operation = edit;
// 这里只生成候选，PreviewReady 后仍要用户确认。
const auto a = parts->SendEditRequest(request, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::PreviewReady) qWarning() << "Edit did not produce preview";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Edit rejected";
```

### 10.8 去除小岛

计算这一种标签编辑的候选；保护区由 scope.editRoi/protectionRoi/protectedParts 限定。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedLabelMap/expectedCatalogRevision` | `DataRevisionRef、uint64_t` | 分别冻结当前标签和目录版本。 |
| `minIslandVoxels` | `uint64_t` | 最小保留岛体素数。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。 SendEditRequest 拒绝不保留回调；计算只产生 PreviewReady，不发布正式标签。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
PartEditRequest request;
request.expectedLabelMap = parts->GetState().labelMap;
request.expectedCatalogRevision = catalog->catalogRevision;
PartIslandEdit edit;
edit.target = binding; edit.minIslandVoxels = minIslandVoxels;
request.operation = edit;
// 这里只生成候选，PreviewReady 后仍要用户确认。
const auto a = parts->SendEditRequest(request, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::PreviewReady) qWarning() << "Edit did not produce preview";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Edit rejected";
```

### 10.9 区域生长

计算这一种标签编辑的候选；保护区由 scope.editRoi/protectionRoi/protectedParts 限定。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedLabelMap/expectedCatalogRevision` | `DataRevisionRef、uint64_t` | 分别冻结当前标签和目录版本。 |
| `seeds/minimum/maximum` | `下标集合与 double` | 源网格种子和灰度区间；覆盖其他零件须明确 overwriteParts。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。 SendEditRequest 拒绝不保留回调；计算只产生 PreviewReady，不发布正式标签。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
PartEditRequest request;
request.expectedLabelMap = parts->GetState().labelMap;
request.expectedCatalogRevision = catalog->catalogRevision;
PartGrowEdit edit;
edit.target = binding; edit.seeds = seeds;
edit.minimum = minimum; edit.maximum = maximum;
request.operation = edit;
// 这里只生成候选，PreviewReady 后仍要用户确认。
const auto a = parts->SendEditRequest(request, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::PreviewReady) qWarning() << "Edit did not produce preview";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Edit rejected";
```

### 10.10 拆分零件

计算这一种标签编辑的候选；保护区由 scope.editRoi/protectionRoi/protectedParts 限定。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedLabelMap/expectedCatalogRevision` | `DataRevisionRef、uint64_t` | 分别冻结当前标签和目录版本。 |
| `seeds/barriers` | `vector<PartSplitSeed>、vector<PartBarrier>` | 子对象序号须连续 1..N；屏障是沿 axis 正向的邻接边。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。 SendEditRequest 拒绝不保留回调；计算只产生 PreviewReady，不发布正式标签。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
PartEditRequest request;
request.expectedLabelMap = parts->GetState().labelMap;
request.expectedCatalogRevision = catalog->catalogRevision;
PartSplitEdit edit;
edit.target = binding; edit.seeds = splitSeeds; edit.barriers = barriers;
request.operation = edit;
// 这里只生成候选，PreviewReady 后仍要用户确认。
const auto a = parts->SendEditRequest(request, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::PreviewReady) qWarning() << "Edit did not produce preview";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Edit rejected";
```

### 10.11 合并零件

计算这一种标签编辑的候选；保护区由 scope.editRoi/protectionRoi/protectedParts 限定。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedLabelMap/expectedCatalogRevision` | `DataRevisionRef、uint64_t` | 分别冻结当前标签和目录版本。 |
| `parts` | `vector<PartBindingRef>` | 当前结果中要合并的精确对象引用。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。 SendEditRequest 拒绝不保留回调；计算只产生 PreviewReady，不发布正式标签。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
PartEditRequest request;
request.expectedLabelMap = parts->GetState().labelMap;
request.expectedCatalogRevision = catalog->catalogRevision;
PartMergeEdit edit;
edit.parts = mergeBindings;
request.operation = edit;
// 这里只生成候选，PreviewReady 后仍要用户确认。
const auto a = parts->SendEditRequest(request, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::PreviewReady) qWarning() << "Edit did not produce preview";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Edit rejected";
```

### 10.12 撤销标签编辑

计算这一种标签编辑的候选；保护区由 scope.editRoi/protectionRoi/protectedParts 限定。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedLabelMap/expectedCatalogRevision` | `DataRevisionRef、uint64_t` | 分别冻结当前标签和目录版本。 |
| `isRedo` | `bool` | false 为撤销，仍须经过预览和确认。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。 SendEditRequest 拒绝不保留回调；计算只产生 PreviewReady，不发布正式标签。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
PartEditRequest request;
request.expectedLabelMap = parts->GetState().labelMap;
request.expectedCatalogRevision = catalog->catalogRevision;
PartHistoryEdit edit;
edit.isRedo = false;
request.operation = edit;
// 这里只生成候选，PreviewReady 后仍要用户确认。
const auto a = parts->SendEditRequest(request, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::PreviewReady) qWarning() << "Edit did not produce preview";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Edit rejected";
```

### 10.13 重做标签编辑

计算这一种标签编辑的候选；保护区由 scope.editRoi/protectionRoi/protectedParts 限定。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedLabelMap/expectedCatalogRevision` | `DataRevisionRef、uint64_t` | 分别冻结当前标签和目录版本。 |
| `isRedo` | `bool` | true 为重做，仍须经过预览和确认。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。 SendEditRequest 拒绝不保留回调；计算只产生 PreviewReady，不发布正式标签。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
PartEditRequest request;
request.expectedLabelMap = parts->GetState().labelMap;
request.expectedCatalogRevision = catalog->catalogRevision;
PartHistoryEdit edit;
edit.isRedo = true;
request.operation = edit;
// 这里只生成候选，PreviewReady 后仍要用户确认。
const auto a = parts->SendEditRequest(request, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::PreviewReady) qWarning() << "Edit did not produce preview";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Edit rejected";
```

### 10.14 确认编辑候选

用户确认后才发布正式标签和零件目录，不能把 PreviewReady 当成已修改。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `previewId` | `uint64_t` | GetEditPreview() 返回的当前候选 ID。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。 SetEditCommit 可能在返回前回调，回调前已完成状态转换，允许再次请求或 Detach；失败会消费该 previewId，但不改正式数据或历史游标。

```cpp
const auto preview = parts->GetEditPreview();
if (!preview) return;
// 可能在返回前回调，先准备好回调依赖的界面状态。
const auto a = parts->SetEditCommit(preview->previewId, [](PartSegmentationResult r) {
    if (r.status != PartResultStatus::Succeeded
        && r.status != PartResultStatus::SucceededWithDisplayFailure) qWarning() << "Commit failed";
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Commit rejected";
```

### 10.15 丢弃编辑候选

用户取消确认时只移除候选，不发布它。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `previewId` | `uint64_t` | 当前候选 ID。 |

出参与返回：ClearEditPreview() 同步返回 PartMutationResult，无回调。Succeeded 为完成；InvalidRequest/NotFound/StaleReference/Busy/Unavailable 等表示 ID、版本或生命周期不符。

```cpp
const auto preview = parts->GetEditPreview();
if (!preview) return;
// 丢弃临时候选，不改正式标签。
const auto result = parts->ClearEditPreview(preview->previewId);
if (result.status != PartMutationStatus::Succeeded)
    qWarning() << "Discard failed";
```

### 10.16 停止分割任务

协作停止当前计算，等待终态。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `PartSegmentationAction` | Stop |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。

```cpp
PartSegmentationRequest request;
request.action = PartSegmentationAction::Stop;

// 是否接纳和业务终态分开处理。
const auto a = parts->SendRequest(request, [](PartSegmentationResult r) {
    if (r.status == PartResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Part action rejected";
```

### 10.17 设置零件叠加可见性

只改变显示，不删除正式标签。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `PartSegmentationAction` | SetVisibility |
| `isVisible` | `optional<bool>` | 明确可见性。 |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。

```cpp
PartSegmentationRequest request;
request.action = PartSegmentationAction::SetVisibility;
request.isVisible = isVisible;
// 是否接纳和业务终态分开处理。
const auto a = parts->SendRequest(request, [](PartSegmentationResult r) {
    if (r.status == PartResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Part action rejected";
```

### 10.18 清除零件结果

清理当前结果，先收口任务和消费者。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `PartSegmentationAction` | Clear |

出参与返回：同步返回 PartSegmentationAdmission；Accepted 表示接纳，InvalidRequest/Busy/Stopping/Unavailable/RevisionConflict/BudgetExceeded 分别表示请求、忙、停止、可用性、版本或预算问题。非空完成回调在所属线程执行一次，短命令可能同步触发。结果 Succeeded/SucceededWithDisplayFailure/Cancelled/Failed/PreviewReady 分别表示成功、已提交但显示失败、取消、失败、候选待确认。

```cpp
PartSegmentationRequest request;
request.action = PartSegmentationAction::Clear;

// 是否接纳和业务终态分开处理。
const auto a = parts->SendRequest(request, [](PartSegmentationResult r) {
    if (r.status == PartResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != PartAdmissionStatus::Accepted) qWarning() << "Part action rejected";
```

### 10.19 选择上一零件

按当前目录顺序循环选择，首项回绕到末项。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `expectedCatalogRevision` | `uint64_t` | 当前目录版本。 |

出参与返回：SetPreviousPart() 同步返回 PartMutationResult，无回调；Succeeded 成功，RevisionConflict 目录过期，Unavailable/Busy 为不可用或忙，其余状态沿用 SetPartState。

```cpp
const auto catalog = parts->GetPartSetSnapshot();
if (!catalog) return;
const auto result = parts->SetPreviousPart(catalog->catalogRevision);
if (result.status != PartMutationStatus::Succeeded)
    qWarning() << "Refresh catalog"; // 不重用旧目录版本。
```

## 11. 孔隙分析

安装头为 MVVCVTK/API/Features/GapAnalysis/Host/GapHostFeature.h、GapHostTypes.h，链接 MVVCVTK::GapAnalysis。当前图像已加载并有相应运行依赖；Start 输入由当前数据和 inputViews 解析。

1. 挂载并配置输入及显示视图。
2. 按灰度与区域条件检测，或基于父结果重新评价。
3. 读取正式统计与对象，再设置显示、选择和退出。

### 11.1 挂载孔隙功能

构造配置声明参与输入的视图，以及默认显示与快捷键。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `inputViews/defaultStart.targetViews` | `HostViewTargets` | 明确的已有视图集合。 |
| `keys` | `GapHostKeys` | 需要时设叠加和退出键。 |

出参与返回：AttachFeature() 同步返回 bool，没有完成回调，真表示完成，假表示配置或挂载条件不满足。

```cpp
GapHostConfig config;
config.inputViews.viewIds = {"primary-3d", "slice-top-down"};
config.defaultStart.targetViews = config.inputViews;
config.keys.switchOverlay.keySym = "F9"; config.keys.exit.keySym = "Escape";
gap = std::make_shared<GapHostFeature>(config);
// 挂载失败停止后续操作，成功句柄由宿主持有。
if (!session->AttachFeature(gap)) qWarning() << "Gap attach failed";
```

### 11.2 按绝对阈值检测孔隙

使用原始标量域中的明确表面阈值。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `surface` | `GapSurfaceConfig` | 阈值方式和已确认的 backgroundMean/materialMean。 |
| `evaluationRoi/detectionRoi` | `optional<DataRevisionRef>` | 可选评价和检测区域，必须同源。 |
| `voidParams` | `GapVoidParams` | 需要时开启体积筛选，minVolumeMM3 为 mm³。 |

出参与返回：同步返回 GapAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 分别表示请求、忙、停止或可用性问题。拒绝不回调；接纳后所属线程回调一次，短命令可同步触发。GapHostResult.status 的 Succeeded/SucceededWithDisplayFailure/Failed/SourceChanged 分别表示成功、业务成功但显示失败、执行失败、来源改变，诊断在 message。

```cpp
GapHostStartParams p;
p.targetViews.viewIds = {"primary-3d"};
p.surface.isoMode = GapIsoMode::AbsoluteValue;
p.surface.absoluteIsoValue = isoValue;
p.surface.backgroundMean = backgroundMean; p.surface.materialMean = materialMean;
p.evaluationRoi = evaluationRoi; p.detectionRoi = detectionRoi;
GapHostRequest request; request.action = GapHostAction::Start; request.start = p;
// 业务提交和显示准备分开判断。
const auto a = gap->SendRequest(request, [](GapHostResult r) {
    if (r.status == GapResultStatus::Failed || r.status == GapResultStatus::SourceChanged)
        qWarning() << QString::fromStdString(r.message);
});
if (a.status != GapAdmissionStatus::Accepted) qWarning() << "Gap rejected";
```

### 11.3 按范围比例检测孔隙

阈值按 min+(max-min)*ratio 解析；当前不钳制比例。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `surface` | `GapSurfaceConfig` | 阈值方式和已确认的 backgroundMean/materialMean。 |
| `evaluationRoi/detectionRoi` | `optional<DataRevisionRef>` | 可选评价和检测区域，必须同源。 |
| `voidParams` | `GapVoidParams` | 需要时开启体积筛选，minVolumeMM3 为 mm³。 |

出参与返回：同步返回 GapAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 分别表示请求、忙、停止或可用性问题。拒绝不回调；接纳后所属线程回调一次，短命令可同步触发。GapHostResult.status 的 Succeeded/SucceededWithDisplayFailure/Failed/SourceChanged 分别表示成功、业务成功但显示失败、执行失败、来源改变，诊断在 message。

```cpp
GapHostStartParams p;
p.targetViews.viewIds = {"primary-3d"};
p.surface.isoMode = GapIsoMode::DataRangeRatio;
p.surface.dataRangeRatio = ratio;
p.surface.backgroundMean = backgroundMean; p.surface.materialMean = materialMean;
p.evaluationRoi = evaluationRoi; p.detectionRoi = detectionRoi;
GapHostRequest request; request.action = GapHostAction::Start; request.start = p;
// 业务提交和显示准备分开判断。
const auto a = gap->SendRequest(request, [](GapHostResult r) {
    if (r.status == GapResultStatus::Failed || r.status == GapResultStatus::SourceChanged)
        qWarning() << QString::fromStdString(r.message);
});
if (a.status != GapAdmissionStatus::Accepted) qWarning() << "Gap rejected";
```

### 11.4 重新评价孔隙结果

用父结果和新的评价区域或筛选条件进行评价，不混用另一代结果。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `parentResult` | `optional<DataRevisionRef>` | 既有 resultSet。 |
| `evaluationRoi/roiPolicy` | `ROI 修订、GapRoiPolicy` | InsideOnly 或 WholeIfIntersects。 |

出参与返回：同步返回 GapAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 分别表示请求、忙、停止或可用性问题。拒绝不回调；接纳后所属线程回调一次，短命令可同步触发。GapHostResult.status 的 Succeeded/SucceededWithDisplayFailure/Failed/SourceChanged 分别表示成功、业务成功但显示失败、执行失败、来源改变，诊断在 message。

```cpp
GapHostStartParams p = evaluationParams;
p.parentResult = resultSet; p.evaluationRoi = roiRevision;
p.isEvaluationOnly = true;
GapHostRequest request; request.action = GapHostAction::Evaluate; request.start = p;
// 业务提交和显示准备分开判断。
const auto a = gap->SendRequest(request, [](GapHostResult r) {
    if (r.status == GapResultStatus::Failed) qWarning() << "Gap evaluation failed";
});
if (a.status != GapAdmissionStatus::Accepted) qWarning() << "Evaluation rejected";
```

### 11.5 读取孔隙对象

正式统计与对象目录定义不同：目录包含 BoundaryUncertain，正式孔数、体积和孔隙率排除它们。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `resultSet` | `DataRevisionRef` | 完成结果的精确结果集。 |
| `offset/count` | `uint64_t、size_t` | 对象页最多 4096 条。 |

出参与返回：GetResultReader() 同步返回 `shared_ptr<const GapResultReader>`，空为不可用；GetObjects() 返回 GapObjectPage，None 为成功，Unavailable/InvalidRequest/Cancelled/Failed 为数据、参数、取消或读取问题。没有回调；只读 reader 可用于后台，按 next 续读。

```cpp
// 固定同一代正式结果，不用当前页面替换结果身份。
const auto reader = gap->GetResultReader(resultSet);
if (!reader) return;
const auto page = reader->GetObjects(offset, 256);
if (page.error != GapReadError::None) return;
offset = page.next;
for (const auto& object : page.objects) {
    if (object.boundaryStatus == GapBoundaryStatus::BoundaryUncertain) continue;
    qInfo() << "Formal void mm3:" << object.volumeMM3;
}
```

### 11.6 读取孔隙几何

按对象分块读点和三角形，保留正式全局点编号。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `objectId/pointOffset/triangleOffset` | `int32_t、uint64_t` | 对象 ID 和两类独立偏移。 |
| `count` | `size_t` | 每类最多 65536 条。 |

出参与返回：GetGeometry() 同步返回 GapGeometryChunk，无回调。error=None 为成功，Unavailable 表示后备几何不可用；CreateObjectReader() 只保留目录，其几何读取也返回 Unavailable。用 nextPoint/nextTriangle 续读。

```cpp
const auto reader = gap->GetResultReader(resultSet);
if (!reader) return;
const auto chunk = reader->GetGeometry(objectId, pointOffset, triangleOffset, 1024);
if (chunk.error != GapReadError::None) return;
pointOffset = chunk.nextPoint; triangleOffset = chunk.nextTriangle;
// triangleIndices 不是当前页的局部点下标。
```

### 11.7 设置孔隙显示

色带投影对象体积，修改它不改变检测标签或正式统计。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `displayResult` | `DataRevisionRef` | 明确的结果修订。 |
| `display` | `optional<GapDisplayParams>` | 颜色、透明度、图例；range 单位 mm³，rangeMode 为 Result/SelectedInterval。 |

出参与返回：同步返回 GapAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 分别表示请求、忙、停止或可用性问题。拒绝不回调；接纳后所属线程回调一次，短命令可同步触发。GapHostResult.status 的 Succeeded/SucceededWithDisplayFailure/Failed/SourceChanged 分别表示成功、业务成功但显示失败、执行失败、来源改变，诊断在 message。

```cpp
GapHostRequest request;
request.action = GapHostAction::SetDisplay;
request.displayResult = resultSet;
GapDisplayParams p; p.mode = GapColorMode::Rainbow;
p.rangeMode = GapRangeMode::SelectedInterval; p.range = volumeRangeMm3;
request.display = p;
// 业务提交和显示准备分开判断。
const auto a = gap->SendRequest(request, [](GapHostResult r) {
    if (r.status == GapResultStatus::Failed) qWarning() << "Gap display failed";
});
if (a.status != GapAdmissionStatus::Accepted) qWarning() << "Display rejected";
```

### 11.8 选择或隔离孔隙

对一个正式结果选择对象；空 regionId 表示清除选择。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `selection` | `GapObjectSelection` | 结果修订、可选 int32_t 对象编号及 isIsolated。 |

出参与返回：同步返回 GapAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 分别表示请求、忙、停止或可用性问题。拒绝不回调；接纳后所属线程回调一次，短命令可同步触发。GapHostResult.status 的 Succeeded/SucceededWithDisplayFailure/Failed/SourceChanged 分别表示成功、业务成功但显示失败、执行失败、来源改变，诊断在 message。

```cpp
GapHostRequest request;
request.action = GapHostAction::SelectObject;
request.selection = GapObjectSelection{resultSet, objectId, true};
// 业务提交和显示准备分开判断。
const auto a = gap->SendRequest(request, [](GapHostResult r) {
    if (r.status == GapResultStatus::Failed) qWarning() << "Gap selection failed";
});
if (a.status != GapAdmissionStatus::Accepted) qWarning() << "Selection rejected";
```

### 11.9 启用孔隙拾取

通过 Host 输入链选取对象。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `GapHostAction` | SetObjectPicking |

出参与返回：同步返回 GapAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 分别表示请求、忙、停止或可用性问题。拒绝不回调；接纳后所属线程回调一次，短命令可同步触发。GapHostResult.status 的 Succeeded/SucceededWithDisplayFailure/Failed/SourceChanged 分别表示成功、业务成功但显示失败、执行失败、来源改变，诊断在 message。

```cpp
GapHostRequest request;
request.action = GapHostAction::SetObjectPicking;
request.isObjectPickingEnabled = true;
// 业务提交和显示准备分开判断。
const auto a = gap->SendRequest(request, [](GapHostResult r) {
    if (r.status == GapResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != GapAdmissionStatus::Accepted) qWarning() << "Gap action rejected";
```

### 11.10 切换孔隙叠加

显示或隐藏已经生成的结果。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `GapHostAction` | Overlay |

出参与返回：同步返回 GapAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 分别表示请求、忙、停止或可用性问题。拒绝不回调；接纳后所属线程回调一次，短命令可同步触发。GapHostResult.status 的 Succeeded/SucceededWithDisplayFailure/Failed/SourceChanged 分别表示成功、业务成功但显示失败、执行失败、来源改变，诊断在 message。

```cpp
GapHostRequest request;
request.action = GapHostAction::Overlay;

// 业务提交和显示准备分开判断。
const auto a = gap->SendRequest(request, [](GapHostResult r) {
    if (r.status == GapResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != GapAdmissionStatus::Accepted) qWarning() << "Gap action rejected";
```

### 11.11 退出孔隙分析

等待视图解绑完成后兑现退出；不是独立 Cancel 动作。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `GapHostAction` | Exit |

出参与返回：同步返回 GapAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 分别表示请求、忙、停止或可用性问题。拒绝不回调；接纳后所属线程回调一次，短命令可同步触发。GapHostResult.status 的 Succeeded/SucceededWithDisplayFailure/Failed/SourceChanged 分别表示成功、业务成功但显示失败、执行失败、来源改变，诊断在 message。

```cpp
GapHostRequest request;
request.action = GapHostAction::Exit;

// 业务提交和显示准备分开判断。
const auto a = gap->SendRequest(request, [](GapHostResult r) {
    if (r.status == GapResultStatus::Failed) qWarning() << QString::fromStdString(r.message);
});
if (a.status != GapAdmissionStatus::Accepted) qWarning() << "Gap action rejected";
```

## 12. 壁厚

仓内打开 MVVCVTK_BUILD_WALL_THICKNESS，链接 MVVCVTK::WallThickness，包含 Host/WallThicknessHostFeature.h、WallThicknessHostTypes.h。当前只有 CUDA 引擎，没有 CPU 后端。输入需要同源灰度、整数标签和正式测量网格，不能只给显示表面。

1. 从图像、零件分割和正式表面结果取得 source/labels/mesh 精确修订。
2. 明确长度单位和材料阈值，启动壁厚计算。
3. 读取覆盖率、有效性和统计，再设置评价或显示。
4. 取消、清理并成功卸载后释放句柄。

### 12.1 挂载壁厚功能

壁厚实例由 Session 管理生命周期。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `ThicknessConfig` | `构造配置` | 需要时设置工作预算、样本上限和截止时间。 |

出参与返回：AttachFeature() 同步返回 bool，没有完成回调；真为完成，假为环境、依赖或拓扑条件不满足。

```cpp
wall = std::make_shared<WallThicknessHostFeature>();
// 挂载失败停止后续操作，成功句柄由宿主持有。
if (!session->AttachFeature(wall)) {
    qWarning() << "Wall feature attach failed";
    return;
}
const auto state = wall->GetState();
```

### 12.2 完整局部支持模式

每个实际搜索方向都要求完整的局部体支持，不要求整个网格闭合。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `input.source/labels/mesh` | `DataRevisionRef` | 同源灰度、材料标签、正式测量网格；materialLabels 可选择非零材料标签。 |
| `input.unit` | `ThicknessUnit` | 显式 Millimeter 或 Meter，不自动缩放数据。 |
| `params.maxDistance/sampleSpacing` | `double` | 正的长度，单位跟随 input.unit。 |
| `params.materialThreshold` | `optional<double>` | 必须给有限原始灰度，材料定义为 trilinear(source)>threshold。 |
| `boundaryPolicy` | `ThicknessBoundaryPolicy` | Complete |

出参与返回：同步返回 ThicknessAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 为请求、忙、停止或可用性问题。拒绝不回调；所属线程完成回调一次，短命令可同步触发，可查询、再次请求或取消。ThicknessStatus 的 Succeeded/InvalidInput/IncompleteBoundary/NoValidSamples/Cancelled/DeadlineExceeded/BudgetExceeded/StaleInput/DisplayFailed 分别表示成功、输入、边界、有效样本、取消、超时、预算、过期或显示问题。

```cpp
ThicknessStartRequest request;
request.input = input; // input 的三份修订已核实同源。
request.params = params;
request.params.boundaryPolicy = ThicknessBoundaryPolicy::Complete;
const auto a = wall->SendRequest(ThicknessRequest{request}, [](const ThicknessResult& r) {
    if (r.status != ThicknessStatus::Succeeded) qWarning() << QString::fromStdString(r.message);
});
if (a.status != ThicknessAdmissionStatus::Accepted) qWarning() << "Wall rejected";
```

### 12.3 源边界局部模式

使用可用局部支持；缺失方向可能改变最小值时仍保留未测，不能当零厚度。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `input.source/labels/mesh` | `DataRevisionRef` | 同源灰度、材料标签、正式测量网格；materialLabels 可选择非零材料标签。 |
| `input.unit` | `ThicknessUnit` | 显式 Millimeter 或 Meter，不自动缩放数据。 |
| `params.maxDistance/sampleSpacing` | `double` | 正的长度，单位跟随 input.unit。 |
| `params.materialThreshold` | `optional<double>` | 必须给有限原始灰度，材料定义为 trilinear(source)>threshold。 |
| `boundaryPolicy` | `ThicknessBoundaryPolicy` | SourceExtentLocal |

出参与返回：同步返回 ThicknessAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 为请求、忙、停止或可用性问题。拒绝不回调；所属线程完成回调一次，短命令可同步触发，可查询、再次请求或取消。ThicknessStatus 的 Succeeded/InvalidInput/IncompleteBoundary/NoValidSamples/Cancelled/DeadlineExceeded/BudgetExceeded/StaleInput/DisplayFailed 分别表示成功、输入、边界、有效样本、取消、超时、预算、过期或显示问题。

```cpp
ThicknessStartRequest request;
request.input = input; // input 的三份修订已核实同源。
request.params = params;
request.params.boundaryPolicy = ThicknessBoundaryPolicy::SourceExtentLocal;
const auto a = wall->SendRequest(ThicknessRequest{request}, [](const ThicknessResult& r) {
    if (r.status != ThicknessStatus::Succeeded) qWarning() << QString::fromStdString(r.message);
});
if (a.status != ThicknessAdmissionStatus::Accepted) qWarning() << "Wall rejected";
```

### 12.4 读取壁厚结果

统计需结合有效面积覆盖率，无效样本不以零厚度参与均值。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `result` | `DataRevisionRef` | 完成结果中的 result 修订。 |

出参与返回：GetResult() 同步返回 `optional<ThicknessSnapshot>`，空为结果不可读，无回调。statistics 的 minimum/maximum/mean/quantiles 可为空；样本 validity 区分有效、截断支持、搜索上限、无效材料路径等原因。

```cpp
const auto snapshot = wall->GetResult(resultRevision);
if (!snapshot) return;
qInfo() << "Coverage:" << snapshot->statistics.coverage;
if (snapshot->statistics.mean) qInfo() << "Mean:" << *snapshot->statistics.mean;
else qWarning() << "No valid thickness mean"; // 不用 0 代替缺失值。
```

### 12.5 重设壁厚评价

改变公差和统计区间，保留原始计算结果。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `evaluation.lower/upper` | `double` | 0≤lower≤upper 且 upper>0，单位跟随输入长度。 |
| `histogramRange/histogramBins` | `区间、size_t` | 非负递增区间，桶数 1～4096。 |

出参与返回：同步返回 ThicknessAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 为请求、忙、停止或可用性问题。拒绝不回调；所属线程完成回调一次，短命令可同步触发，可查询、再次请求或取消。ThicknessStatus 的 Succeeded/InvalidInput/IncompleteBoundary/NoValidSamples/Cancelled/DeadlineExceeded/BudgetExceeded/StaleInput/DisplayFailed 分别表示成功、输入、边界、有效样本、取消、超时、预算、过期或显示问题。

```cpp
ThicknessEvaluation evaluation;
evaluation.lower = lower; evaluation.upper = upper;
evaluation.histogramRange = histogramRange;
evaluation.histogramBins = 64;
// 计算终态与显示准备分开检查。
const auto a = wall->SendRequest(ThicknessEvaluationRequest{evaluation},
    [](const ThicknessResult& r) { if (r.status != ThicknessStatus::Succeeded) qWarning() << "Evaluation failed"; });
if (a.status != ThicknessAdmissionStatus::Accepted) qWarning() << "Evaluation rejected";
```

### 12.6 连续壁厚显示

按壁厚值映射色带，不改变测量值。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `result` | `DataRevisionRef` | 明确绑定要修改的结果。 |
| `display.targetViews/range` | `目标视图、长度区间` | 已有视图和输入长度单位的显示区间。 |
| `display.mode` | `ThicknessDisplayMode` | Continuous |

出参与返回：同步返回 ThicknessAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 为请求、忙、停止或可用性问题。拒绝不回调；所属线程完成回调一次，短命令可同步触发，可查询、再次请求或取消。ThicknessStatus 的 Succeeded/InvalidInput/IncompleteBoundary/NoValidSamples/Cancelled/DeadlineExceeded/BudgetExceeded/StaleInput/DisplayFailed 分别表示成功、输入、边界、有效样本、取消、超时、预算、过期或显示问题。

```cpp
ThicknessDisplay display;
display.targetViews.viewIds = {"primary-3d"};
display.mode = ThicknessDisplayMode::Continuous; display.range = displayRange;
ThicknessDisplayRequest request{display, resultRevision};
// 计算终态与显示准备分开检查。
const auto a = wall->SendRequest(request, [](const ThicknessResult& r) {
    if (r.status != ThicknessStatus::Succeeded) qWarning() << "Wall display failed";
});
if (a.status != ThicknessAdmissionStatus::Accepted) qWarning() << "Display rejected";
```

### 12.7 公差壁厚显示

按评价公差分类显示，使用独立的公差颜色。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `result` | `DataRevisionRef` | 明确绑定要修改的结果。 |
| `display.targetViews/range` | `目标视图、长度区间` | 已有视图和输入长度单位的显示区间。 |
| `display.mode` | `ThicknessDisplayMode` | Tolerance |

出参与返回：同步返回 ThicknessAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 为请求、忙、停止或可用性问题。拒绝不回调；所属线程完成回调一次，短命令可同步触发，可查询、再次请求或取消。ThicknessStatus 的 Succeeded/InvalidInput/IncompleteBoundary/NoValidSamples/Cancelled/DeadlineExceeded/BudgetExceeded/StaleInput/DisplayFailed 分别表示成功、输入、边界、有效样本、取消、超时、预算、过期或显示问题。

```cpp
ThicknessDisplay display;
display.targetViews.viewIds = {"primary-3d"};
display.mode = ThicknessDisplayMode::Tolerance; display.range = displayRange;
ThicknessDisplayRequest request{display, resultRevision};
// 计算终态与显示准备分开检查。
const auto a = wall->SendRequest(request, [](const ThicknessResult& r) {
    if (r.status != ThicknessStatus::Succeeded) qWarning() << "Wall display failed";
});
if (a.status != ThicknessAdmissionStatus::Accepted) qWarning() << "Display rejected";
```

### 12.8 激活历史壁厚结果

读取历史结果不自动激活，用明确动作恢复其显示。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `resultRevision` | `DataRevisionRef` | 要激活的正式结果。 |
| `display` | `optional<ThicknessDisplay>` | 需要同时更新显示时提供。 |

出参与返回：同步返回 ThicknessAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 为请求、忙、停止或可用性问题。拒绝不回调；所属线程完成回调一次，短命令可同步触发，可查询、再次请求或取消。ThicknessStatus 的 Succeeded/InvalidInput/IncompleteBoundary/NoValidSamples/Cancelled/DeadlineExceeded/BudgetExceeded/StaleInput/DisplayFailed 分别表示成功、输入、边界、有效样本、取消、超时、预算、过期或显示问题。

```cpp
ThicknessActivationRequest request;
request.resultRevision = resultRevision;
// 计算终态与显示准备分开检查。
const auto a = wall->SendRequest(request, [](const ThicknessResult& r) {
    if (!r.isActivated) qWarning() << "Wall activation did not complete";
});
if (a.status != ThicknessAdmissionStatus::Accepted) qWarning() << "Activation rejected";
```

### 12.9 选择壁厚样本

把表格选中样本投影到结果显示。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `resultRevision/sampleIndex` | `DataRevisionRef、size_t` | 明确结果和样本下标。 |

出参与返回：同步返回 ThicknessAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 为请求、忙、停止或可用性问题。拒绝不回调；所属线程完成回调一次，短命令可同步触发，可查询、再次请求或取消。ThicknessStatus 的 Succeeded/InvalidInput/IncompleteBoundary/NoValidSamples/Cancelled/DeadlineExceeded/BudgetExceeded/StaleInput/DisplayFailed 分别表示成功、输入、边界、有效样本、取消、超时、预算、过期或显示问题。

```cpp
ThicknessSampleRequest request;
request.resultRevision = resultRevision;
request.sampleIndex = sampleIndex;
// 计算终态与显示准备分开检查。
const auto a = wall->SendRequest(request, [](const ThicknessResult& r) {
    if (r.status != ThicknessStatus::Succeeded) qWarning() << "Sample selection failed";
});
if (a.status != ThicknessAdmissionStatus::Accepted) qWarning() << "Sample rejected";
```

### 12.10 查询模型点壁厚

从节点场插值查询一个位置；这不是重新发起测量。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `result/modelPoint` | `DataRevisionRef、ThicknessPoint` | 模型坐标和输入长度单位。 |

出参与返回：GetThicknessValue() 同步返回 `optional<double>`，有值为该位置可用厚度，无值为缺少有效支持或结果不可读，没有回调。最近角点须有效，不能把空值当作零。

```cpp
const auto value = wall->GetThicknessValue(resultRevision, modelPoint);
if (!value) {
    qWarning() << "No valid thickness support";
    return;
}
qInfo() << "Thickness:" << *value; // 单位跟随 ThicknessInput.unit。
```

### 12.11 取消壁厚任务

协作取消当前计算，并等完成终态。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetRequestId` | `uint64_t` | 实际启动请求的 ID。 |

出参与返回：同步返回 ThicknessAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 为请求、忙、停止或可用性问题。拒绝不回调；所属线程完成回调一次，短命令可同步触发，可查询、再次请求或取消。ThicknessStatus 的 Succeeded/InvalidInput/IncompleteBoundary/NoValidSamples/Cancelled/DeadlineExceeded/BudgetExceeded/StaleInput/DisplayFailed 分别表示成功、输入、边界、有效样本、取消、超时、预算、过期或显示问题。

```cpp
// 计算终态与显示准备分开检查。
const auto a = wall->SendRequest(ThicknessCancelRequest{targetRequestId}, [](const ThicknessResult& r) {
    qInfo() << "Wall terminal:" << static_cast<int>(r.status);
});
if (a.status != ThicknessAdmissionStatus::Accepted) qWarning() << "Wall action rejected";
const auto state = wall->GetState();
```

### 12.12 清除壁厚结果

先收口计算和消费者，再清理结果。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `ThicknessClearRequest` | 清理当前结果。 |

出参与返回：同步返回 ThicknessAdmission；Accepted 接纳，InvalidRequest/Busy/Stopping/Unavailable 为请求、忙、停止或可用性问题。拒绝不回调；所属线程完成回调一次，短命令可同步触发，可查询、再次请求或取消。ThicknessStatus 的 Succeeded/InvalidInput/IncompleteBoundary/NoValidSamples/Cancelled/DeadlineExceeded/BudgetExceeded/StaleInput/DisplayFailed 分别表示成功、输入、边界、有效样本、取消、超时、预算、过期或显示问题。

```cpp
// 计算终态与显示准备分开检查。
const auto a = wall->SendRequest(ThicknessClearRequest{}, [](const ThicknessResult& r) {
    qInfo() << "Wall terminal:" << static_cast<int>(r.status);
});
if (a.status != ThicknessAdmissionStatus::Accepted) qWarning() << "Wall action rejected";
const auto state = wall->GetState();
```

## 13. 模型旋转

仓内打开 MVVCVTK_BUILD_MODEL_ROTATION，链接 MVVCVTK::ModelRotation，包含 Host/ModelRotationHostFeature.h、ModelRotationHostTypes.h。该功能改模型显示姿态；相机观察方向由导航 RotateBy() 控制。

1. 挂载功能并选择目标视图。
2. 启用交互或发送明确的世界轴旋转。
3. 用帧完成结果确认，按需要取消、撤销或禁用。

### 13.1 挂载模型旋转

目标可以是三维、组合和切片视图。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetViews` | `HostViewTargets` | 需要限制作用范围时显式提供，默认覆盖常用三维和切片角色。 |

出参与返回：AttachFeature() 同步返回 bool，无完成回调；真为完成，假为环境或拓扑条件不满足。

```cpp
ModelRotationConfig config;
config.targetViews.viewIds = {"primary-3d", "slice-top-down"};
rotation = std::make_shared<ModelRotationHostFeature>(config);
// 挂载失败停止后续操作，成功句柄由宿主持有。
if (!session->AttachFeature(rotation)) qWarning() << "Rotation attach failed";
const auto state = rotation->GetState();
```

### 13.2 启用交互旋转

把旋转工具接到已有输入链，后续拖动由功能处理。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `isEnabled` | `bool` | true 启用，false 退出交互工具。 |

出参与返回：同步返回 ModelRotationAdmission：Accepted 接纳，InvalidRequest/Unavailable 为参数或可用性问题。拒绝不回调；所属线程回调一次，旋转和撤销成功在帧提交后兑现，失败或短命令可同步完成。结果 Succeeded/Cancelled/Invalidated 为成功、取消、失效，Detached/Idle 为未挂载/空闲；Pending/Dragging 仍未完成。

```cpp
ModelRotationRequest request;
request.action = ModelRotationAction::SetEnabled;
request.isEnabled = true;
// 成功需要满足本动作的帧提交规则。
const auto a = rotation->SendRequest(request, [](ModelRotationResult r) {
    qInfo() << "Rotation state:" << static_cast<int>(r.status);
});
if (a.status != ModelRotationAdmissionStatus::Accepted) qWarning() << "Enable rejected";
```

### 13.3 按数值旋转

围绕世界坐标中的轴和中心旋转模型。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `worldAxis/angleDeg` | `array<double,3>、double` | 非零有限世界轴、有限角度，度。 |
| `worldCenter` | `optional<array<double,3>>` | 明确中心时填世界坐标，省略则由功能解析。 |

出参与返回：同步返回 ModelRotationAdmission：Accepted 接纳，InvalidRequest/Unavailable 为参数或可用性问题。拒绝不回调；所属线程回调一次，旋转和撤销成功在帧提交后兑现，失败或短命令可同步完成。结果 Succeeded/Cancelled/Invalidated 为成功、取消、失效，Detached/Idle 为未挂载/空闲；Pending/Dragging 仍未完成。

```cpp
ModelRotationRequest request;
request.action = ModelRotationAction::Rotate;
request.worldAxis = axis; request.angleDeg = angleDeg;
request.worldCenter = worldCenter;
// 成功需要满足本动作的帧提交规则。
const auto a = rotation->SendRequest(request, [](ModelRotationResult r) {
    if (r.status != ModelRotationStatus::Succeeded) qWarning() << "Rotation not committed";
});
if (a.status != ModelRotationAdmissionStatus::Accepted) qWarning() << "Rotation rejected";
```

### 13.4 取消模型旋转

取消当前动作，正式帧状态从结果读取。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `ModelRotationAction` | Cancel |

出参与返回：同步返回 ModelRotationAdmission：Accepted 接纳，InvalidRequest/Unavailable 为参数或可用性问题。拒绝不回调；所属线程回调一次，旋转和撤销成功在帧提交后兑现，失败或短命令可同步完成。结果 Succeeded/Cancelled/Invalidated 为成功、取消、失效，Detached/Idle 为未挂载/空闲；Pending/Dragging 仍未完成。

```cpp
ModelRotationRequest request;
request.action = ModelRotationAction::Cancel;
// 成功需要满足本动作的帧提交规则。
const auto a = rotation->SendRequest(request, [](ModelRotationResult r) {
    qInfo() << "Rotation terminal:" << static_cast<int>(r.status);
});
if (a.status != ModelRotationAdmissionStatus::Accepted) qWarning() << "Rotation action rejected";
```

### 13.5 撤销模型旋转

撤销一个已提交旋转；用 GetState().undoCount 控制按钮。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `ModelRotationAction` | Undo |

出参与返回：同步返回 ModelRotationAdmission：Accepted 接纳，InvalidRequest/Unavailable 为参数或可用性问题。拒绝不回调；所属线程回调一次，旋转和撤销成功在帧提交后兑现，失败或短命令可同步完成。结果 Succeeded/Cancelled/Invalidated 为成功、取消、失效，Detached/Idle 为未挂载/空闲；Pending/Dragging 仍未完成。

```cpp
ModelRotationRequest request;
request.action = ModelRotationAction::Undo;
// 成功需要满足本动作的帧提交规则。
const auto a = rotation->SendRequest(request, [](ModelRotationResult r) {
    qInfo() << "Rotation terminal:" << static_cast<int>(r.status);
});
if (a.status != ModelRotationAdmissionStatus::Accepted) qWarning() << "Rotation action rejected";
```

## 14. 计量对齐与尺寸评价

请求并链接可选组件 MVVCVTK::MetrologyAlignment，安装头在 MVVCVTK/API/Features/MetrologyAlignment/Host/：MetrologyAlignmentHostFeature.h、MetrologyAlignmentHostTypes.h、MetrologyReport.h、VoxelVolume.h。单位、坐标身份和顶点编号必须绑定精确修订。

1. 挂载功能，准备 source/mesh/scopeData 和参考网格。
2. 保存明确配方，再用返回的配方修订求解。
3. 读取求解结果，分别进行偏差、尺寸或体素体积评价。
4. 审核后才激活或应用姿态，退出时先取消并确认卸载完成。

### 14.1 挂载计量功能

为计量结果和可视化设置目标视图。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetViews` | `HostViewTargets` | 已有显示目标。 |

出参与返回：AttachFeature() 同步返回 bool，无回调。真为挂载完成，假为依赖或拓扑失败；正常析构前必须成功 Detach，返回 false 时仍保留对象并重试。

```cpp
AlignmentConfig config;
config.targetViews.viewIds = {"primary-3d"};
alignment = std::make_shared<MetrologyAlignmentHostFeature>(config);
// 挂载失败停止后续操作，成功句柄由宿主持有。
if (!session->AttachFeature(alignment)) qWarning() << "Alignment attach failed";
const auto state = alignment->GetState();
```

### 14.2 导入参考 STL

STL 不携带单位，需要宿主明确声明文件单位和目标单位。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `filePath` | `string` | UTF-8 STL 路径。 |
| `frameId/coordinateFrame` | `string` | 参考坐标身份和轴约定。 |
| `fileUnit/unit` | `AlignmentUnit` | Millimeter、Meter 或保持 ModelUnit；不能从 ModelUnit 猜物理单位。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentReferenceRequest reference;
reference.filePath = path.toUtf8().toStdString();
reference.frameId = frameId; reference.coordinateFrame = "RAS";
reference.fileUnit = AlignmentUnit::Millimeter; reference.unit = AlignmentUnit::Millimeter;
AlignmentRequest request; request.action = AlignmentAction::ImportReference;
request.reference = reference;
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << "Reference import failed";
    // 成功后的 r.referenceMesh 可填入配方 nominalData。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Reference rejected";
```

### 14.3 顺序平面对齐配方

按配方的顺序基准平面约束对齐。 宿主必须提供经业务确认的几何、选区和名义约束，不由指南猜测基准。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `recipe.geometries/constraints` | `几何和约束集合` | 点、线、平面、圆、球、圆柱及 Hard/Soft/Check 约束。 |
| `exactMesh/nominalData` | `DataRevisionRef` | 顶点编号固定的实测网格与参考数据。 |
| `fitPairs/surfaceFit` | `点对集合或可选表面拟合` | 最佳拟合两者互斥，必要时提供 prealignment。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentRecipe recipe = configuredRecipe;
recipe.method = AlignmentMethod::SequentialPlanes;
AlignmentRequest request;
request.action = AlignmentAction::SaveRecipe; request.recipe = recipe;
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << "Recipe rejected";
    // 成功后保存 r.recipe，Start 使用精确配方修订。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Save rejected";
```

### 14.4 平面与双孔对齐配方

按明确平面和双孔几何约束对齐。 宿主必须提供经业务确认的几何、选区和名义约束，不由指南猜测基准。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `recipe.geometries/constraints` | `几何和约束集合` | 点、线、平面、圆、球、圆柱及 Hard/Soft/Check 约束。 |
| `exactMesh/nominalData` | `DataRevisionRef` | 顶点编号固定的实测网格与参考数据。 |
| `fitPairs/surfaceFit` | `点对集合或可选表面拟合` | 最佳拟合两者互斥，必要时提供 prealignment。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentRecipe recipe = configuredRecipe;
recipe.method = AlignmentMethod::PlaneTwoHoles;
AlignmentRequest request;
request.action = AlignmentAction::SaveRecipe; request.recipe = recipe;
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << "Recipe rejected";
    // 成功后保存 r.recipe，Start 使用精确配方修订。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Save rejected";
```

### 14.5 参考点系统对齐配方

通过配方约束构造参考点系统。 宿主必须提供经业务确认的几何、选区和名义约束，不由指南猜测基准。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `recipe.geometries/constraints` | `几何和约束集合` | 点、线、平面、圆、球、圆柱及 Hard/Soft/Check 约束。 |
| `exactMesh/nominalData` | `DataRevisionRef` | 顶点编号固定的实测网格与参考数据。 |
| `fitPairs/surfaceFit` | `点对集合或可选表面拟合` | 最佳拟合两者互斥，必要时提供 prealignment。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentRecipe recipe = configuredRecipe;
recipe.method = AlignmentMethod::Rps;
AlignmentRequest request;
request.action = AlignmentAction::SaveRecipe; request.recipe = recipe;
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << "Recipe rejected";
    // 成功后保存 r.recipe，Start 使用精确配方修订。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Save rejected";
```

### 14.6 受约束最佳拟合配方

使用基准和对应点或表面拟合优化剩余自由度。 宿主必须提供经业务确认的几何、选区和名义约束，不由指南猜测基准。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `recipe.geometries/constraints` | `几何和约束集合` | 点、线、平面、圆、球、圆柱及 Hard/Soft/Check 约束。 |
| `exactMesh/nominalData` | `DataRevisionRef` | 顶点编号固定的实测网格与参考数据。 |
| `fitPairs/surfaceFit` | `点对集合或可选表面拟合` | 最佳拟合两者互斥，必要时提供 prealignment。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentRecipe recipe = configuredRecipe;
recipe.method = AlignmentMethod::ConstrainedBestFit;
AlignmentRequest request;
request.action = AlignmentAction::SaveRecipe; request.recipe = recipe;
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << "Recipe rejected";
    // 成功后保存 r.recipe，Start 使用精确配方修订。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Save rejected";
```

### 14.7 启动对齐求解

保存配方后再启动，输入声明与数据修订一起参与条件提交。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `recipeRef/input` | `DataRevisionRef、AlignmentInput` | input 含 source/mesh/scopeData、sourceFrameId/coordinateFrame/scope/unit。 |
| `initialPoses` | `vector<AlignmentMatrix>` | 行主序刚体矩阵，pTarget=T*pSource，不允许尺度。 |
| `isActivationRequested` | `bool` | 默认 true；希望审核后激活时显式 false。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentRequest request;
request.action = AlignmentAction::Start;
request.recipeRef = recipeRevision; request.input = input;
request.isActivationRequested = false; // 求解和用户确认分开。
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << QString::fromStdString(r.message);
    // FullyDetermined 不是尺寸合格，后续读取各评价 verdict。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Solve rejected";
```

### 14.8 读取对齐结果

取得配方、输入、拟合几何、残差和变换修订，历史可读性与当前适用性分开。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `result` | `DataRevisionRef` | 完成结果的 result 修订。 |

出参与返回：GetResult() 同步返回 `optional<AlignmentSnapshot>`，空表示结果不可读，没有回调。isCurrent 表示捕获时是否仍当前，diagnostics 给出求解质量；不能只用有值判断业务合格。

```cpp
// 历史结果可读性和当前输入适用性分开。
const auto snapshot = alignment->GetResult(resultRevision);
if (!snapshot) {
    qWarning() << "Alignment result unavailable";
    return;
}
qInfo() << "Current:" << snapshot->isCurrent;
const auto transformRevision = snapshot->transform;
```

### 14.9 无符号表面偏差

评价实测到名义表面的距离。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `alignmentResult` | `DataRevisionRef` | 正式对齐结果。 |
| `maxDistance/tolerance` | `double、optional<array<double,2>>` | 同对齐长度单位；公差省略只报告偏差。 |
| `computeBackend` | `AlignmentComputeBackend` | 默认 Cpu；显式 Cuda 不可用时返回 Unavailable，不回退。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentDeviationRequest p;
p.alignmentResult = resultRevision;
p.metric = AlignmentDeviationMetric::UnsignedDistance;
p.maxDistance = maxDistance; p.tolerance = tolerance;
AlignmentRequest request; request.action = AlignmentAction::EvaluateSurface;
request.deviation = p;
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << "Deviation failed";
    // r.deviationResult 对应 GetDeviation()，合格判定看 snapshot.verdict。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Deviation rejected";
```

### 14.10 名义法向投影偏差

计算最近名义三角法向上的点积，不是全局内外符号距离。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `alignmentResult` | `DataRevisionRef` | 正式对齐结果。 |
| `maxDistance/tolerance` | `double、optional<array<double,2>>` | 同对齐长度单位；公差省略只报告偏差。 |
| `computeBackend` | `AlignmentComputeBackend` | 默认 Cpu；显式 Cuda 不可用时返回 Unavailable，不回退。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentDeviationRequest p;
p.alignmentResult = resultRevision;
p.metric = AlignmentDeviationMetric::NominalNormalProjection;
p.maxDistance = maxDistance; p.tolerance = tolerance;
AlignmentRequest request; request.action = AlignmentAction::EvaluateSurface;
request.deviation = p;
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << "Deviation failed";
    // r.deviationResult 对应 GetDeviation()，合格判定看 snapshot.verdict。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Deviation rejected";
```

### 14.11 评价几何尺寸

通过独立几何选区和测量项目评价尺寸，不把拟合残差当形位公差或测量不确定度。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `plan.alignmentResult/geometries` | `对齐修订、InspectionGeometrySpec 集合` | 几何可来自实测、参考或名义定义；选区编号绑定其网格。 |
| `plan.items` | `vector<InspectionItem>` | 半径、直径、中心、中心距、角度、平行平面距离或拟合残差。 |
| `nominal/nominalMeasure/deviationLimits` | `可选名义及公差` | 名义值与名义几何计算二选一；偏差为实测减名义，角度为度。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
InspectionPlan plan = configuredPlan;
plan.alignmentResult = resultRevision;
AlignmentRequest request; request.action = AlignmentAction::Inspect;
request.inspection = plan;
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << "Inspection failed";
    // GetInspection(r.inspectionResult) 的 report->verdict 才是尺寸结论。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Inspection rejected";
```

### 14.12 按灰度区间计算体积

直接比较存储值闭区间，不应用 slope/intercept。 体素体积不依赖先完成对齐。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `source/extent` | `DataRevisionRef、optional<array<int,6>>` | 源修订及含端点索引区域，省略区域为整卷。 |
| `storedValueRange/labels` | `灰度区间或整数集合` | 按本模式设置。 |
| `requireEnclosedSelection` | `bool` | 需要非空且不碰评价索引边界时设 true。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
VoxelVolumeRequest p; p.id = "material"; p.source = sourceRevision;
p.method = VoxelVolumeMethod::StoredValueRange;
p.storedValueRange = storedRange;
AlignmentRequest request; request.action = AlignmentAction::EvaluateVoxelVolume;
request.voxelVolume = p;
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << "Volume evaluation failed";
    // GetVoxelVolume(r.voxelVolumeResult)：volumeMm3 为空时不能用 0 代替。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Volume rejected";
```

### 14.13 按标签计算体积

整数标签精确比较，可明确选择 0；负标签记为不可观测。 体素体积不依赖先完成对齐。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `source/extent` | `DataRevisionRef、optional<array<int,6>>` | 源修订及含端点索引区域，省略区域为整卷。 |
| `storedValueRange/labels` | `灰度区间或整数集合` | 按本模式设置。 |
| `requireEnclosedSelection` | `bool` | 需要非空且不碰评价索引边界时设 true。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
VoxelVolumeRequest p; p.id = "material"; p.source = sourceRevision;
p.method = VoxelVolumeMethod::Labels;
p.labels = selectedLabels;
AlignmentRequest request; request.action = AlignmentAction::EvaluateVoxelVolume;
request.voxelVolume = p;
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << "Volume evaluation failed";
    // GetVoxelVolume(r.voxelVolumeResult)：volumeMm3 为空时不能用 0 代替。
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Volume rejected";
```

### 14.14 设置偏差显示

明确绑定偏差结果，设置色带或公差颜色，不重算测量。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `result/rangeMode/range` | `结果修订、AlignmentRangeMode、区间` | Result 或 Manual，区间同偏差定义和单位。 |
| `palette/useToleranceColors` | `AlignmentColorMode、bool` | 主动选色带或使用公差判定颜色。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentDeviationDisplay display;
display.result = deviationRevision;
display.rangeMode = AlignmentRangeMode::Manual; display.range = displayRange;
AlignmentRequest request; request.action = AlignmentAction::SetDeviationDisplay;
request.deviationDisplay = display;
// 本次请求成功不表示后续尺寸或偏差评价合格。
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (!r.isDisplayReady) qWarning() << "Deviation display not ready";
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Display rejected";
```

### 14.15 组合计量场景

以一个对齐结果为锚，组合多个实测/参考对象和尺寸项目。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `anchorAlignment/parts` | `DataRevisionRef、vector<AlignmentScenePart>` | 锚定结果和各部件的正式对齐结果；单位与目标坐标须一致。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentSceneDisplay display;
display.anchorAlignment = anchorResult;
display.parts = sceneParts;
AlignmentRequest request; request.action = AlignmentAction::SetSceneDisplay;
request.sceneDisplay = display;
// 本次请求成功不表示后续尺寸或偏差评价合格。
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (!r.isDisplayReady) qWarning() << "Metrology scene not ready";
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Scene rejected";
```

### 14.16 激活对齐结果

审核后激活明确结果。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `resultRef` | `DataRevisionRef` | 要操作的正式对齐结果。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentRequest request;
request.action = AlignmentAction::Activate;
request.resultRef = resultRevision;

// 本次请求成功不表示后续尺寸或偏差评价合格。
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << QString::fromStdString(r.message);
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Alignment action rejected";
```

### 14.17 停用对齐结果

移除当前激活关系。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `resultRef` | `DataRevisionRef` | 要操作的正式对齐结果。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentRequest request;
request.action = AlignmentAction::Deactivate;
request.resultRef = resultRevision;

// 本次请求成功不表示后续尺寸或偏差评价合格。
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << QString::fromStdString(r.message);
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Alignment action rejected";
```

### 14.18 控制对齐显示

只改变指定对齐结果的显示。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `resultRef` | `DataRevisionRef` | 要操作的正式对齐结果。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentRequest request;
request.action = AlignmentAction::SetVisibility;
request.resultRef = resultRevision;
request.isVisible = isVisible;
// 本次请求成功不表示后续尺寸或偏差评价合格。
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << QString::fromStdString(r.message);
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Alignment action rejected";
```

### 14.19 应用计量模型姿态

用绝对计量矩阵替换 Host 模型显示姿态，不重采样；当前显示数据须等于结果源体或源网格。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `resultRef` | `DataRevisionRef` | 要操作的正式对齐结果。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentRequest request;
request.action = AlignmentAction::ApplyModelPose;
request.resultRef = resultRevision;

// 本次请求成功不表示后续尺寸或偏差评价合格。
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    if (r.status != AlignmentStatus::FullyDetermined) qWarning() << QString::fromStdString(r.message);
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Alignment action rejected";
```

### 14.20 捕获计量报告

在所属线程的同一图快照捕获结果与显示配置，返回后可在后台只读使用。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `alignments/inspections/deviations/voxelVolumes` | `vector<DataRevisionRef>` | 要纳入报告的精确结果集合。 |
| `captureCurrentDisplay` | `bool` | 捕获当前显示配置；与显式显示配置互斥，不捕获屏幕像素。 |

出参与返回：GetReport() 同步返回 MetrologyReportResult，没有完成回调。FullyDetermined 且 report 非空表示捕获成功，不表示所有尺寸合格；InvalidInput/Unavailable/BudgetExceeded 等为输入、可用性或预算失败，读 message。

```cpp
MetrologyReportRequest request;
request.id = reportId; request.title = reportTitle;
request.alignments = {resultRevision};
request.inspections = inspectionRevisions;
request.captureCurrentDisplay = true;
// 冻结同一提交中的结果，捕获成功不等于尺寸合格。
const auto result = alignment->GetReport(request);
if (result.status != AlignmentStatus::FullyDetermined || !result.report)
    qWarning() << QString::fromStdString(result.message);
```

### 14.21 写出计量报告

使用冻结报告写出文件，适合宿主后台导出任务；父目录须存在，不覆盖已有目标。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `report/utf8Path` | `MetrologyReport、string` | 已捕获报告和 UTF-8 常规文件路径。 |
| `options` | `MetrologyReportWriteOptions` | Json/Csv 格式；必要时提供 isCancelled。 |

出参与返回：WriteMetrologyReportFile() 同步返回 MetrologyReportFileResult，不回调 Host。只有 isCommitted=true 表示文件已提交；write.status 的 InvalidInput/BudgetExceeded/Cancelled/IoError/AlreadyExists/ResourceExhausted 为输入、预算、取消、I/O、目标存在或资源失败。流式输出另有 WriteMetrologyReport()。

```cpp
MetrologyReportWriteOptions options;
options.format = MetrologyReportFormat::Json;
const auto result = WriteMetrologyReportFile(*report, utf8Path, options);
if (!result.isCommitted) {
    qWarning() << QString::fromStdString(result.write.message);
    return;
} // 此时才向用户报告导出完成。
```

### 14.22 取消计量任务

显式取消一个正在执行的请求，随后继续所属线程更新以完成收口。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `targetRequestId` | `uint64_t` | 启动请求 ID。 |

出参与返回：同步返回 AlignmentAdmission；Accepted 接纳，InvalidRequest/Busy/Unavailable 为请求、忙或可用性问题。非空回调在所属线程执行一次，可能同步触发，允许查询和再次请求。FullyDetermined 表示本次求解或发布成功；Underconstrained/Degenerate/Conflicting/Ambiguous/NotConverged 为欠约束、退化、冲突、歧义、不收敛，Cancelled/Stale/InvalidInput/Unavailable 为取消、过期输入、输入无效、不可用；DeadlineExceeded/BudgetExceeded/Unsupported/IoError/ResourceExhausted/InternalError 为截止时间、预算、不支持、I/O、资源或内部失败，诊断读 message。

```cpp
AlignmentRequest request;
request.action = AlignmentAction::Cancel;
request.targetRequestId = targetRequestId;
// 本次请求成功不表示后续尺寸或偏差评价合格。
const auto a = alignment->SendRequest(request, [](AlignmentResult r) {
    qInfo() << "Alignment terminal:" << static_cast<int>(r.status);
});
if (a.status != AlignmentAdmissionStatus::Accepted) qWarning() << "Cancel rejected";
```

## 15. 伪影抑制

仓内打开 MVVCVTK_BUILD_ARTIFACT_REDUCTION，链接 MVVCVTK::ArtifactReduction，包含 Host/ArtifactReductionHostFeature.h、ArtifactReductionHostTypes.h。所有请求和查询在挂载所属线程串行执行；该功能不注册视图，也不自动切换主绑定。

1. 准备源修订和可选处理、保护、质量 ROI。
2. 计算扩散或环校正候选，继续 Host 更新并观察状态。
3. 候选 Ready 后检查样本与质量，再明确提交或丢弃。
4. 发布成功后由宿主选择新图像，退出前收口并卸载。

### 15.1 挂载伪影功能

为候选计算和发布建立可选预算。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `ArtifactConfig` | `构造配置` | 必要时设置 memoryBudgetBytes/publishBudgetBytes 和停止等待。 |

出参与返回：AttachFeature() 同步返回 bool，没有完成回调；真表示挂载完成，假表示依赖或拓扑失败。

```cpp
artifact = std::make_shared<ArtifactReductionHostFeature>();
// 挂载失败停止后续操作，成功句柄由宿主持有。
if (!session->AttachFeature(artifact)) {
    qWarning() << "Artifact feature attach failed";
    return;
}
const auto state = artifact->GetState();
```

### 15.2 准备扩散滤波候选

扩散只产生候选，不立即替换当前图像。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `source/inputMode` | `DataRevisionRef、ArtifactInputMode` | CurrentPrimary 须匹配当前主卷，历史显式输入选 ExplicitRevision；三种 ROI 均须绑定该源。 |
| `diffusion` | `ArtifactDiffusionParams` | 主动设置 iterations、threshold、factor；threshold 为存储灰度/网格物理单位。 |
| `processingRoi/protectionRoi/qualityRoi` | `optional<DataRevisionRef>` | 分别限定处理、保护、评价，质量区不决定输出范围。 |

出参与返回：同步返回 ArtifactAdmission，没有完成回调。error=None 表示接纳或本次短动作成功；WrongThread/Busy/InvalidRequest/SourceChanged/TooLarge/TimedOut/CommitFailed 分别表示线程、忙、参数、来源、资源、超时或发布问题。后台结果由 Host 更新消费，再从 GetState().status/error 读取。

```cpp
ArtifactRequest p;
p.source = sourceRevision;
ArtifactDiffusionParams diffusion;
diffusion.iterations = iterations; diffusion.threshold = threshold;
diffusion.factor = factor; p.diffusion = diffusion;
ArtifactHostRequest request;
request.action = ArtifactAction::Prepare; request.prepare = p;
const auto a = artifact->SendRequest(request);
if (a.error != ArtifactError::None) qWarning() << "Diffusion rejected";
// 继续 Host 更新后观察 Ready，不在这里阻塞等待。
```

### 15.3 周期环校正候选

按网格轴和旋转中心准备环状伪影校正，中心不是世界坐标。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `axis/centerIndex` | `int、array<double,2>` | 网格轴 0～2；另外两轴按 XYZ 升序，包含原始 extent 偏移。 |
| `threshMin/threshMax/threshold` | `double` | 有限灰度区间和正阈值；默认未配置，必须主动填写。 |
| `strength/maxCorrection` | `double` | 强度 0～1，正的最大灰度校正量。 |

出参与返回：同步返回 ArtifactAdmission，没有完成回调。error=None 表示接纳或本次短动作成功；WrongThread/Busy/InvalidRequest/SourceChanged/TooLarge/TimedOut/CommitFailed 分别表示线程、忙、参数、来源、资源、超时或发布问题。后台结果由 Host 更新消费，再从 GetState().status/error 读取。

```cpp
ArtifactRingParams ring;
ring.axis = axis; ring.centerIndex = centerIndex;
ring.threshMin = grayMin; ring.threshMax = grayMax; ring.threshold = ringThreshold;
ring.strength = strength; ring.maxCorrection = maxCorrection;
ring.mode = ArtifactRingMode::Wrap;
ArtifactRequest p; p.source = sourceRevision; p.ring = ring;
ArtifactHostRequest request; request.action = ArtifactAction::Prepare; request.prepare = p;
// 本动作检查接纳，候选终态从后续状态读取。
const auto a = artifact->SendRequest(request);
if (a.error != ArtifactError::None) qWarning() << "Ring correction rejected";
```

### 15.4 反射边界环校正候选

按网格轴和旋转中心准备环状伪影校正，中心不是世界坐标。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `axis/centerIndex` | `int、array<double,2>` | 网格轴 0～2；另外两轴按 XYZ 升序，包含原始 extent 偏移。 |
| `threshMin/threshMax/threshold` | `double` | 有限灰度区间和正阈值；默认未配置，必须主动填写。 |
| `strength/maxCorrection` | `double` | 强度 0～1，正的最大灰度校正量。 |

出参与返回：同步返回 ArtifactAdmission，没有完成回调。error=None 表示接纳或本次短动作成功；WrongThread/Busy/InvalidRequest/SourceChanged/TooLarge/TimedOut/CommitFailed 分别表示线程、忙、参数、来源、资源、超时或发布问题。后台结果由 Host 更新消费，再从 GetState().status/error 读取。

```cpp
ArtifactRingParams ring;
ring.axis = axis; ring.centerIndex = centerIndex;
ring.threshMin = grayMin; ring.threshMax = grayMax; ring.threshold = ringThreshold;
ring.strength = strength; ring.maxCorrection = maxCorrection;
ring.mode = ArtifactRingMode::Reflect;
ArtifactRequest p; p.source = sourceRevision; p.ring = ring;
ArtifactHostRequest request; request.action = ArtifactAction::Prepare; request.prepare = p;
// 本动作检查接纳，候选终态从后续状态读取。
const auto a = artifact->SendRequest(request);
if (a.error != ArtifactError::None) qWarning() << "Ring correction rejected";
```

### 15.5 检查候选状态与样本

宿主只在任务待完成时观察状态，到终态停止观察；GetState 不会等待或推进计算。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `requestId/firstVoxel/stride/maxCount` | `ID 与 size_t` | 只读当前 Ready 候选；stride>0，maxCount 1～4096。 |

出参与返回：GetState() 同步返回 ArtifactState，无回调。Running/Cancelling/Ready/Publishing/Failed 为计算、取消、可检查、发布或失败；错误读 error。GetCandidateSamples() 返回可选样本对集合，空表示状态、参数或数据不符；不发布候选。

```cpp
const auto state = artifact->GetState();
if (state.status == ArtifactStatus::Ready) {
    const auto samples = artifact->GetCandidateSamples(state.requestId, 0, 1, 256);
    if (!samples) qWarning() << "Candidate samples unavailable";
} else if (state.status == ArtifactStatus::Failed) {
    qWarning() << "Artifact failed:" << static_cast<int>(state.error);
}
// 质量统计不能自行证明 fidelityVerified；需要独立参考或计量证据。
```

### 15.6 提交校正候选

用户确认后发布新体数据，宿主随后用 HostDataSelectRequest 明确选择它。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `requestId` | `uint64_t` | 当前 Ready 候选 ID；只有 Commit 动作填此字段。 |

出参与返回：同步返回 ArtifactAdmission，没有完成回调。error=None 表示接纳或本次短动作成功；WrongThread/Busy/InvalidRequest/SourceChanged/TooLarge/TimedOut/CommitFailed 分别表示线程、忙、参数、来源、资源、超时或发布问题。后台结果由 Host 更新消费，再从 GetState().status/error 读取。

```cpp
const auto state = artifact->GetState();
if (state.status != ArtifactStatus::Ready) return;
ArtifactHostRequest request;
request.action = ArtifactAction::Commit; request.requestId = state.requestId;
const auto a = artifact->SendRequest(request);
if (a.error != ArtifactError::None) qWarning() << "Artifact commit failed";
const auto published = artifact->GetState().correctedVolume; // 有值后再选择新图像。
```

### 15.7 取消校正计算

请求协作取消正在计算的候选，等待更新后的终态。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `ArtifactAction` | Cancel；requestId 保持 0，不附 prepare。 |

出参与返回：同步返回 ArtifactAdmission，没有完成回调。error=None 表示接纳或本次短动作成功；WrongThread/Busy/InvalidRequest/SourceChanged/TooLarge/TimedOut/CommitFailed 分别表示线程、忙、参数、来源、资源、超时或发布问题。后台结果由 Host 更新消费，再从 GetState().status/error 读取。

```cpp
ArtifactHostRequest request;
request.action = ArtifactAction::Cancel;
const auto a = artifact->SendRequest(request);
if (a.error != ArtifactError::None) qWarning() << "Artifact action rejected";
const auto state = artifact->GetState(); // 后续生命周期仍由 Host 更新推进。
```

### 15.8 丢弃校正候选

丢弃尚未发布的候选；处理中应先取消并完成收口。

入参：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `action` | `ArtifactAction` | Discard；requestId 保持 0，不附 prepare。 |

出参与返回：同步返回 ArtifactAdmission，没有完成回调。error=None 表示接纳或本次短动作成功；WrongThread/Busy/InvalidRequest/SourceChanged/TooLarge/TimedOut/CommitFailed 分别表示线程、忙、参数、来源、资源、超时或发布问题。后台结果由 Host 更新消费，再从 GetState().status/error 读取。

```cpp
ArtifactHostRequest request;
request.action = ArtifactAction::Discard;
const auto a = artifact->SendRequest(request);
if (a.error != ArtifactError::None) qWarning() << "Artifact action rejected";
const auto state = artifact->GetState(); // 后续生命周期仍由 Host 更新推进。
```

## 接口索引

下表给出章节所依赖的公开头及仓内调用对照。安装路径相对开发包 `include/`；仓内路径相对仓库根目录。源码形式头只供相应仓内目标消费，不表示已经安装到 SDK。

| 章节 | 公开入口与类型头 | 仓内调用或契约对照 |
| --- | --- | --- |
| 1 会话与嵌入 | `MVVCVTK/API/Host/VtkAppHostSession.h`、`MVVCVTK/API/Host/Types/HostSessionTypes.h` | `test/QtHost/Manual/App/TestWindow.cpp`、`QtHostPump.cpp`；首帧附件见 `CT/MVVCVTK/c_ui/viewport/ViewportGather.cpp` |
| 2 数据加载 | `MVVCVTK/API/Host/Types/HostRequestTypes.h`、`HostValueTypes.h`、`MVVCVTK/API/Data/ImageReadTypes.h` | `test/QtHost/QtHostLoadTests.cpp`、`CT/MVVCVTK/c_ui/context/SessionManager.cpp` |
| 3 视图模式 | 同目录 `HostRequestTypes.h`、`HostValueTypes.h`、`HostViewTypes.h` | `test/QtHost/QtHostViewTests.cpp` |
| 4 显示参数 | 同目录 `HostValueTypes.h`、`HostSessionTypes.h` | `test/QtHost/Manual/Modules/ViewTest.cpp`、`test/QtHost/QtHostViewTests.cpp` |
| 5 导航与输入 | `MVVCVTK/API/Interaction/InteractionController.h`、`ViewNavigation.h`、`NavigationTypes.h`、`InputBindings.h`；Host 的 `HostRequestTypes.h` | `test/Interaction/InteractionControllerTests.cpp`、`HostInputRegistryTests.cpp` |
| 6 读取与导出 | `MVVCVTK/API/Data/ImageReadTypes.h`、`DataGraphTypes.h`；Host 的请求与状态类型 | `test/QtHost/Manual/Modules/DataTest.cpp`、`test/QtHost/QtHostLabelMapTests.cpp`、`QtHostExportTests.cpp` |
| 7 ROI 正式数据 | `MVVCVTK/API/Data/RoiTypes.h`、会话入口 | `test/RoiIntegration/RoiIntegrationTests.cpp`、`test/QtHost/HostConsumerContractTests.cpp` |
| 7 ROI 交互编辑 | 仓内 `MVVCVTK/features/RoiEditing/include/Host/RoiEditingHostFeature.h` | `test/QtHost/Manual/Modules/RoiTest.cpp`、`test/RoiEditing/RoiEditingTests.cpp` |
| 8 裁切 | `MVVCVTK/API/Features/OrthogonalCrop/Host/CropHostFeature.h`、同组件 `OrthogonalCropTypes.h` | `test/QtHost/QtHostCropTests.cpp`、`CT/MVVCVTK/c_ui/context/SessionManager.cpp` |
| 9 表面测定 | 仓内 `MVVCVTK/features/SurfaceDetermination/include/Host/SurfaceDeterminationHostFeature.h`、`SurfaceDeterminationHostTypes.h`；该 include 根下 `SurfaceRecipe.h` | `test/QtHost/Manual/Modules/SurfaceTest.cpp` |
| 10 零件分割 | 仓内 `MVVCVTK/features/PartSegmentation/include/Host/PartSegmentationHostFeature.h`、`PartSegmentationHostTypes.h` | `test/QtHost/Manual/Modules/PartTest.cpp`、`PartEditTest.cpp` |
| 11 孔隙分析 | `MVVCVTK/API/Features/GapAnalysis/Host/GapHostFeature.h`、`GapHostTypes.h` | `test/QtHost/Manual/Modules/GapTest.cpp`、`test/QtHost/QtHostGapTests.cpp` |
| 12 壁厚 | 仓内 `MVVCVTK/features/WallThickness/include/Host/WallThicknessHostFeature.h`、`WallThicknessHostTypes.h` | `test/QtHost/Manual/Modules/WallTest.cpp`、`test/WallThickness/ThicknessSessionTests.cpp` |
| 13 模型旋转 | 仓内 `MVVCVTK/features/ModelRotation/include/Host/ModelRotationHostFeature.h`、`ModelRotationHostTypes.h` | `test/QtHost/Manual/Modules/RotationTest.cpp` |
| 14 计量与报告 | `MVVCVTK/API/Features/MetrologyAlignment/Host/MetrologyAlignmentHostFeature.h`、`MetrologyAlignmentHostTypes.h`、`MetrologyReport.h`、`VoxelVolume.h` | `test/QtHost/Manual/Modules/AlignmentTest.cpp`、`AlignmentInspection.cpp` |
| 15 伪影抑制 | 仓内 `MVVCVTK/features/ArtifactReduction/include/Host/ArtifactReductionHostFeature.h`、`ArtifactReductionHostTypes.h` | `test/QtHost/Manual/Modules/ArtifactTest.cpp` |

Host 和已安装 Feature 的头路径映射见 `MVVCVTK/CMakeLists.txt`；各功能是否构建、公开源码头及是否安装，以 `cmake/MVVCVTKBuildPolicy.cmake` 和相应功能的 `CMakeLists.txt` 为准。内部算法、私有服务、供应商 bridge 和渲染策略没有在这里作为独立宿主入口列出。
