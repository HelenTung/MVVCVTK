# MVVCVTK Qt Host API 使用手册

本文面向 Qt 5.14.2 Widgets 上位机，说明安装态 SDK 的公开头、Session 生命周期、主体请求、
Crop/Gap Feature 与 QVTK 关闭流程。当前核验基线为 2026-09-02 的 Gap 结果复用实现；接口签名以
公开头为准，异步流程以所有者定时器和结构化结果回调为准。

## 1. 公共约定

### 1.1 公开头文件

| 安装态头文件 | 公开内容 |
| --- | --- |
| `MVVCVTK/API/Host/VtkAppHostSession.h` | Session 入口：生命周期、请求、状态、endpoint 与图像读取 |
| `MVVCVTK/API/Host/Types/HostRequestTypes.h` | 九种主体请求、`HostResult` 与结果回调 |
| `MVVCVTK/API/Host/Types/HostSessionTypes.h` | Session、停止状态、视图、Timer/Hotkey 与所有者任务投递器配置 |
| `MVVCVTK/API/Host/Types/HostValueTypes.h` | 导出格式、scalar TF、模式、材质和质量等值类型 |
| `MVVCVTK/API/Host/Types/HostViewTypes.h` | 单/多视图目标 DTO |
| `MVVCVTK/API/Data/ImageReadTypes.h` | 普通调用方的图像读取 request/result/chunk 类型 |
| `MVVCVTK/API/Features/OrthogonalCrop/Host/CropHostFeature.h` | Crop Feature 入口、配置、请求、状态和回调 |
| `MVVCVTK/API/Features/GapAnalysis/Host/GapHostFeature.h` | Gap Feature 入口、配置、请求、状态和回调 |
| `MVVCVTK/SPI/Host/HostFeature.h` | 仅供自定义 Feature 实现者使用的扩展契约 |

SDK 项目头统一位于 `include/MVVCVTK/`，物理分为 `API/` 与 `SPI/`。普通 Qt 调用方使用
API 头；只有实现自定义 Feature 时才直接依赖 `MVVCVTK::FeatureSPI`。源码树仍保留
`Host/...` 等原路径，CMake staging 会把批准头及其物理闭包改写到安装态路径。内部
Algorithm、Service、主体 Strategy、Router、view context 和具体 VTK 实现不进入 SDK。

普通 Qt consumer 的入口 include：

```cpp
#include <MVVCVTK/API/Host/VtkAppHostSession.h>
#include <MVVCVTK/API/Features/OrthogonalCrop/Host/CropHostFeature.h>
#include <MVVCVTK/API/Features/GapAnalysis/Host/GapHostFeature.h>
```

### 1.2 调用线程

- Session 构建、主体请求、同步图像读取、Feature 请求和状态读取都必须在创建 Session 的
  Qt GUI/VTK 所有者线程（owner thread）调用。
- Qt 只进入 `QApplication::exec()`，不调用 `VtkAppHostSession::Start()` 或
  `vtkRenderWindowInteractor::Start()`。
- `HostSessionConfig::sendOwnerTask` 负责把跨线程析构产生的 Stop 任务投递回所有者事件循环；
  普通业务请求不会自动切换线程。
- 回调更新 Qt 对象时，调用方使用 `QPointer` 和 `Qt::QueuedConnection`，并允许同步请求的
  结果回调在发送函数返回前执行。
- Feature 异步完成、异步图像读取回调和退出收口都依赖 QVTK `TimerEvent` 持续到达。

### 1.3 返回值与回调

主体新代码统一使用 `SendRequestResult()`。它把“请求是否接纳”和“操作最终结果”拆开：

| 返回或回调 | 含义 |
| --- | --- |
| `bool == true` | 请求已被识别并接纳；不代表最终成功 |
| `bool == false` | 请求未接纳；非空结果回调仍会同步执行一次 |
| `HostResultCallback` | 必填；每个请求恰好执行一次，携带 `HostResult` |
| `HostCompleteCallback` | 旧 `SendRequest()` 的兼容回调；新 Qt 代码不再使用 |
| `CropBuildCallback` | 只供 Crop `BuildResult` 使用 |
| `GapHostCallback` | 只供 Gap `Start` 使用；`true` 表示结果已消费且至少一个 overlay 挂载成功 |

`HostResult` 包含 `isSucceeded`、`errorCode` 和 `message`：

| `HostErrorCode` | 含义 |
| --- | --- |
| `None` | 操作成功 |
| `SessionNotReady` | Session 无法构建或已经不可用 |
| `WrongThread` | 请求不在 Session 所有者线程 |
| `RequestRejected` | 请求在执行前被拒绝或类型未知 |
| `OperationFailed` | 已识别操作执行失败或抛出异常 |

同步 View/Session/Tool 请求的结果回调可能在 `SendRequestResult()` 返回前执行；已接纳的
Load、Reload、Export 请求则在所有者定时器消费终态后回调。可复用的 Qt 结果回调：

```cpp
const QPointer<QMainWindow> owner(this); // 弱引用 Qt 窗口，避免异步完成后访问已析构对象。
const HostResultCallback onHostResult = // 保存主体请求共用的结构化结果回调。
    [owner](HostResult result) { // 同步拒绝/完成或异步终态都进入同一入口。
        if (!owner) { // 检查 Qt 对象是否仍然存活。
            return; // 对象已销毁时丢弃本次 UI 更新。
        }
        QMetaObject::invokeMethod( // 把 UI 更新排队到 owner 所在线程。
            owner.data(), // 指定接收 queued invocation 的 Qt 对象。
            [owner, result = std::move(result)]() mutable { // 移动保存完整结果。
                if (owner) { // 执行排队任务前再次检查对象生命期。
                    qInfo() // 在 Qt 线程消费成功位、错误码和消息。
                        << "Host completed:"
                        << result.isSucceeded
                        << static_cast<int>(result.errorCode)
                        << QString::fromStdString(result.message);
                }
            },
            Qt::QueuedConnection); // 明确使用排队调用，不直接跨线程更新 UI。
    };
```

### 1.4 路径编码

所有 Host 路径字段使用 UTF-8 owning `std::string`：

```cpp
const std::string inputPath = // 创建拥有自身存储的 UTF-8 路径。
    sourcePath.toUtf8().toStdString(); // 从 QString 转换，不使用本地 ACP 编码。
```

### 1.5 视图目标

`HostViewTarget` 的字段：

| 字段             | 类型                 | 说明                                |
| ---------------- | -------------------- | ----------------------------------- |
| `viewId`         | `std::string`        | 非空时优先按稳定 id 查找            |
| `isViewRoleUsed` | `bool`               | `viewId` 为空时，是否启用 role 查找 |
| `viewRole`       | `HostRenderViewRole` | role 查找使用的角色                 |

按 id 选择视图：

```cpp
const HostViewTarget primary3D{ // 定义后续请求复用的主 3D 目标。
    "primary-3d", // 使用 Session 中唯一且稳定的 view id。
    false, // 禁止 role 回退；id 未命中时请求直接失败。
    HostRenderViewRole::Primary3D // 该字段在 isViewRoleUsed=false 时不参与查找。
};
const HostViewTarget topDown{ // 定义俯视切片目标。
    "slice-top-down", // 指定俯视切片的稳定 view id。
    false, // 只按 id 查找。
    HostRenderViewRole::TopDownSlice // 保留目标的业务角色说明。
};
```

按 role 选择 topology 中第一个匹配视图：

```cpp
const HostViewTarget firstTopDown{ // 创建 role 选择器。
    "", // 留空 id，允许进入 role 分支。
    true, // 显式启用 viewRole。
    HostRenderViewRole::TopDownSlice // 选择 topology 中第一个俯视切片。
};
```

选择规则：

1. `viewId` 非空时只按 id 查找；id 未命中不会回退到 role。
2. `viewId` 为空且 `isViewRoleUsed == true` 时才按 role 查找。
3. 两者都未指定时是空目标。
4. `HostViewTargets` 按 Session topology 顺序返回 id/role 的去重并集；空集合不是全选。
5. View、Tool 和显式 Slice Export 要求有效目标；Data Export 的空 `sourceView` 会回退
   到首选数据视图。Load/Reload 没有目标字段，由 Session 的数据路由选择数据服务。

### 1.6 版本化 MVVCVTK SDK 与显示算法边界

本项目提供由 Qt 上位机调用的显示算法和 Host API。`examples/standalone/main.cpp` 是默认进入
Visual Studio Solution 的本地组合程序，便于直接修改、启动和观察效果；它不进入三个产品静态库，
也不进入 SDK 安装。源码和测试只有一套构建真相：

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-debug
cmake --build --preset vs2026-release
ctest --preset vs2026-debug
ctest --preset vs2026-release
```

第一条命令在 `out/build/vs2026-x64/MVVCVTK.slnx` 生成 Visual Studio 2026 Solution；仓库根不
保留手写 Solution 或产品/测试 vcxproj。SDK 发布脚本安装 CMake 生成的三个独立模块：

```text
<version>-win-x64/
  include/MVVCVTK/API/             # 普通 Qt/Host API 与 Feature 入口
  include/MVVCVTK/SPI/             # 自定义 Feature 扩展契约
  lib/Debug/                       # Host/Crop/Gap 三个 /MDd 静态库
  lib/Release/                     # Host/Crop/Gap 三个 /MD 静态库
  lib/cmake/MVVCVTK/               # Host API、Feature SPI 与模块 package config
  deps/third_party/defx/bin/Debug/ # MVVCVTKGapKernel.dll + DefXAnalysis.dll
  deps/third_party/defx/bin/Release/ # MVVCVTKGapKernel.dll + DefXAnalysis.dll
  deps/official/vtk/               # 锁定的 VTK 编译与运行环境
  deps/official/opencv/            # 锁定的 OpenCV 编译与运行环境
```

三个实体库分别是 `MVVCVTKHost.lib`、`MVVCVTKOrthogonalCrop.lib`、
`MVVCVTKGapAnalysis.lib`；应用通过 CMake 显式链接所需模块，不存在继承 standalone 组合的
`MVVCVTK::SDK` target。SDK 不提供单体 `MVVCVTK.lib`、产品 DLL 或测试库。

Qt/CMake 上位机使用 `find_package(MVVCVTK CONFIG REQUIRED)` 并显式链接所需目标。只编译
Host API 或 Feature SPI 契约的 target 可分别链接 `MVVCVTK::HostAPI`、
`MVVCVTK::FeatureSPI`；实际运行功能仍显式链接 `MVVCVTK::Host`、
`MVVCVTK::OrthogonalCrop`、`MVVCVTK::GapAnalysis`。

只使用 Host 与 GapAnalysis 的上位机最小 CMake 配置：

```cmake
find_package(
    MVVCVTK
    CONFIG REQUIRED
    COMPONENTS Host GapAnalysis
)

target_link_libraries(
    UpperMachine
    PRIVATE
        MVVCVTK::Host
        MVVCVTK::GapAnalysis
)
```

`UpperMachine` 替换为 Qt 上位机自己的 target。若还使用 Crop，再把 `OrthogonalCrop` 加入
`COMPONENTS`，并链接 `MVVCVTK::OrthogonalCrop`。不要链接不存在的 `MVVCVTK::SDK`，也不要
让上位机直接链接 DefX 头、import library 或内部 Gap kernel target。安装态
`MVVCVTKConfig.cmake` 会从 SDK 自带依赖目录解析锁定的 OpenCV 与 VTK package；只有上位机
自身直接调用 OpenCV API 时才额外链接 `opencv_world`，不要改用系统中其它版本的同名依赖。

原生 CT `.vcxproj` 不转换为 CMake，也不在工程文件中写版本化 SDK 目录或逐项维护
VTK/OpenCV 库清单。CT 维护并提交稳定属性表，由它把环境变量 `MVVCVTK_SDK_ROOT`
映射为内部属性 `MVVCVTKSdkRoot`：

```xml
<Import Project="MVVCVTK.Sdk.props"
        Condition="'$(Platform)'=='x64' And Exists('MVVCVTK.Sdk.props')" />
```

`MVVCVTK.Sdk.props` 只消费 SDK 已有的 `include`、`lib/<配置>`、VTK 和 OpenCV 目录，按
目录模式发现当前配置的头、库与运行时，不嵌入版本号或逐个第三方文件名。当前 CT 只接入
Host 与 OrthogonalCrop；GapAnalysis 仍是 SDK 的可选模块，CT 未启用对应功能时不链接也不
部署它。替换 SDK 时只设置 `MVVCVTK_SDK_ROOT`，临时或 CI 构建也可传入
`/p:MVVCVTKSdkRoot=<SDK 根目录>`；工程不使用 `Local.props`，也不自动扫描并选择最新 SDK。
CT 自有依赖从项目目录解析 `../libs/include` 与 `../libs/lib`，不依赖 `SolutionDir`。Qt 仍由
CT/宿主通过 Qt VS Tools、`QtMsBuild` 与 `QtInstall` 提供，并由 QtMsBuild 的部署目标处理；
Qt 本体和插件不进入 MVVCVTK SDK。仓库发布流程仍在仓库外动态生成一次性 CMake
clean-room probe，验证安装态 MVVCVTK package。

`UIReconstruct3D` 与 `UIPhantomCalib` 是 CT 自有二进制依赖，不属于 MVVCVTK SDK。
CT 工程始终从 `../libs/include` 与 `../libs/lib` 编译和链接传统重建，并在构建后复制
`../libs/lib` 中的全部 DLL；因此新增 CT 运行时只需放入该目录，不需要修改 SDK 属性表或
逐个维护复制文件名。该二进制链仍要求调用方提供与其匹配的 OpenCV 4.10、CUDA 11、TIFF
和 Qt 运行时，不能用 SDK 的 OpenCV 4.12 或其他版本 CUDA 通过改名替代。正式运行验收以
`Release|x64` 为准；CT 运行时与 SDK/Qt 出现同名 DLL 时，工程会报依赖来源冲突而不静默覆盖。

公开头以 CMake `FILE_SET` 精确安装，不复制源码 include 整树。闭包中的 `RenderEffect` 等类型是
入口公开签名需要的抽象能力；其中 VTK 指针只表示非拥有框架端口，不向 Qt 暴露内部 adapter、
service、strategy 或其生命周期。

### 1.7 GapAnalysis 私有运行时

`DefX::Analysis` 只存在于源码构建树，且仅由私有模块 `mvvcvtk_gap_kernel` 以 `PRIVATE`
方式链接。安装后的 CMake package、consumer target 图和公开头闭包不暴露该 target、
`MVVCVTK_DEFX_ROOT`、DefX 供应商头文件或导入库；Qt 调用方仍只需链接
`MVVCVTK::GapAnalysis`。

SDK 只把 `MVVCVTKGapKernel.dll` 与 `DefXAnalysis.dll` 成对安装到
`deps/third_party/defx/bin/<配置>`。运行含 GapAnalysis 的 Qt 宿主时，将
`MVVCVTK_GAP_RUNTIME_DIR` 设为该配置目录的绝对路径。显式目录非法时 worker 会把本批
分析收口为失败，不会回退到其它搜索来源；未设置时只接受进程可执行文件同目录中的成对 DLL。GapAnalysis
不从 PATH 或当前工作目录发现这两个模块，通用 `MVVCVTK_RUNTIME_DIRS` 也不注入 DefX 目录。

配置必须匹配：Debug 上位机指向 `<SDK>/deps/third_party/defx/bin/Debug`，Release 上位机指向
`<SDK>/deps/third_party/defx/bin/Release`，不能交叉使用。上位机也可以把对应配置的两个 DLL
一起复制到自身 `.exe` 目录，此时不设置 `MVVCVTK_GAP_RUNTIME_DIR`；两种部署方式只选一种。

## 2. Session API

### 2.1 创建 Session 与 `BuildSession`

| 项目     | 内容                                                     |
| -------- | -------------------------------------------------------- |
| 接口     | `explicit VtkAppHostSession(HostSessionConfig config);`  |
| 接口     | `bool BuildSession();`                                   |
| 说明     | 构造并幂等构建一次 Host 会话                             |
| 适用场景 | QVTK widget 已绑定外部 `vtkGenericOpenGLRenderWindow` 后 |
| 调用线程 | Qt GUI/VTK 所有者线程                                    |

**参数：**

| 参数                         | 类型                               | 必填 | 说明                                                         |
| ---------------------------- | ---------------------------------- | ---- | ------------------------------------------------------------ |
| `config`                     | `HostSessionConfig`                | 是   | `renderViews` 非空；声明顺序即 topology 顺序                 |
| `renderViews[].id`           | `std::string`                      | 是   | Session 内非空且唯一                                         |
| `renderViews[].role`         | `HostRenderViewRole`               | 否   | 默认 Auxiliary；业务视图建议显式指定                         |
| `renderViews[].renderWindow` | `vtkSmartPointer<vtkRenderWindow>` | 否   | Qt 复用 QVTK 外部 window 时提供同一实例；为空时 Session 自建 |
| `renderViews[].isEventLoopEnabled` | `bool` | 否 | 仅 standalone `Start()` 使用；Qt 保持 `false` |
| `sendOwnerTask` | `std::function<bool(std::function<void()>)>` | 建议 | 把最终 Stop 投递回 Qt 所有者事件循环 |
| `sendDiagnostic` | `std::function<void(const std::string&)>` | 否 | 立即消费生命周期诊断；不得抛异常或缓存 message 引用 |

**返回值：**

- `true`：首次构建成功，或同一所有者线程上的重复构建复用既有 Session。
- `false`：视图为空、id/配置非法、构建失败或调用线程不正确。

**调用示例：**

```cpp
QSurfaceFormat::setDefaultFormat( // 设置 QVTK 所需的默认 OpenGL surface format。
    QVTKOpenGLNativeWidget::defaultFormat()); // 此调用必须早于 QApplication 构造。
QApplication app(argc, argv); // 创建 Qt 唯一事件循环。

auto primaryWindow = // 创建主 3D 视图使用的 GenericOpenGL window。
    vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New(); // 分配一个新的 VTK OpenGL window。
auto primaryWidget = // 创建承载主 3D 视图的 QVTK widget。
    std::make_unique<QVTKOpenGLNativeWidget>(); // 由 Qt adapter 独占 widget 生命周期。
primaryWidget->setRenderWindow(primaryWindow); // 先把 window 交给 QVTK widget。

HostRenderViewConfig primaryView; // 创建一个远端 Host 视图配置。
primaryView.id = "primary-3d"; // 设置 Session 内唯一且稳定的 view id。
primaryView.role = HostRenderViewRole::Primary3D; // 声明该视图是主 3D 视图。
primaryView.window.title = "Primary 3D"; // 设置 Host 侧可写入的窗口标题。
primaryView.window.viewInit.viewMode = // 设置首次构建使用的渲染模式。
    HostRenderMode::CompositeIsoSurface; // 初始化为组合等值面模式。
primaryView.renderWindow = primaryWindow; // 注入与 QVTK widget 相同的 window 实例。

HostSessionConfig config; // 创建 Session 一次性配置。
config.renderViews.push_back( // 按 topology 顺序加入主视图。
    std::move(primaryView)); // 配置进入 Session 后不再复用原对象。
config.sendOwnerTask = [](std::function<void()> task) { // 配置 Qt 所有者线程投递器。
    auto* appInstance = QCoreApplication::instance(); // 取得归属 GUI 线程的事件循环对象。
    if (!appInstance || !task) { // 事件循环或任务无效时拒绝投递。
        return false;
    }
    QTimer::singleShot( // 下一轮 Qt 事件循环在 appInstance 所在线程执行任务。
        0,
        appInstance,
        [ownerTask = std::move(task)]() mutable {
            ownerTask();
        });
    return true;
};
config.sendDiagnostic = [](const std::string& message) { // 立即复制并写入线程安全日志。
    qWarning().noquote() << QString::fromStdString(message);
};

auto session = // Qt 以 unique_ptr 强持有整个 Host Session。
    std::make_unique<VtkAppHostSession>(std::move(config)); // 移交一次性 Session 配置。
const bool isBuilt = session->BuildSession(); // 在所有者线程显式构建 Session。
if (!isBuilt) { // 检查构建结果。
    return false; // 构建失败时停止后续 Host 调用。
}
```

### 2.2 `AttachTimer`

| 项目     | 内容                                                         |
| -------- | ------------------------------------------------------------ |
| 接口     | `bool AttachTimer(const HostTimerConfig& config);`           |
| 说明     | 在指定视图的 context 上绑定或卸载 Host timer hook            |
| 适用场景 | Crop/Gap 异步完成、Feature tick 和退出收口                   |
| 前置条件 | Session 可成功构建；启用时 `targetView` 必须命中有效 context |

**参数：**

| 字段             | 类型             | 必填     | 说明                               |
| ---------------- | ---------------- | -------- | ---------------------------------- |
| `isTimerEnabled` | `bool`           | 是       | `true` 绑定；`false` 卸载当前 hook |
| `targetView`     | `HostViewTarget` | 启用时是 | timer 事件来源视图                 |

**返回值：**

- `true`：hook 已绑定或已卸载。
- `false`：Session/线程/目标 context 无效。

`true` 只证明 Host hook 已绑定，不证明底层 repeating timer 已创建；Qt 仍需确认
QVTK interactor 持续产生 `TimerEvent`。

**启用调用示例：**

```cpp
HostTimerConfig timer; // 创建 Host timer 配置。
timer.isTimerEnabled = true; // 请求绑定 Host timer hook。
timer.targetView = primary3D; // 使用有效主视图作为 TimerEvent 来源。
const bool isTimerAttached = // 保存立即绑定结果。
    session->AttachTimer(timer); // 把 Feature tick 接入现有 QVTK timer。
```

**卸载调用示例：**

```cpp
HostTimerConfig timer; // 创建用于卸载的 timer 配置。
timer.isTimerEnabled = false; // false 表示移除当前 Host timer hook。
const bool isTimerDetached = // 保存立即卸载结果。
    session->AttachTimer(timer); // targetView 在卸载分支不参与选择。
```

### 2.3 `AttachHotkeys`

| 项目     | 内容                                                  |
| -------- | ----------------------------------------------------- |
| 接口     | `bool AttachHotkeys(const HostHotkeyConfig& config);` |
| 说明     | 替换 standalone VTK interactor 的主体热键入口         |
| 适用场景 | standalone 或显式保留 VTK 调试热键的宿主              |
| Qt 约束  | 生产 Qt action 直接发送具体 Request，通常不调用本接口 |

**参数：**

`HostHotkeyConfig` 可配置 context 输入、主体命令输入、工具切换键、数据/切片导出键、
退出键以及对应导出路径和来源视图。

**返回值：**

- `true`：热键配置已接入。
- `false`：Session 构建、所有者线程或输入配置无效。

**调用示例：**

```cpp
HostHotkeyConfig hotkeys; // 创建 standalone 调试热键配置。
hotkeys.isContextInputEnabled = true; // 启用视图内工具输入。
hotkeys.contextInputViews.viewIds = { "primary-3d" }; // 限定接收热键的视图。
hotkeys.modelSwitchKey = 'm'; // 设置 Navigation/ModelTransform 切换键。
const bool isHotkeyAttached = // 保存立即接入结果。
    session->AttachHotkeys(hotkeys); // 替换既有主体热键 observer。
```

### 2.4 `AttachFeature`

| 项目     | 内容                                                               |
| -------- | ------------------------------------------------------------------ |
| 接口     | `bool AttachFeature(const std::shared_ptr<HostFeature>& feature);` |
| 说明     | 把一个 Feature 挂载到已经构建的 Session                            |
| 适用场景 | 接入 `CropHostFeature`、`GapHostFeature` 或其它 Host Feature       |
| 前置条件 | 必须先 `BuildSession()`；Feature id 非空且未重复                   |

**参数：**

| 参数      | 类型                           | 必填 | 所有权                                    |
| --------- | ------------------------------ | ---- | ----------------------------------------- |
| `feature` | `std::shared_ptr<HostFeature>` | 是   | Session 强持有到 Detach/Stop 成功；Qt 保留句柄以发送请求 |

**返回值：**

- `true`：Feature 已挂载。
- `false`：Session 未构建、线程错误、Feature 无效、id 重复或 Feature 拒绝挂载。

**调用示例：**

```cpp
auto crop = std::make_shared<CropHostFeature>( // Qt 创建并强持有 Crop Feature。
    std::move(cropConfig)); // 把一次性 Crop 配置移交给 Feature。
const bool isCropAttached = // 保存立即挂载结果。
    session->AttachFeature(crop); // Session 强持有 Feature，并建立输入/视图绑定。
```

### 2.5 `DetachFeature`

| 项目     | 内容                                              |
| -------- | ------------------------------------------------- |
| 接口     | `bool DetachFeature(const HostFeature& feature);` |
| 说明     | 从 Session 解绑一个已挂载 Feature                 |
| 适用场景 | 单个 Feature 停用或替换；完整窗口关闭使用 `Stop()` |
| 前置条件 | 所有者线程；Feature 已挂载                        |

**返回值：**

- `true`：Feature 已完成解绑。
- `false`：Session/线程/Feature 状态不满足解绑条件；调用方继续保留句柄，并可在所有者线程
  修复后重试。Session 会先退休该 Feature 的活动视图，再调用其解绑钩子；失败时保留强注册。

**调用示例：**

```cpp
const bool isCropDetached = // 保存 Crop 解绑结果。
    session->DetachFeature(*crop); // 先让 Session 移除输入和 Feature 注册。
if (isCropDetached) { // 只在解绑成功后释放 Qt 强引用。
    crop.reset(); // 销毁或释放 Qt 持有的 Crop Feature。
}
```

### 2.6 `Start`

| 项目     | 内容                                               |
| -------- | -------------------------------------------------- |
| 接口     | `bool Start();`                                    |
| 说明     | 渲染全部视图并进入阻塞的 standalone VTK event loop |
| 适用场景 | 仅 standalone executable                           |
| Qt 约束  | Qt 禁止调用；Qt 使用 `QApplication::exec()`        |

**返回值：**

- `true`：standalone 循环正常退出。
- `false`：Session、启动视图或重复启动状态无效。

**standalone 调用示例：**

```cpp
const bool isStarted = session->Start(); // 仅 standalone 进入阻塞 VTK event loop。
```

### 2.7 `Stop` 与 pending Stop

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool Stop() noexcept;` |
| 查询 | `GetIsStopped()`、`GetStopState()` |
| pending fallback | `SendPendingStops()`、`GetPendingStopCount()` |
| 调用线程 | Session 所有者线程 |

`Stop()` 是幂等且可重试的显式停止入口。它依次解绑全部 Feature、停止 view lease
（包含 timer、executor 和 observer）、兑现已接纳图像读取的唯一终态回调，再清理 endpoint、
Hotkey、Router 和共享服务。

| `HostStopState` | 含义 |
| --- | --- |
| `Stopped` | runtime aggregate 已完整停止，可安全释放外部句柄 |
| `Building` / `Running` | 正在构建 / 正常运行 |
| `StopRequested` | 非所有者线程调用了 `Stop()`，尚未执行 VTK 清理 |
| `Stopping` | 所有者线程正在执行停止流程 |
| `StopPending` | 某一停止阶段失败；Session 保留完整状态供后续重试 |

所有者线程直接调用 `Stop()`；返回 `false` 时不得释放 Session、Feature、QVTK widget 或外部
render window，应在修复暂态失败后再次调用同一 Session 的 `Stop()`。如果 Session
析构时仍未完成 Stop（尤其发生在错误线程），析构函数会通过 `sendOwnerTask` 投递任务，并由
内部 reaper 持有 runtime；
dispatcher 缺失、拒绝或抛异常时，在所有者线程调用静态 `SendPendingStops()`，直到
`GetPendingStopCount() == 0`。不要依赖错误线程析构代替正常的显式 Stop。

### 2.8 `SendRequestResult` 与兼容 `SendRequest`

| 项目 | 内容 |
| --- | --- |
| 推荐接口 | `bool SendRequestResult(HostRequest&& request, HostResultCallback onComplete);` |
| 兼容接口 | `bool SendRequest(HostRequest&& request, HostCompleteCallback onComplete = nullptr);` |
| 适用场景 | Load、Reload、View、Session、Tool、Data Export、Slice Export |
| 前置条件 | 所有者线程；调用方构造九种已知具体 Request；结果回调非空 |

**参数：**

| 参数 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `request` | `HostRequest&&` | 是 | 必须是九种已知具体 Request；发送后不再复用 |
| `onComplete` | `HostResultCallback` | 是 | 每个请求恰好调用一次；同步结果可能在函数返回前到达 |

**返回值：**

- 返回 `true` 只表示请求被识别并接纳；操作成败以 `HostResult::isSucceeded` 为准。
- 返回 `false` 时，非空结果回调仍同步收到 `WrongThread`、`SessionNotReady`、
  `RequestRejected` 或 `OperationFailed`。
- 兼容 `SendRequest()` 保留给旧调用方；它会混合接纳状态与最终结果，新 Qt 代码不要据其
  `bool` 推断统一结果语义。

**调用示例：**

```cpp
HostViewResetRequest request; // 创建一个具体的相机复位请求。
request.targetView = primary3D; // 指定必须命中的目标视图。
const bool isAccepted = // 保存同步请求的立即结果。
    session->SendRequestResult( // 移交具体 Request，并接收结构化结果。
        std::move(request),
        onHostResult);
```

### 2.9 `GetRenderViewEndpoints`

| 项目     | 内容                                                                   |
| -------- | ---------------------------------------------------------------------- |
| 接口     | `const std::vector<HostRenderViewEndpoint>& GetRenderViewEndpoints();` |
| 说明     | 返回 Session 全部 endpoint 的只读引用                                  |
| 适用场景 | 枚举 Qt 已接入的全部 Host 视图                                         |
| 参数     | 无                                                                     |
| 返回值   | 构建失败时返回空集合；成功时按 topology 顺序返回                       |
| 生命周期 | 只在当前 Session topology 不变且处于 Running 时有效；move/rebuild/Stop/析构后不得缓存 |

**调用示例：**

```cpp
const auto& endpoints = // 取得 Session 内部 endpoint 集合的只读引用。
    session->GetRenderViewEndpoints(); // 调用方不获得 endpoint 所指 VTK 对象的所有权。
for (const HostRenderViewEndpoint& endpoint : endpoints) { // 按 topology 顺序遍历。
    qInfo() << QString::fromStdString(endpoint.id); // 读取稳定 view id。
}
```

### 2.10 `GetRenderViewEndpoint`

| 项目     | 内容                                                                              |
| -------- | --------------------------------------------------------------------------------- |
| 接口     | `const HostRenderViewEndpoint* GetRenderViewEndpoint(const std::string& viewId);` |
| 说明     | 按稳定 id 查询一个 endpoint                                                       |
| 适用场景 | 核对 Qt widget 与 Host 是否使用同一 render window/interactor                      |
| 参数     | `viewId`：Session 内稳定视图 id                                                   |
| 返回值   | 命中时返回非拥有指针；失败时返回 `nullptr`                                        |

**调用示例：**

```cpp
const HostRenderViewEndpoint* endpoint = // 声明非拥有 endpoint 指针。
    session->GetRenderViewEndpoint("primary-3d"); // 按稳定 id 查询主视图。
if (!endpoint || // 检查 id 是否命中。
    endpoint->renderWindow != primaryWindow.GetPointer()) { // 核对 Qt 与 Host 使用同一 window。
    return false; // window 不一致时拒绝继续接入。
}
```

### 2.11 `GetPrimaryEndpoint`

| 项目     | 内容                                                               |
| -------- | ------------------------------------------------------------------ |
| 接口     | `const HostRenderViewEndpoint* GetPrimaryEndpoint();`              |
| 说明     | 查询 Session 的首选 3D endpoint                                    |
| 适用场景 | 获取默认渲染、TimerEvent 或状态核对使用的 3D endpoint              |
| 参数     | 无                                                                 |
| 返回值   | 优先 Primary3D，其次 Composite3D，再退到首视图；失败返回 `nullptr` |

**调用示例：**

```cpp
const HostRenderViewEndpoint* primary = // 声明非拥有首选 endpoint 指针。
    session->GetPrimaryEndpoint(); // 按远端优先级解析首选视图。
if (!primary) { // 检查 Session 是否存在可用 endpoint。
    return false; // 没有可用视图时停止后续调用。
}
```

### 2.12 `GetRenderViewState`

| 项目     | 内容                                                                                   |
| -------- | -------------------------------------------------------------------------------------- |
| 接口     | `std::optional<HostRenderViewState> GetRenderViewState(const HostViewTarget& target);` |
| 说明     | 读取一个目标视图的独立值快照，不暴露内部 App runtime 或 VTK 内部对象                   |
| 选择规则 | `viewId` 非空时只按 id 查找；否则在 `isViewRoleUsed=true` 时按 role 查找               |
| 返回值   | 命中返回快照；目标不存在、线程错误或 Session 未处于 Running 时返回 `std::nullopt`       |

该接口只读当前状态，不改变主体请求写入链。mode、material、TF、ISO、background、WW/WC、
volume quality、visibility 和 axes 都是目标 View 的私有展示状态；spacing 与 cursor 来自
Session 共享数据状态，并复制进各 View 快照。`volumeTransferFunction` 是真实 scalar 坐标的
完整值副本。快照不包含相机字段、TF 编辑 revision、内部 App runtime 或 VTK identity。
查询必须在所有者线程调用；其它线程返回空结果。

| 主要字段 | 语义 |
| --- | --- |
| `id` / `role` / `viewMode` | 视图身份和当前模式 |
| `material` / `volumeTransferFunction` / `isoThreshold` / `background` | 目标 View 私有显示状态 |
| `windowLevel` / `volumeQuality` / `visibilityMask` / `isAxesVisible` | 目标 View 私有展示与 context 状态 |
| `spacing` / `cursorWorld` | Session 共享值在该快照中的副本 |
| `scalarRange` / `dataVersion` | 当前渲染批次的数据域和诊断版本 |
| `isFeatureActive` / `isInteracting` | 目标 View 当前活动状态 |

```cpp
const auto state = session->GetRenderViewState(primary3D); // 按既有 selector 读取一个视图。
if (state) {
    const double iso = state->isoThreshold; // 读取当前 ISO 阈值。
    const auto& colorNodes = // 读取独立颜色节点副本。
        state->volumeTransferFunction.colorNodes;
    const auto& opacityNodes = // 读取独立透明度节点副本。
        state->volumeTransferFunction.opacityNodes;
}
```

### 2.13 `GetRenderViewStates`

| 项目                           | 内容                                                                                                   |
| ------------------------------ | ------------------------------------------------------------------------------------------------------ |
| 接口                           | `std::vector<HostRenderViewState> GetRenderViewStates();`                                              |
| 说明                           | 按 `HostSessionConfig::renderViews` 的 topology 顺序读取全部视图快照                                   |
| 与 `GetRenderViewState` 的区别 | 前者返回单个 selector 命中的 `optional`；本接口一次返回全部视图；Session 未 ready、线程错误或无视图时为空 |
| 生命周期                       | 返回值和其中所有容器均为独立副本，不受后续渲染状态更新影响                                             |

```cpp
const auto states = session->GetRenderViewStates(); // 一次读取全部视图。
for (const auto& state : states) { // 顺序与 renderViews 配置一致。
    qInfo() << QString::fromStdString(state.id);
}
```

### 2.14 图像读取 API

普通 Qt 调用方通过值 DTO 读取当前 image，不取得 `vtkImageData` identity 或可写 scalar
指针。所有同步读取与异步接纳都要求 Session 处于 `Running`，并从所有者线程调用。

| 接口 | 语义 |
| --- | --- |
| `std::optional<ImageReadState> GetImageReadState();` | 使用默认 512 MiB 预算整卷深拷贝；失败只返回空 |
| `ImageReadResult GetImageReadResult(std::size_t maxReadBytes);` | 整卷同步读取，并保留错误和 `requiredBytes` |
| `ImageReadResult GetImageReadResult(const ImageReadRequest& request);` | 按 region 和预算同步读取 |
| `ImageReadChunkResult GetImageReadChunk(const ImageReadRequest&, std::size_t voxelOffset);` | 按 x-fast offset 分块续读，每块最多 8 MiB |
| `ImageReadAdmission StartImageRead(ImageReadRequest, ImageReadCallback);` | 固定执行器异步复制，由所有者定时器回调 |

`ImageReadRequest::region` 为空表示整卷；显式 region 是相对源 image 的 X/Y/Z 半开区间
`[offset, offset + size)`，三个 size 都必须大于零。`maxBytes` 默认 512 MiB，是本次调用的
分配预算；chunk 实际预算取 `maxBytes` 与 8 MiB 的较小值。region 成功输出的 extent 从 0
开始，origin 已移动到 region 首体素。

`ImageReadResult`/`ImageReadChunkResult` 的 `error` 使用 `ImageReadError`：`None`、`NoImage`、
`InvalidData`、`UnsupportedType`、`InvalidRegion`、`TooLarge`、`CopyFailed` 或 `Cancelled`。
`requiredBytes` 表示整个 region 所需字节数；chunk 还返回 `nextVoxelOffset` 与 `isDone`。
成功的 `ImageReadState` 自有只读 `values`/`validityMask` 字节，记录 geometry、direction、
scalar range、value/component 类型、tuple 顺序和 `DataVersion`。

同步读取一个 region：

```cpp
ImageReadRequest readRequest; // 创建普通值读取请求。
readRequest.region = ImageReadRegion{ // 使用相对源 image 的半开区间。
    { 0, 0, 0 },
    { 128, 128, 64 }
};
readRequest.maxBytes = 64ULL * 1024ULL * 1024ULL; // 本次最多分配 64 MiB。

ImageReadResult readResult = // 在 Session 所有者线程同步复制。
    session->GetImageReadResult(readRequest);
if (readResult.error == ImageReadError::None && readResult.state) {
    qInfo() << "Read voxels:"
            << static_cast<qulonglong>(readResult.state->voxelCount);
}
```

`StartImageRead()` 一次 Session 只允许一个尚未回调的读取。返回 `Accepted` 只表示任务入队；
其它接纳结果为 `InvalidRequest`、`Busy`、`QueueFull`、`Stopping` 或 `Unavailable`。最终
`ImageReadResult` 在所有者线程恰好回调一次。Stop 成功停止执行器后，已就绪结果按实际终态
回调；尚未形成结果的已接纳读取补为 `Cancelled`，不会静默丢弃回调。因此发起异步读取后
必须持续处理 `TimerEvent`，并在 Stop 完成前保持 Session 与 Qt 回调接收对象存活。

## 3. 主体 Request API

以下九种具体 Request 都通过推荐的
`SendRequestResult(HostRequest&&, HostResultCallback)` 发送，并复用 1.3 的
`onHostResult`。`isAccepted` 只表示接纳状态；同步请求也必须读取回调中的
`HostResult::isSucceeded`，不能把发送函数的 `bool` 当作业务结果。

本章代码均为嵌入 Qt 调用方上下文的片段：默认 `session`、视图目标与 `onHostResult` 已按
前文创建，路径、尺寸、spacing、origin 和拥有型 voxel buffer 由调用方在发送前准备并保持
数值一致；片段只展示 Request 的构造与发送边界。

### 3.1 `HostLoadRequest`

| 项目         | 内容                                                                                                     |
| ------------ | -------------------------------------------------------------------------------------------------------- |
| 接口         | `bool VtkAppHostSession::SendRequestResult(HostRequest&& request, HostResultCallback onComplete);`       |
| Request 类型 | `HostLoadRequest`                                                                                        |
| 说明         | 从 UTF-8 文件路径异步加载体数据                                                                          |
| 适用场景     | RAW 或 Data 层支持的体数据文件                                                                           |
| 结果回调     | 必填；未接纳时同步回调，接纳后由所有者定时器报告最终结果                                                |

**参数：**

| 字段                  | 类型                   | 必填 | 约束                                            |
| --------------------- | ---------------------- | ---- | ----------------------------------------------- |
| `filePath`            | `std::string`          | 是   | 非空 UTF-8 路径                                 |
| `geometry.dimensions` | `std::array<int, 3>`   | 是   | X/Y/Z；全零时仅 RAW 可从文件名末尾 `NxMxK` 推导 |
| `geometry.spacing`    | `std::array<float, 3>` | 是   | 三轴物理间距                                    |
| `geometry.origin`     | `std::array<float, 3>` | 是   | 输入体数据物理原点                              |

**结果语义：**

- `isAccepted == true`：加载任务已接纳；最终数据提交仍可能失败。
- `HostResult::isSucceeded == false`：路径、geometry、数据路由/数据服务、线程或异步提交失败。

**调用示例：**

```cpp
HostLoadRequest request; // 创建文件加载请求。
request.filePath = sourcePath.toUtf8().toStdString(); // 写入拥有存储的 UTF-8 路径。
request.geometry.dimensions = { sizeX, sizeY, sizeZ }; // 按 X/Y/Z 设置体素尺寸。
request.geometry.spacing = { // 设置三个轴向的物理间距。
    spacingX, spacingY, spacingZ
};
request.geometry.origin = { // 设置输入体数据的物理原点。
    originX, originY, originZ
};
const bool isAccepted = session->SendRequestResult( // 启动异步加载并保存接纳状态。
    std::move(request), // 移交 Request 及其路径和 geometry。
    onHostResult); // 复用 Qt 生命周期安全的结构化结果回调。
```

### 3.2 `HostReloadRequest`

| 项目         | 内容                                                                                                     |
| ------------ | -------------------------------------------------------------------------------------------------------- |
| 接口         | `bool VtkAppHostSession::SendRequestResult(HostRequest&& request, HostResultCallback onComplete);`       |
| Request 类型 | `HostReloadRequest`                                                                                      |
| 说明         | 从调用方拥有的 float32 buffer 异步重载体数据                                                             |
| 适用场景     | Qt 已在内存中准备好完整体素数据                                                                          |
| 结果回调     | 必填；未接纳时同步回调，接纳后由所有者定时器报告最终结果                                                |

**参数：**

| 字段                  | 类型                   | 必填 | 约束                               |
| --------------------- | ---------------------- | ---- | ---------------------------------- |
| `voxels`              | `std::vector<float>`   | 是   | X 最快、随后 Y/Z；Request 接管存储 |
| `geometry.dimensions` | `std::array<int, 3>`   | 是   | 乘积必须等于 `voxels.size()`       |
| `geometry.spacing`    | `std::array<float, 3>` | 是   | 三轴物理间距                       |
| `geometry.origin`     | `std::array<float, 3>` | 是   | 输入体数据物理原点                 |

Qt 上游如果只有 `float*`，必须在适配边界先复制到拥有存储的 `std::vector<float>`；
异步 worker 不能借用 Qt 临时缓冲区。下例中的 `qtVoxelCount` 必须由上游用已检查乘法得到，
并与 geometry dimensions 的乘积一致：

```cpp
bool SendReloadFromBuffer( // Qt/Host 边界只在这里复制一次。
    VtkAppHostSession& session,
    const float* qtVoxels,
    const std::size_t qtVoxelCount,
    const HostVolumeGeometry& geometry,
    HostResultCallback onComplete)
{
    if (qtVoxels == nullptr || qtVoxelCount == 0 || !onComplete) {
        return false; // 裸指针、数量或回调无效时不构造请求。
    }
    HostReloadRequest request; // Request 拥有复制后的连续 float32 数据。
    request.voxels.assign(qtVoxels, qtVoxels + qtVoxelCount);
    request.geometry = geometry; // geometry 已填入 dimensions/spacing/origin。
    return session.SendRequestResult(
        std::move(request),
        std::move(onComplete));
}
```

后续链路仍会为 VTK image 分配独立 scalar 存储并完成 LPS→RAS 重排，因此仅把公开字段改成
裸指针不能安全地消除该次拷贝。

**结果语义：**

- `isAccepted == true`：重载任务已接纳；最终数据提交仍可能失败。
- `HostResult::isSucceeded == false`：buffer/layout、数据路由/数据服务、线程或异步提交失败。

**调用示例：**

```cpp
HostReloadRequest request; // 创建内存重载请求。
request.voxels = std::move(ownedVoxels); // 把连续 float32 存储移交给 Request。
request.geometry.dimensions = { sizeX, sizeY, sizeZ }; // 写入与元素数量一致的尺寸。
request.geometry.spacing = { // 写入 X/Y/Z 物理间距。
    spacingX, spacingY, spacingZ
};
request.geometry.origin = { // 写入输入体数据物理原点。
    originX, originY, originZ
};
const bool isAccepted = session->SendRequestResult( // 启动异步 buffer 重载。
    std::move(request), // 移交 Request 和 voxels 所有权。
    onHostResult); // 接收结构化最终结果。
```

### 3.3 `HostViewSetRequest`

| 项目         | 内容                                                                                                     |
| ------------ | -------------------------------------------------------------------------------------------------------- |
| 接口         | `bool VtkAppHostSession::SendRequestResult(HostRequest&& request, HostResultCallback onComplete);`       |
| Request 类型 | `HostViewSetRequest`                                                                                     |
| 说明         | 以 partial patch 修改目标 View 的私有展示状态                                                           |
| 适用场景     | 模式、材质、TF、ISO、背景、窗宽窗位、显隐、质量和方向轴等 View 调整                                     |
| 结果回调     | 必填；同步报告整笔 patch 的结构化结果                                                                    |

**参数：**

`targetView` 必填。其余字段均为 optional；未填写的字段保留当前状态。

| 字段                     | 最短赋值语句                                                                | 主要约束                                                                                       |
| ------------------------ | --------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| `mode`                   | `request.mode = HostRenderMode::IsoSurface;`                                | 有效 `HostRenderMode`                                                                          |
| `material`               | `request.material = HostMaterialParams{0.15, 0.75, 0.35, 24.0, 0.9, true};` | finite；opacity `[0,1]`                                                                        |
| `materialPreset`         | `request.materialPreset = HostMaterialPreset::Glossy;`                      | 不能同时给 material/opacity                                                                    |
| `opacity`                | `request.opacity = 0.4;`                                                    | `[0,1]`                                                                                        |
| `volumeTransferFunction` | `request.volumeTransferFunction = function;`                                | 完整 scalar 快照；两类节点各至少两个、scalar finite 且分别递增，颜色/透明度位于 `[0,1]`        |
| `iso`                    | `request.iso = 420.0;`                                                      | finite                                                                                         |
| `background`             | `request.background = HostBackgroundColor{0.08, 0.08, 0.12};`               | RGB `[0,1]`                                                                                    |
| `windowLevel`            | `request.windowLevel = HostWindowLevelParams{600.0, 120.0};`                | width 大于零；center finite                                                                    |
| `volumeQuality`          | `request.volumeQuality = HostVolumeQuality::Ultra;`                         | 五档 `Auto/Low/High/XHigh/Ultra`；显式四档为 `25%/50%/75%/100%`；允许四种 Volume/Iso 3D 模式   |
| `visibility`             | `request.visibility = visibility;`                                          | 每个 optional 只修改目标 View 的一个显隐位                                                     |
| `isAxesVisible`          | `request.isAxesVisible = false;`                                            | 只作用目标 context                                                                             |

`volumeTransferFunction` 是唯一 scalar TF 快照；一次请求同时替换颜色节点与透明度节点，不存在半更新、preset、domain、节点 ID、插值字段或第二写入口。首次数据若尚无函数，App 会生成并保存一份实际默认函数；reload 与 LOD 不会覆盖显式快照。

Request 会先整体校验；任一已提供字段非法时在 setter 执行前令结果失败。合法候选进入
`AppViewPort` 的完整快照事务，失败时恢复旧状态；Host 的 context 补偿失败会停用目标 view，
不继续暴露半状态。实际 Render 策略在 owner tick 应用质量；失败时读回仍保持此前已应用档位。

**结果语义：**

- `HostResult::isSucceeded == true`：整个 patch 已提交。
- 失败：目标、字段、互斥关系、候选 mode、线程或下游事务无效；`isAccepted` 仍可能为
  `true`，因此必须读取结果回调。

**只调节 ISO：**

```cpp
HostViewSetRequest request; // 创建只包含本次修改的 View patch。
request.targetView = primary3D; // 指定必须命中的有效路由目标。
request.iso = 420.0; // 只写入新的等值面阈值，不构造其它当前值。
const bool isAccepted = // 保存整个 patch 的同步结果。
    session->SendRequestResult(std::move(request), onHostResult); // 移交 patch 并读取结果。
```

**调节窗宽窗位：**

```cpp
HostViewSetRequest request; // 创建只包含窗宽窗位修改的 View patch。
request.targetView = topDown; // 指定有效写入锚点；该字段不能省略。
request.windowLevel = HostWindowLevelParams{ // 写入目标 View 的私有窗宽窗位。
    600.0, // windowWidth：显示灰阶窗口宽度，必须大于零。
    120.0 // windowCenter：显示灰阶窗口中心，必须 finite。
};
const bool isAccepted = // 保存整个 patch 的同步结果。
    session->SendRequestResult( // 发送 targetView + windowLevel。
        std::move(request), onHostResult);
```

`topDown` 不是固定要求，任意有效视图都可以作为写入锚点；但只设置 `windowLevel` 而不设置
`targetView` 会令结果失败。WW/WC 属于目标 View；其它切片视图不会被这次 patch 隐式改写。

**设置完整 Volume 传递函数与质量：**

```cpp
HostViewSetRequest request; // 创建一个先统一校验、再顺序提交的 View patch。
request.targetView = primary3D; // 指定需要修改的 3D 视图。
request.mode = HostRenderMode::Volume; // 在同一 patch 中先给出合法候选 mode。
request.volumeQuality = HostVolumeQuality::High; // 提交固定 50% dimensions 的 High 质量意图。
HostVolumeTransferFunction function; // 构造真实 scalar 坐标的完整快照。
function.colorNodes = {
    { -1000.0, 0.0, 0.0, 0.0 },
    { 3000.0, 1.0, 1.0, 1.0 }
};
function.opacityNodes = {
    { -1000.0, 0.0 },
    { 3000.0, 1.0 }
};
request.volumeTransferFunction = std::move(function); // 颜色与透明度同批替换。
const bool isAccepted = // 保存整笔请求的同步结果。
    session->SendRequestResult( // 任一字段非法时结果回调报告失败。
        std::move(request), onHostResult);
```

主 3D 质量使用五档 `Auto/Low/High/XHigh/Ultra`，适用于 `Volume`、`CompositeVolume`、
`IsoSurface` 和 `CompositeIsoSurface`。显式四档都固定为原始 dimensions 的
`25%/50%/75%/100%`，不会在资源不足时静默降为较低档；`Ultra` 必须使用原始 dimensions。

Volume 由自己的 LOD controller 解析档位。`Auto` 在建立数据 LOD 计划时按系统内存、GPU
可用预算和 CPU 核心数选择一次 dimensions，不按逐帧耗时改档。运行期切换档位会先释放
旧 GPU 输入，再按当前 free VRAM 计算安全 block budget 和 partitions；该分块只控制 GPU
准入，不重新计算 Auto dimensions。交互期只覆盖屏幕采样、ray multiplier 和 jitter，
不修改 TF 或 Data LOD。

Iso 由独立 LOD controller 解析等值面提取输入尺寸，不复用 Volume 的 ray、jitter、GPU
预算或 partitions。`Auto` 只依据可用系统内存和 CPU 核心数选择一次输入比例。切档时先在
owner thread 完整物化 image resample、`vtkFlyingEdges3D` 以及可选的 mask resample/clip
候选；全部成功后才替换稳定 mapper 背后的输入，失败时已应用挡位和旧等值面保持不变。
`Ultra` 直接使用原始体数据，不再存在 766 最大维度上限。上游直接提供的 PolyData 不做
体数据降采样，Feature 私有 overlay 也不受主体 Iso 挡位影响。

连续 TF 编辑仍重复发送完整的
`HostViewSetRequest::volumeTransferFunction` 快照。更新只重建目标 View 的一维颜色/透明度
函数并请求渲染，不重绑 mapper input，也不触发三维 LOD 构建。

**设置目标 View 显隐：**

```cpp
HostVisibilityParams visibility; // 创建目标 View 的业务元素显隐 patch。
visibility.isPlanes3DVisible = true; // 显示 3D 彩色参考切平面。
visibility.isCrosshairVisible = false; // 隐藏切片十字线。
visibility.isRulerVisible = true; // 显示 3D cube axes 标尺。

HostViewSetRequest request; // 创建 View patch 请求。
request.targetView = primary3D; // 指定有效写入入口。
request.visibility = visibility; // 只写入目标 View 中显式提供的显隐位。
const bool isAccepted = // 保存显隐 patch 的同步结果。
    session->SendRequestResult(std::move(request), onHostResult); // 发送并读取结构化结果。
```

### 3.4 `HostSessionSetRequest`

`spacing` 和 world `cursor` 是 Session 共享真源，不伪装成单 View 字段：

```cpp
HostSessionSetRequest request;
request.spacing = std::array<double, 3>{ 0.4, 0.4, 1.0 };
request.cursor = HostCursorParams{{ x, y, z }, -1};
const bool isAccepted = session->SendRequestResult(
    std::move(request), onHostResult);
```

spacing 三轴必须 finite 且大于零；cursor 三个 world 分量必须 finite，axis 只允许
`-1/0/1/2`。两者都缺省时结果失败。spacing/cursor 是 Session 共享数据状态，成功提交后
所有 View 在各自状态快照和对应策略中消费同一值。

### 3.5 相机边界（当前无公开 Set Request）

当前 Host/Qt 契约没有相机值 DTO、相机 Set Request 或相机状态读回字段。模式切换所需的
投影、朝向、中心迁移、renderer 重绑和失败回滚由目标 App runtime 内部维护，不扩散到 Qt
adapter。唯一公开的相机命令仍是下一节的 `HostViewResetRequest`。

endpoint 的 renderer/window/interactor 是既有 Qt 嵌入兼容接口，不是业务相机接口。Qt/上位机
不得缓存或修改 endpoint renderer 的 `GetActiveCamera()` 裸指针；该对象由 Session/renderer
管理，窗口重建或 Session 析构会使外部生命周期假设失效，也会绕过所有者线程门禁和
dirty/Timer 渲染链。若未来需要公开相机写入或跨视图同步，必须另行定义纯值协议、所有权、
模式切换时序和交互优先级；本轮没有改变 Qt 层接口。

### 3.6 `HostViewResetRequest`

| 项目         | 内容                                                                                                     |
| ------------ | -------------------------------------------------------------------------------------------------------- |
| 接口         | `bool VtkAppHostSession::SendRequestResult(HostRequest&& request, HostResultCallback onComplete);`       |
| Request 类型 | `HostViewResetRequest`                                                                                   |
| 说明         | 复位目标视图相机                                                                                         |
| 参数         | `targetView`：必填且必须命中有效 context/service                                                         |
| 结果回调     | 必填；同步报告复位结果                                                                                   |

**结果语义：**

- `HostResult::isSucceeded == true`：目标相机已复位并标记刷新。
- 失败：目标、线程或 context 无效。

**调用示例：**

```cpp
HostViewResetRequest request; // 创建相机复位请求。
request.targetView = primary3D; // 指定需要复位的目标视图。
const bool isAccepted = // 保存同步复位结果。
    session->SendRequestResult(std::move(request), onHostResult); // 发送并读取结果。
```

### 3.7 `HostToolSetRequest`

| 项目         | 内容                                                                                                     |
| ------------ | -------------------------------------------------------------------------------------------------------- |
| 接口         | `bool VtkAppHostSession::SendRequestResult(HostRequest&& request, HostResultCallback onComplete);`       |
| Request 类型 | `HostToolSetRequest`                                                                                     |
| 说明         | 显式设置目标视图工具模式                                                                                 |
| 参数         | `targetView` 必填；`toolMode` 为 Navigation 或 ModelTransform                                            |
| 结果回调     | 必填；同步报告工具设置结果                                                                               |

**结果语义：**

- `HostResult::isSucceeded == true`：目标工具模式已写入。
- 失败：目标、枚举、线程或 context 无效。

**调用示例：**

```cpp
HostToolSetRequest request; // 创建显式工具模式请求。
request.targetView = primary3D; // 指定目标交互 context。
request.toolMode = HostToolMode::ModelTransform; // 写入模型变换模式。
const bool isAccepted = // 保存同步写入结果。
    session->SendRequestResult(std::move(request), onHostResult); // 发送并读取结果。
```

### 3.8 `HostToolSwitchRequest`

| 项目         | 内容                                                                                                     |
| ------------ | -------------------------------------------------------------------------------------------------------- |
| 接口         | `bool VtkAppHostSession::SendRequestResult(HostRequest&& request, HostResultCallback onComplete);`       |
| Request 类型 | `HostToolSwitchRequest`                                                                                  |
| 说明         | 在 Navigation 和 ModelTransform 之间切换                                                                 |
| 参数         | `targetView`：必填且必须命中有效 context                                                                 |
| 结果回调     | 必填；同步报告工具切换结果                                                                               |

**结果语义：**

- `HostResult::isSucceeded == true`：工具模式已切换。
- 失败：目标、线程或 context 无效。

**调用示例：**

```cpp
HostToolSwitchRequest request; // 创建二态工具切换请求。
request.targetView = primary3D; // 指定接收切换动作的目标 context。
const bool isAccepted = // 保存同步切换结果。
    session->SendRequestResult(std::move(request), onHostResult); // 发送并读取结果。
```

### 3.9 `HostDataExportRequest`

| 项目         | 内容                                                                                                     |
| ------------ | -------------------------------------------------------------------------------------------------------- |
| 接口         | `bool VtkAppHostSession::SendRequestResult(HostRequest&& request, HostResultCallback onComplete);`       |
| Request 类型 | `HostDataExportRequest`                                                                                  |
| 说明         | 异步导出 RAW 或等值面网格                                                                                |
| 适用场景     | 保存当前数据快照为 RAW、PLY、STL 或 OBJ                                                                  |
| 结果回调     | 必填；未接纳时同步回调，接纳后由所有者定时器报告最终写出结果                                           |

**参数：**

| 字段         | 类型                                  | 必填 | 约束                           |
| ------------ | ------------------------------------- | ---- | ------------------------------ |
| `outputPath` | `std::string`                         | 是   | UTF-8 输出目录，不是完整文件名 |
| `format`     | `std::optional<HostDataExportFormat>` | 否   | Raw/Ply/Stl/Obj                |
| `sourceView` | `HostViewTarget`                      | 否   | 空目标回退 Primary 视图        |

`format` 缺省时，Volume/CompositeVolume 推断 RAW，IsoSurface/CompositeIsoSurface
推断 PLY；slice mode 不支持 Data Export。导出始终读取当前完整数据快照并按原始
dimensions 生成结果，不读取 Volume/Iso 的展示 LOD。

**结果语义：**

- `isAccepted == true`：导出任务已接纳；文件写出仍可能失败。
- `HostResult::isSucceeded == false`：目录、来源模式、目标、线程或异步写出失败。

**调用示例：**

```cpp
HostDataExportRequest request; // 创建数据导出请求。
request.outputPath = // 写入 UTF-8 输出目录。
    outputDir.toUtf8().toStdString(); // 创建拥有自身存储的目录字符串。
request.format = HostDataExportFormat::Ply; // 显式选择 PLY 网格格式。
request.sourceView = primary3D; // 冻结主 3D 视图对应的数据/ISO/变换状态。
const bool isAccepted = session->SendRequestResult( // 启动异步导出任务。
    std::move(request), // 移交导出目录、格式和来源选择器。
    onHostResult); // 接收结构化最终结果。
```

Data 层生成 `<dimX>x<dimY>x<dimZ>_transform.ext`。任务接纳时会一起冻结 image/mask、
iso、model-to-world、scalar range 与 TF。PLY 在真实输出网格的 point scalar 上使用该批
scalar range/TF 写入三分量 `uchar RGB`；TF 为空时使用数据域灰阶。点、三角面、world
坐标、法线与 RGB 均来自同一输出网格。RGB 不读取界面材质、灯光、纹理或 framebuffer，
因此屏幕光照明暗不是文件颜色。OBJ 只承诺几何与法线，STL 只承诺几何；两者不伪造点
RGB。当前 Host/App/Data 链没有 PDF 导出实现。

### 3.10 `HostSliceExportRequest`

| 项目         | 内容                                                                                                     |
| ------------ | -------------------------------------------------------------------------------------------------------- |
| 接口         | `bool VtkAppHostSession::SendRequestResult(HostRequest&& request, HostResultCallback onComplete);`       |
| Request 类型 | `HostSliceExportRequest`                                                                                 |
| 说明         | 按目标切片方向异步逐层导出 PNG                                                                           |
| 结果回调     | 必填；未接纳时同步回调，接纳后由所有者定时器报告最终写出结果                                           |

**参数：**

| 字段         | 类型                    | 必填 | 约束                      |
| ------------ | ----------------------- | ---- | ------------------------- |
| `outputDir`  | `std::string`           | 是   | UTF-8 输出目录            |
| `sourceView` | `HostViewTarget`        | 是   | 必须命中 slice role       |
| `angleDeg`   | `std::optional<double>` | 否   | 平面内旋转角；必须 finite |

**结果语义：**

- `isAccepted == true`：切片导出任务已接纳；文件写出仍可能失败。
- `HostResult::isSucceeded == false`：目录、切片目标、角度、线程或异步写出失败。

**调用示例：**

```cpp
HostSliceExportRequest request; // 创建切片导出请求。
request.outputDir = // 写入 UTF-8 输出目录。
    outputDir.toUtf8().toStdString(); // 创建拥有自身存储的目录字符串。
request.sourceView = topDown; // 指定必须命中的俯视切片来源。
request.angleDeg = 30.0; // 在当前切片方向上增加 30 度平面内旋转。
const bool isAccepted = session->SendRequestResult( // 启动异步逐层 PNG 导出。
    std::move(request), // 移交目录、来源和可选角度。
    onHostResult); // 接收结构化最终结果。
```

## 4. CropHostFeature API

### 4.1 创建并挂载 Crop

| 项目     | 内容                                                              |
| -------- | ----------------------------------------------------------------- |
| 接口     | `explicit CropHostFeature(CropHostConfig config);`                |
| 说明     | 创建 Crop Feature 并通过 Session 挂载                             |
| 适用场景 | Qt 需要 Box/Plane 交互、history 或结果物化                        |
| 所有权   | Session 强持有到 Detach/Stop；Qt 保留 `shared_ptr` 句柄以发送请求 |
| 前置条件 | Session 已 `BuildSession()`                                       |

**参数：**

| 字段                              | 类型              | 说明                               |
| --------------------------------- | ----------------- | ---------------------------------- |
| `defaultTarget`                   | `CropHostTarget`  | 热键或默认动作使用的 Crop 目标     |
| `inputViews`                      | `HostViewTargets` | 可选 VTK interactor 输入视图       |
| `keys`                            | `CropHostKeys`    | 可选 Crop 热键                     |
| `defaultTarget.referenceView`     | `HostViewTarget`  | Crop 参考 3D 视图                  |
| `defaultTarget.targetViews`       | `HostViewTargets` | Crop 效果作用的视图集合            |
| `defaultTarget.isTargetViewsUsed` | `bool`            | 是否显式使用 `targetViews`         |
| `defaultTarget.isStatusVisible`   | `bool`            | 是否输出 Crop 状态信息             |
| `defaultTarget.source`            | `CropHostSource`  | CurrentImage 或 RegisteredPolyData |

**返回值：**

由 `session->AttachFeature()` 返回是否挂载成功。

**调用示例：**

```cpp
CropHostTarget cropTarget; // 创建 Crop 默认目标。
cropTarget.referenceView = primary3D; // 指定交互 widget 所在的参考 3D 视图。
cropTarget.targetViews.viewIds = { // 指定 Crop 效果作用的视图集合。
    "primary-3d",
    "slice-top-down"
};
cropTarget.isTargetViewsUsed = true; // 显式启用上述 targetViews。
cropTarget.source = CropHostSource::CurrentImage; // 使用当前 Session image 作为输入。

CropHostConfig cropConfig; // 创建 Crop Feature 一次性配置。
cropConfig.defaultTarget = cropTarget; // 设置默认 Crop 目标。

auto crop = std::make_shared<CropHostFeature>( // Qt 创建并强持有 Crop Feature。
    std::move(cropConfig)); // 移交一次性配置。
const bool isCropAttached = // 保存 Feature 立即挂载结果。
    session->AttachFeature(crop); // 由 Session 注入 HostFeatureContext。
```

只通过 Qt action 发送请求时，Crop `inputViews` 和 `keys` 可以保持空。

### 4.2 Crop 框架钩子索引

以下方法是 `HostFeature` 框架钩子，不是 Qt 业务调用入口。Qt 只调用
`session->AttachFeature()`、`session->DetachFeature()`、Crop `SendRequest()` 和
`GetState()`。

| 方法                                              | Qt 调用规则                                       |
| ------------------------------------------------- | ------------------------------------------------- |
| `GetFeatureId()`                                  | 可查询稳定 id，业务通常无需调用                   |
| `AttachHost(const HostFeatureContext&)`           | 框架钩子；Qt 不直接调用                           |
| `DetachHost()`                                    | 框架钩子；Qt 使用 `session->DetachFeature(*crop)` |
| `OnHostTick()`                                    | 框架钩子；由 Session timer 驱动                   |
| `SendRequest(CropHostRequest, CropBuildCallback)` | Qt 唯一 Crop 业务入口                             |
| `GetState()`                                      | Qt 在所有者线程读取状态                           |

### 4.3 `CropHostAction::Start`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::Start`                                                                               |
| 说明     | 建立或更新 Crop binding，不创建新的 Box/Plane widget                                                  |
| 参数     | `target` 必填                                                                                         |
| callback | 禁止                                                                                                  |

**返回值：**

- `true`：Crop binding 已建立。
- `false`：Feature/线程/目标/输入无效或正在 publishing。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop 请求。
request.action = CropHostAction::Start; // 选择只建立 binding 的 Start 动作。
request.target = cropTarget; // 提供完整 Crop 目标。
const bool isAccepted = // 保存立即启动结果。
    crop->SendRequest(std::move(request)); // 发送 Start，不能传 callback。
```

### 4.4 `CropHostAction::Box`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::Box`                                                                                 |
| 说明     | 建立 binding 并启动一个新的 Box 裁切 widget                                                           |
| 参数     | `target` 必填                                                                                         |
| callback | 禁止                                                                                                  |

Box 自己会完成 Start，不需要先重复发送 `CropHostAction::Start`。

**返回值：**

- `true`：Box binding 和新 widget 已启动。
- `false`：Feature、线程、目标、输入或 publishing 状态无效。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop 请求。
request.action = CropHostAction::Box; // 选择启动新 Box。
request.target = cropTarget; // 指定参考视图、作用视图和数据来源。
const bool isAccepted = // 保存 Box 启动结果。
    crop->SendRequest(std::move(request)); // 发送 Box，内部同时建立 binding。
```

### 4.5 `CropHostAction::Plane`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::Plane`                                                                               |
| 说明     | 建立 binding 并启动一个新的 Plane 裁切 widget                                                         |
| 参数     | `target` 必填                                                                                         |
| callback | 禁止                                                                                                  |

**返回值：**

- `true`：Plane binding 和新 widget 已启动。
- `false`：Feature、线程、目标、输入或 publishing 状态无效。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop 请求。
request.action = CropHostAction::Plane; // 选择启动新 Plane。
request.target = cropTarget; // 指定参考视图、作用视图和数据来源。
const bool isAccepted = // 保存 Plane 启动结果。
    crop->SendRequest(std::move(request)); // 发送 Plane，内部同时建立 binding。
```

### 4.6 `CropHostAction::Mode`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::Mode`                                                                                |
| 说明     | 设置 Crop 当前编辑模式                                                                                |
| 参数     | `target` 和 `removalMode` 必填                                                                        |
| callback | 禁止                                                                                                  |

`CropRemovalMode`：

- `None`：只定位 widget，不生成或改写裁切历史。
- `KeepInside`：保留 Box/Plane Inside。
- `RemoveInside`：移除 Box/Plane Inside。

**返回值：**

- `true`：目标 binding 和编辑模式已写入。
- `false`：Feature、线程、目标、`removalMode` 或 publishing 状态无效。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop 模式请求。
request.action = CropHostAction::Mode; // 选择 Mode 动作。
request.target = cropTarget; // 指定需要激活或更新的 Crop 目标。
request.removalMode = CropRemovalMode::RemoveInside; // 设置移除 Inside 的编辑语义。
const bool isAccepted = // 保存模式写入结果。
    crop->SendRequest(std::move(request)); // 发送 Mode，不能传 callback。
```

### 4.7 `CropHostAction::Previous`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::Previous`                                                                            |
| 说明     | 回退到上一个有效 Crop history 节点                                                                    |
| 参数     | 无；复用当前已绑定 history                                                                             |
| callback | 禁止                                                                                                  |

**返回值：**

- `true`：Previous 已在当前 history 上执行。
- `false`：没有已绑定 history、已在基线边界、线程错误或正在 publishing。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop history 请求。
request.action = CropHostAction::Previous; // 选择回退动作。
const bool isAccepted = // 保存回退结果。
    crop->SendRequest(std::move(request)); // 使用当前已绑定 history，不重复传 target。
```

### 4.8 `CropHostAction::Next`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::Next`                                                                                |
| 说明     | 前进到下一个有效 Crop history 节点                                                                    |
| 参数     | 无；复用当前已绑定 history                                                                             |
| callback | 禁止                                                                                                  |

**返回值：**

- `true`：Next 已在当前 history 上执行。
- `false`：没有已绑定 history、已在 redo 尾部、线程错误或正在 publishing。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop history 请求。
request.action = CropHostAction::Next; // 选择前进动作。
const bool isAccepted = // 保存前进结果。
    crop->SendRequest(std::move(request)); // 使用当前已绑定 history，不重复传 target。
```

### 4.9 `CropHostAction::Node`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::Node`                                                                                |
| 说明     | 跳转到指定 Crop history 节点数                                                                        |
| 参数     | `nodeCount` 必填；复用当前已绑定 history                                                               |
| callback | 禁止                                                                                                  |

**返回值：**

- `true`：已跳转到指定有效节点数。
- `false`：缺少/越界 `nodeCount`、没有已绑定 history、线程错误或正在 publishing。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop history 跳转请求。
request.action = CropHostAction::Node; // 选择按节点数跳转。
request.nodeCount = 2; // 指定目标有效节点数。
const bool isAccepted = // 保存节点跳转结果。
    crop->SendRequest(std::move(request)); // 使用当前已绑定 history 执行跳转。
```

Previous、Next 和 Node 在尚未建立 binding、ClearPolyData 或 Detach 后返回 `false`。Exit 只
退出 widget/活动视图并保留 committed history；只要 binding 和目标节点仍有效，Exit 后仍可
导航 Previous、Next 和 Node。

### 4.10 `CropHostAction::BuildResult`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::BuildResult`                                                                         |
| 说明     | 异步物化当前 Crop 结果                                                                                |
| 参数     | `target` 必填                                                                                         |
| callback | 必填；这是唯一允许 callback 的 Crop Action                                                            |

**返回值：**

- `true`：物化任务已启动。
- `false`：目标、输入、线程、publishing 状态或 callback 规则无效。

`CropBuildResult` 提供 `isSucceeded`、`failureReason`、输入版本、节点数以及结果
`imageData`/`maskImage`/`polyData`。输入版本变化时返回
`CropFailure::VersionMismatch`，旧结果不覆盖新数据。

`CropFailure`：

| 值                  | 含义                           |
| ------------------- | ------------------------------ |
| `None`              | 没有失败                       |
| `NoImage`           | image 路径没有绑定输入图像     |
| `NoPolyData`        | PolyData 路径没有绑定输入网格  |
| `BadBounds`         | 请求 bounds 本身非法           |
| `OutOfBounds`       | bounds 超出输入数据范围        |
| `NoBackend`         | 请求组合没有可执行后端         |
| `BadBuildMode`      | image 物化语义不受当前后端支持 |
| `LowRam`            | 内存不足                       |
| `MaskFailed`        | 三维 mask 生成失败             |
| `ImageFailed`       | 输出 image 生成失败            |
| `ClipFailed`        | Box outline PolyData 生成失败  |
| `BadInput`          | 历史、快照或物化参数无效       |
| `EmptyResult`       | 没有保留任何有效点             |
| `Busy`              | 已有物化任务执行中             |
| `VersionMismatch`   | 输入版本或数据源已经变化       |
| `WorkerStartFailed` | worker 未能启动                |
| `WorkerFailed`      | worker 异常终止                |

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop 物化请求。
request.action = CropHostAction::BuildResult; // 选择异步 BuildResult。
request.target = cropTarget; // 指定物化的数据来源和目标视图。

const QPointer<QMainWindow> owner(this); // 保存 Qt 窗口生命期门禁。
const bool isAccepted = crop->SendRequest( // 启动 Crop 异步物化。
    std::move(request), // 移交目标和动作。
    [owner](CropBuildResult result) { // 接收完整物化结果对象。
        if (!owner) { // 检查 Qt 窗口是否仍存在。
            return; // 窗口已销毁时丢弃结果。
        }
        QMetaObject::invokeMethod( // 把结果消费排队到 Qt 线程。
            owner.data(), // 指定 queued invocation 接收对象。
            [owner, result = std::move(result)]() mutable { // 移动保存完整结果。
                if (!owner) { // 执行前再次检查 Qt 对象。
                    return; // 对象失效时不更新 UI。
                }
                qInfo() // 在 Qt 线程记录物化结果。
                    << "Crop built:"
                    << result.isSucceeded
                    << static_cast<int>(result.failureReason); // 同时输出成功位和失败原因。
            },
            Qt::QueuedConnection); // 明确采用排队调用。
    });
```

### 4.11 `CropHostAction::SetPolyData`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::SetPolyData`                                                                         |
| 说明     | 注册 Crop 可选择的 PolyData 输入                                                                      |
| 参数     | 非空 `polyData`；大于零且严格递增的 `sourceVersion`                                                   |
| callback | 禁止                                                                                                  |

同一指针不能只改版本重复注册。成功注册后，目标使用
`CropHostSource::RegisteredPolyData`。

**返回值：**

- `true`：新的 PolyData 和来源版本已注册。
- `false`：数据为空、版本非法/未递增、指针重复、线程错误或正在 publishing。

**调用示例：**

```cpp
auto sourcePolyData = // 创建调用方持有的 PolyData；实际发送前填充有效网格。
    vtkSmartPointer<vtkPolyData>::New(); // 分配一个可由调用方填充的网格对象。
const std::uint64_t nextSourceVersion = 1; // 首次注册使用非零版本，后续严格递增。

CropHostRequest request; // 创建 PolyData 注册请求。
request.action = CropHostAction::SetPolyData; // 选择 SetPolyData 动作。
request.polyData = sourcePolyData; // 传入非空 vtkSmartPointer。
request.sourceVersion = nextSourceVersion; // 写入大于零且严格递增的来源版本。
const bool isAccepted = // 保存注册结果。
    crop->SendRequest(std::move(request)); // 发送请求且不传 callback。
```

使用已注册 PolyData 启动 Box：

```cpp
CropHostTarget target = cropTarget; // 复制现有 Crop 目标。
target.source = CropHostSource::RegisteredPolyData; // 切换到已注册 PolyData 来源。

CropHostRequest request; // 创建 Box 请求。
request.action = CropHostAction::Box; // 选择启动新 Box。
request.target = target; // 提供 PolyData 来源目标。
const bool isAccepted = // 保存 Box 启动结果。
    crop->SendRequest(std::move(request)); // 在已注册 PolyData 上启动 Crop。
```

### 4.12 `CropHostAction::ClearPolyData`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::ClearPolyData`                                                                       |
| 说明     | 清除已注册 PolyData 和版本                                                                            |
| 参数     | 无                                                                                                    |
| callback | 禁止                                                                                                  |

**返回值：**

- `true`：已注册 PolyData、版本和活动绑定已清除。
- `false`：Feature 未挂载、线程错误或正在 publishing。

**调用示例：**

```cpp
CropHostRequest request; // 创建 PolyData 清除请求。
request.action = CropHostAction::ClearPolyData; // 选择 ClearPolyData 动作。
const bool isAccepted = // 保存清除结果。
    crop->SendRequest(std::move(request)); // 清除已注册 PolyData 和活动绑定。
```

### 4.13 `CropHostAction::RestoreOriginal`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::RestoreOriginal`                                                                     |
| 说明     | 恢复首次成功物化前保存的原始 image                                                                    |
| 参数     | 无                                                                                                    |
| callback | 禁止                                                                                                  |

**返回值：**

- `true`：已恢复保存的原始 image。
- `false`：原始快照、当前版本、线程或 publishing 状态不满足恢复条件。

**调用示例：**

```cpp
CropHostRequest request; // 创建原始数据恢复请求。
request.action = CropHostAction::RestoreOriginal; // 选择 RestoreOriginal 动作。
const bool isAccepted = // 保存恢复结果。
    crop->SendRequest(std::move(request)); // 恢复原始数据并重新开放完整 history。
```

### 4.14 `CropHostAction::Exit`

| 项目     | 内容                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| 接口     | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action   | `CropHostAction::Exit`                                                                                |
| 说明     | 退出当前 Crop 编辑并清除活动视图                                                                      |
| 参数     | 无                                                                                                    |
| callback | 禁止                                                                                                  |

Exit 不删除 committed history 或当前 binding。它关闭编辑 widget、清除 Feature 活动视图并恢复
相关质量状态；`GetState().isActive` 会变为 `false`，但 history 仍可用于导航或后续恢复。

**返回值：**

- `true`：Crop 编辑和活动视图已退出。
- `false`：Feature、线程、退出流程或 publishing 状态无效。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop 退出请求。
request.action = CropHostAction::Exit; // 选择 Exit 动作。
const bool isAccepted = // 保存退出结果。
    crop->SendRequest(std::move(request)); // 退出 Crop 编辑且不传 callback。
```

### 4.15 `CropHostFeature::GetState`

| 项目     | 内容                                          |
| -------- | --------------------------------------------- |
| 接口     | `CropHostState GetState() const;`             |
| 说明     | 读取 Crop history、活动状态和 publishing 状态 |
| 调用线程 | 所有者线程                                    |
| 返回值   | 未挂载或线程错误时返回默认状态                |

`history.nodeCount` 是当前 active history 的游标，`history.operationCount` 是该 history 的
节点总数；两者不包含 `baseNodeCount` 已物化进当前 image/mask 基线的绝对前缀。下面只计算
Qt 按钮的粗粒度启用状态，实际请求是否可接纳以及异步物化是否成功仍以 `SendRequest()` 返回值
和 `CropBuildCallback` 为准。

**调用示例：**

```cpp
const CropHostState state = crop->GetState(); // 读取当前 Crop UI 状态快照。
const bool canBuild = // 计算物化按钮是否可用。
    state.isActive && !state.isPublishing && // 必须处于活动、非发布状态，
    state.history.nodeCount > 0; // 且当前 active history 至少包含一个节点。
const bool canGoPrevious = // 计算回退按钮是否可用。
    !state.isPublishing && state.history.nodeCount > 0; // Exit 后也可导航 committed history。
const bool canGoNext = // 计算前进按钮是否可用。
    !state.isPublishing && // publishing 期间不修改 history 游标，
    state.history.nodeCount < state.history.operationCount; // 游标后仍有 committed 节点。
```

## 5. GapHostFeature API

### 5.1 创建并挂载 Gap

| 项目     | 内容                                                             |
| -------- | ---------------------------------------------------------------- |
| 接口     | `explicit GapHostFeature(GapHostConfig config);`                 |
| 说明     | 创建 Gap Feature 并通过 Session 挂载                             |
| 适用场景 | Qt 需要启动 Gap 分析、切换 overlay 或读取统计                    |
| 所有权   | Session 强持有到 Detach/Stop；Qt 保留 `shared_ptr` 句柄以发送请求 |
| 前置条件 | 有效 `defaultStart`、非空 `inputViews`、Overlay/Exit 有效 chord  |

**参数：**

| 字段                       | 类型                 | 说明                                      |
| -------------------------- | -------------------- | ----------------------------------------- |
| `defaultStart`             | `GapHostStartParams` | 热键或默认启动使用的完整分析参数          |
| `inputViews`               | `HostViewTargets`    | 接收 Gap 热键/输入事件的视图集合，不能为空 |
| `keys`                     | `GapHostKeys`        | Overlay 与 Exit chord，二者都必须有效     |
| `defaultStart.targetViews` | `HostViewTargets`    | 分析结果作用的 mesh/slice 视图            |
| `defaultStart.surface`     | `GapSurfaceConfig`   | ISO 来源与阈值                            |
| `defaultStart.voidParams`  | `GapVoidParams`      | DefX 显式过滤开关与最小体积参数           |

**返回值：**

- `true`：Gap Feature 已挂载并建立输入绑定。
- `false`：Session 未构建、配置/按键/输入无效、id 重复或线程错误。

**调用示例：**

```cpp
GapHostStartParams gapStart; // 创建 Gap 默认分析参数。
gapStart.targetViews.viewIds = { // 指定分析结果作用的视图。
    "primary-3d",
    "slice-top-down"
};
gapStart.surface.isoMode = // 选择绝对 ISO。
    GapIsoMode::AbsoluteValue; // 直接使用输入 scalar 域阈值。
gapStart.surface.absoluteIsoValue = 0.172; // 设置 DefX ISO 阈值。
gapStart.surface.backgroundMean = -1.617f; // 设置 DefX 背景均值。
gapStart.surface.materialMean = 0.453f; // 设置 DefX 材料均值。
gapStart.voidParams.isFilterEnabled = false; // 明确关闭 DefX 过滤。
gapStart.voidParams.minVolumeMM3 = 0.0; // 关闭过滤时不参与筛选。

GapHostConfig gapConfig; // 创建 Gap Feature 一次性配置。
gapConfig.defaultStart = gapStart; // 设置热键和默认启动使用的完整参数。
gapConfig.inputViews.viewIds = { "primary-3d" }; // 设置接收 Gap 输入事件的视图。
gapConfig.keys.switchOverlay.keyCode = 'j'; // 配置必需的 Overlay chord。
gapConfig.keys.exit.keySym = "Escape"; // 配置必需的 Exit chord。

auto gap = std::make_shared<GapHostFeature>( // Qt 创建并强持有 Gap Feature。
    std::move(gapConfig)); // 移交一次性 Gap 配置。
const bool isGapAttached = // 保存 Feature 立即挂载结果。
    session->AttachFeature(gap); // 由 Session 注入 HostFeatureContext。
```

### 5.2 Qt 上位机完整调用流程

Qt 上位机的业务入口只有 `VtkAppHostSession` 与 `GapHostFeature`。上位机不创建
`GapAnalysisService`，不调用 `BuildVoidMesh()`/`BuildLabelImage()`，也不从 Feature 取得
`vtkPolyData` 或标签图。Gap Feature 在内部取得当前 Session image，运行分析，并通过目标
View 的 `OverlayService` 把 3D mesh 和 2D label overlay 挂载到现有 renderer。

完整时序：

1. Qt GUI 线程创建两个 QVTK widget，并分别绑定 3D 与切片
   `vtkGenericOpenGLRenderWindow`。
2. 用同一批 render window 构造 `HostSessionConfig`，调用 `BuildSession()`；Qt 不调用
   `VtkAppHostSession::Start()` 或 `vtkRenderWindowInteractor::Start()`。
3. 构造并保存完整 `GapHostStartParams`，创建 `GapHostFeature`，再调用
   `session->AttachFeature(gap)`。
4. 调用 `session->AttachTimer()`。Host 内部会在目标 interactor 上创建 repeating timer；
   Qt 不需要再手工调用 `CreateRepeatingTimer()`，但必须保持 `QApplication::exec()` 运行，并由
   所用 QVTK interactor 集成持续向 Host 派发匹配 timer id 的 `TimerEvent`。
5. 通过 `HostLoadRequest` 或 `HostReloadRequest` 提交数据；只在对应 `HostResult` 成功后发送
   `GapHostAction::Start`。
6. Start 同步返回 `true` 只代表 worker 已接纳。worker 完成后，由 owner-thread timer tick
   消费同批 label/mesh、挂载 overlay，并在 owner thread 执行 `GapHostCallback`。
7. Qt action 使用 `GapHostAction::Overlay` 切换现有结果的可见性；它不会重新运行 DefX。
8. Qt action 使用 `GapHostAction::Exit` 退出 Gap 显示；继续处理 timer，直到
   `GetState().isExitPending == false`。
9. 窗口关闭时在 GUI/owner thread 调用 `session->Stop()`；只有 Stop 完成后才释放 Feature、
   Session、QVTK widget 和 render window。

下面代码假设 `MainWindow` 已经创建 `m_primaryWindow`、`m_topDownWindow`，并先把它们分别
交给两个 `QVTKOpenGLNativeWidget`。三个对象由 Qt adapter 持有：

```cpp
std::unique_ptr<VtkAppHostSession> m_session; // Qt 窗口独占 Session 生命周期。
std::shared_ptr<GapHostFeature> m_gap; // Qt 保存请求句柄，Session 同时强持有已挂载 Feature。
GapHostStartParams m_gapStart; // Qt 保存完整参数；修改 ISO 时复制整份再发新 Start。
```

在 Qt GUI 线程构建 Session、Feature 与 timer：

```cpp
bool MainWindow::BuildGap()
{
    HostRenderViewConfig primary; // 声明接收 3D 孔隙 mesh overlay 的视图。
    primary.id = "primary-3d"; // 使用 Session 内唯一稳定 id。
    primary.role = HostRenderViewRole::Primary3D; // 该 role 会被 Gap 映射为 mesh target。
    primary.window.viewInit.viewMode = // 主体先显示原始对象等值面。
        HostRenderMode::CompositeIsoSurface;
    primary.renderWindow = m_primaryWindow; // 与主 3D QVTK widget 使用同一 window。

    HostRenderViewConfig topDown; // 声明接收 2D label overlay 的俯视切片。
    topDown.id = "slice-top-down"; // 使用 Session 内唯一稳定 id。
    topDown.role = HostRenderViewRole::TopDownSlice; // Gap 映射为 Top_down slice target。
    topDown.window.viewInit.viewMode = HostRenderMode::SliceTopDown;
    topDown.renderWindow = m_topDownWindow; // 与切片 QVTK widget 使用同一 window。

    HostSessionConfig sessionConfig; // 创建一次性 Session 配置。
    sessionConfig.renderViews.push_back(std::move(primary));
    sessionConfig.renderViews.push_back(std::move(topDown));
    const QPointer<MainWindow> owner(this); // 为延迟 Stop 投递保存 Qt 弱引用。
    sessionConfig.sendOwnerTask = [owner](std::function<void()> task) {
        if (!owner || !task) { // Qt 对象或任务失效时拒绝投递。
            return false;
        }
        QTimer::singleShot( // 下一轮 Qt 事件循环在 owner thread 执行 Stop 任务。
            0,
            owner.data(),
            [ownerTask = std::move(task)]() mutable {
                ownerTask();
            });
        return true;
    };

    m_session = std::make_unique<VtkAppHostSession>(
        std::move(sessionConfig));
    if (!m_session->BuildSession()) { // QVTK window 绑定完成后构建 Host。
        return false;
    }

    m_gapStart.targetViews.viewIds = { // 同一批结果同时送往一个 3D 与一个 slice View。
        "primary-3d",
        "slice-top-down"
    };
    m_gapStart.surface.isoMode = GapIsoMode::AbsoluteValue;
    m_gapStart.surface.absoluteIsoValue = 0.172; // 按实际数据 scalar 域填写。
    m_gapStart.surface.backgroundMean = -1.617f; // 按供应商参数填写背景均值。
    m_gapStart.surface.materialMean = 0.453f; // 按供应商参数填写材料均值。
    m_gapStart.voidParams.isFilterEnabled = false;
    m_gapStart.voidParams.minVolumeMM3 = 0.0;

    GapHostConfig gapConfig; // 创建 Feature 一次性配置。
    gapConfig.defaultStart = m_gapStart; // 热键路径和显式 Start 共享同一默认参数。
    gapConfig.inputViews.viewIds = { // 即使 Qt 不接 Hotkey，当前配置仍要求有效输入视图。
        "primary-3d",
        "slice-top-down"
    };
    gapConfig.keys.switchOverlay.keyCode = 'j'; // 提供有效 Overlay chord。
    gapConfig.keys.exit.keySym = "Escape"; // 提供有效 Exit chord。

    m_gap = std::make_shared<GapHostFeature>(std::move(gapConfig));
    if (!m_session->AttachFeature(m_gap)) { // 必须先 BuildSession，再挂 Feature。
        return false;
    }

    HostTimerConfig timer; // 把 Host/Feature tick 接到一个有效 QVTK View。
    timer.isTimerEnabled = true;
    timer.targetView = {
        "slice-top-down",
        false,
        HostRenderViewRole::TopDownSlice
    };
    return m_session->AttachTimer(timer); // Qt 后续只进入 QApplication::exec()。
}
```

如果 `BuildSession()`、`AttachFeature()` 或 `AttachTimer()` 任一步失败，调用方仍保留已经创建的
对象，并在 owner thread 按第 6 章执行 `Stop()` 收口；不要在 Stop 失败时直接 `reset()`。

加载数据成功后启动 Gap。下面用文件 Load；若上位机已经拥有 float32 数据，将请求换成
`HostReloadRequest`，完成回调中的 Start 逻辑不变：

```cpp
bool MainWindow::LoadAndStartGap(const QString& path)
{
    if (!m_session || !m_gap) { // Session/Feature 必须已经构建并挂载。
        return false;
    }

    HostLoadRequest request; // 创建拥有 UTF-8 路径的异步加载请求。
    request.filePath = path.toUtf8().toStdString();
    request.geometry.dimensions = { sizeX, sizeY, sizeZ };
    request.geometry.spacing = { spacingX, spacingY, spacingZ };
    request.geometry.origin = { originX, originY, originZ };

    const QPointer<MainWindow> owner(this); // 防止加载完成时窗口已经析构。
    return m_session->SendRequestResult(
        std::move(request),
        [owner](HostResult result) mutable {
            if (!owner) { // 同步拒绝和异步完成都先检查 Qt 生命周期。
                return;
            }
            QTimer::singleShot( // 避免在 Host 完成回调栈内重入下一笔业务请求。
                0,
                owner.data(),
                [owner, result = std::move(result)]() mutable {
                    if (!owner) {
                        return;
                    }
                    if (!result.isSucceeded) {
                        qWarning().noquote()
                            << QString::fromStdString(result.message);
                        return;
                    }
                    if (!owner->StartGap()) { // 数据提交成功后才允许启动分析。
                        qWarning() << "Gap Start request rejected";
                    }
                });
        });
}
```

`sizeX`、`spacingX` 等占位符必须替换为上位机真实 geometry；`HostReloadRequest::voxels` 则
必须是与 dimensions 乘积一致的拥有型 `std::vector<float>`。

发送 Start，并在完成后读取同一成功批次的统计：

```cpp
bool MainWindow::StartGap()
{
    if (!m_gap) {
        return false;
    }

    GapHostRequest request; // Start 需要完整参数，不是 partial patch。
    request.action = GapHostAction::Start;
    request.start = m_gapStart;

    const QPointer<MainWindow> owner(this);
    return m_gap->SendRequest(
        std::move(request),
        [owner](const bool isDisplayed) {
            if (!owner) {
                return;
            }
            QTimer::singleShot(0, owner.data(), [owner, isDisplayed] {
                if (!owner || !owner->m_gap) {
                    return;
                }
                const GapHostState state = owner->m_gap->GetState();
                qInfo() // callback 与状态分别表达显示结果和分析结果。
                    << "Gap displayed:" << isDisplayed
                    << "analysis:" << static_cast<int>(state.analysisState)
                    << "void voxels:"
                    << static_cast<qulonglong>(state.statistics.voidVoxelCount)
                    << "porosity:" << state.statistics.porosityRatio;
            });
        });
}
```

Start 返回 `false` 时请求未接纳，callback 不会执行。callback 的 `isDisplayed == true` 表示
分析结果已经消费，且至少一个目标 overlay 挂载成功；它不保证每一个请求目标都成功挂载。
`GapHostState::analysisState == Succeeded` 只表示分析 payload 成功，不能替代显示回调。

Qt 按钮切换和退出 Gap：

```cpp
bool MainWindow::SwitchGap()
{
    if (!m_gap) {
        return false;
    }
    GapHostRequest request;
    request.action = GapHostAction::Overlay;
    return m_gap->SendRequest(std::move(request)); // Overlay 禁止 callback。
}

bool MainWindow::ExitGap()
{
    if (!m_gap) {
        return false;
    }
    GapHostRequest request;
    request.action = GapHostAction::Exit;
    return m_gap->SendRequest(std::move(request)); // Exit 禁止 callback。
}
```

Qt 的“孔隙分析”“显示/隐藏”“退出孔隙”按钮分别调用 `StartGap()`、`SwitchGap()`、
`ExitGap()`。Gap 请求直接发给保存的 `GapHostFeature`；不要把 `GapHostRequest` 传给
`VtkAppHostSession::SendRequestResult()`，后者只接收主体 `HostRequest` variant。

Exit 返回 `true` 表示退出请求已处理，并且存在可退出的活动显示或缓存；它不代表清理已经
完成。Qt 不需要手工调用 `OnHostTick()`；继续运行事件循环，让 Session timer 驱动收口，直到
`GetState()` 同时满足：

```cpp
const GapHostState state = m_gap->GetState();
const bool isGapExited =
    state.analysisState == GapAnalysisState::Idle &&
    !state.isViewActive &&
    !state.isExitPending;
```

#### 5.2.1 结果复用与上位机边界

- `Primary3D`/`Composite3D` 目标消费 worker 预构建的孔隙 mesh；三个 slice role 消费同批
  label image。两者随同一结果事务发布，Qt 不负责在两类结果之间传递数据。
- owner tick 只接管已发布的只读 VTK owner，不在 GUI 线程重新执行整卷 mesh 提取或标签
  DeepCopy。
- `GapHostAction::Overlay` 隐藏时只卸载 overlay，内部结果仍保留；再次切换为显示时复用同一
  mesh/label，不重新运行分析。
- SDK 没有公开 Gap raw mesh/label 读取接口。上位机当前可消费的是可视化结果、
  `GapHostState::statistics` 和分析/会话状态；不要包含或调用内部 `GapAnalysisService`。
- 数据 Reload 产生新 `DataVersion` 后，Feature 会退出旧 Gap 会话并清空旧统计；上位机必须
  对新数据重新发送 Start。

### 5.3 Gap 框架钩子索引

以下方法是 `HostFeature` 框架钩子，不是 Qt 业务调用入口。Qt 只调用
`session->AttachFeature()`、`session->DetachFeature()`、Gap `SendRequest()` 和
`GetState()`。

| 方法                                           | Qt 调用规则                                      |
| ---------------------------------------------- | ------------------------------------------------ |
| `GetFeatureId()`                               | 可查询稳定 id，业务通常无需调用                  |
| `AttachHost(const HostFeatureContext&)`        | 框架钩子；Qt 不直接调用                          |
| `DetachHost()`                                 | 框架钩子；Qt 使用 `session->DetachFeature(*gap)` |
| `OnHostTick()`                                 | 框架钩子；由 Session timer 驱动                  |
| `SendRequest(GapHostRequest, GapHostCallback)` | Qt 唯一 Gap 业务入口                             |
| `GetState()`                                   | Qt 在所有者线程读取状态和统计                    |

### 5.4 `GapHostAction::Start`

| 项目     | 内容                                                                                              |
| -------- | ------------------------------------------------------------------------------------------------- |
| 接口     | `bool GapHostFeature::SendRequest(GapHostRequest request, GapHostCallback onComplete = nullptr);` |
| Action   | `GapHostAction::Start`                                                                            |
| 说明     | 使用完整 `GapHostStartParams` 启动一批新分析                                                      |
| 参数     | `start` 必填                                                                                      |
| callback | 可选；`true` 表示本批结果已消费且至少一个 overlay 挂载成功                                        |

调用 Start 前必须按 [1.7](#17-gapanalysis-私有运行时) 部署当前配置的私有运行时，并成功
绑定 Host timer。同步返回 `true` 只表示 worker 线程已被接纳；DLL 解析在 worker 内执行，
私有 runtime 缺失或模块身份不一致会在后续 tick/callback 收口为异步失败。timer 是否持续
产生 `TimerEvent` 则决定已接纳任务能否完成显示与回调交付。

**Start 参数：**

| 字段                         | 说明与约束                                                            |
| ---------------------------- | --------------------------------------------------------------------- |
| `targetViews`                | 非空；至少解析出一个可用 mesh 或 slice 目标                           |
| `surface.isoMode`            | DataRangeRatio 或 AbsoluteValue                                       |
| `surface.dataRangeRatio`     | ratio 模式下必须位于 `[0,1]`                                          |
| `surface.absoluteIsoValue`   | absolute 模式下直接使用的输入标量阈值                                 |
| `surface.backgroundMean`     | DefX 背景均值；finite，且不大于 materialMean                          |
| `surface.materialMean`       | DefX 材料均值；finite                                                 |
| `voidParams.isFilterEnabled` | 是否启用 DefX 供应商过滤                                              |
| `voidParams.minVolumeMM3`    | DefX 最小体积条件；非负且可表示为 float，最终筛选结果以供应商输出为准 |

**返回值：**

- `true`：请求已接纳且 worker 线程已创建；不代表 runtime、分析或 overlay 已成功。
- `false`：Feature、线程、输入快照、目标、参数或当前会话状态无效；非空 validity mask 也会被拒绝，因为当前供应商内核没有对应输入。

**按数据范围比例启动：**

```cpp
GapHostRequest request; // 创建 Gap 请求。
request.action = GapHostAction::Start; // 选择启动新分析。
request.start = gapStart; // 提供完整 Start 参数，而不是局部 patch。

const QPointer<QMainWindow> owner(this); // 保存 Qt 窗口生命期门禁。
const bool isAccepted = gap->SendRequest( // 启动 Gap 异步分析。
    std::move(request), // 移交完整 Start 参数。
    [owner](bool isSuccess) { // 接收本批结果消费和 overlay 显示结果。
        if (!owner) { // 检查 Qt 对象是否仍然存活。
            return; // 对象已销毁时丢弃结果。
        }
        QMetaObject::invokeMethod( // 把 UI 更新排队到 Qt 线程。
            owner.data(), // 指定 queued invocation 的接收对象。
            [owner, isSuccess] { // 捕获生命期门禁和最终结果。
                if (owner) { // 执行前再次检查对象。
                    qInfo() << "Gap displayed:" << isSuccess; // 在 Qt 线程消费显示结果。
                }
            },
            Qt::QueuedConnection); // 明确使用排队调用。
    });
```

DataRangeRatio 的实际阈值为 `min + (max - min) * ratio`。

**只调整绝对 ISO 并重新启动：**

```cpp
GapHostStartParams start = gapStart; // 从 Qt 保存的当前完整配置复制一份。
start.surface.isoMode = GapIsoMode::AbsoluteValue; // 切换到绝对阈值模式。
start.surface.absoluteIsoValue = 420.0; // 只修改输入标量域 ISO。

GapHostRequest request; // 创建新的 Gap Start 请求。
request.action = GapHostAction::Start; // 修改 ISO 会启动一批新分析。
request.start = std::move(start); // 移交包含其它已知参数的完整配置。
const bool isAccepted = // 保存分析启动结果。
    gap->SendRequest(std::move(request)); // callback 可选，此处不传。
```

Gap Start 不是 partial patch。只改 ISO 时，Qt 保存并复制当前
`GapHostStartParams`；不需要从 `GapHostState` 反向构造其它参数。

### 5.5 `GapHostAction::Overlay`

| 项目     | 内容                                                                                              |
| -------- | ------------------------------------------------------------------------------------------------- |
| 接口     | `bool GapHostFeature::SendRequest(GapHostRequest request, GapHostCallback onComplete = nullptr);` |
| Action   | `GapHostAction::Overlay`                                                                          |
| 说明     | 切换当前 Gap overlay 显示状态                                                                     |
| 参数     | 无；复用当前活动 Gap 会话                                                                         |
| callback | 禁止                                                                                              |

**返回值：**

- `true`：活动 overlay 已切换。
- `false`：没有活动会话、正在退出、线程错误或携带 callback。

**调用示例：**

```cpp
GapHostRequest request; // 创建 Gap 显示请求。
request.action = GapHostAction::Overlay; // 选择 Overlay 切换动作。
const bool isAccepted = // 保存立即切换结果。
    gap->SendRequest(std::move(request)); // 不传 bool 目标值，也不传 callback。
```

Overlay 是 Switch，不是显式 Set。`GapHostState` 不公开 overlay 可见位；
`isViewActive` 不等于 overlay 当前可见。

### 5.6 `GapHostAction::Exit`

| 项目     | 内容                                                                                              |
| -------- | ------------------------------------------------------------------------------------------------- |
| 接口     | `bool GapHostFeature::SendRequest(GapHostRequest request, GapHostCallback onComplete = nullptr);` |
| Action   | `GapHostAction::Exit`                                                                             |
| 说明     | 退出当前 Gap 会话                                                                                 |
| 参数     | 无                                                                                                |
| callback | 禁止                                                                                              |

**返回值：**

- `true`：退出已接纳并可能进入 pending。
- `false`：没有活动会话、已经 pending、线程错误或携带 callback。

**调用示例：**

```cpp
GapHostRequest request; // 创建 Gap 退出请求。
request.action = GapHostAction::Exit; // 选择 Exit 动作。
const bool isAccepted = // 保存退出接纳结果。
    gap->SendRequest(std::move(request)); // 发送退出且不传 callback。
```

Exit 接纳后继续处理 `TimerEvent`，直到 `isExitPending == false`。

### 5.7 `GapHostFeature::GetState`

| 项目     | 内容                                                |
| -------- | --------------------------------------------------- |
| 接口     | `GapHostState GetState() const;`                    |
| 说明     | 读取分析状态、当前批次统计、活动状态和退出 pending  |
| 调用线程 | 所有者线程                                          |
| 返回值   | 未挂载、线程错误或无活动/pending 会话时返回默认状态 |

**状态字段：**

| 字段            | 说明                                                                  |
| --------------- | --------------------------------------------------------------------- |
| `analysisState` | Idle、Running、Succeeded、Failed                                      |
| `statistics`    | 由供应商 header/region 直接投影的 object/void 体素数、体积和 porosity |
| `isViewActive`  | 当前存在可接受 Overlay/Exit 的活动会话                                |
| `isExitPending` | Exit 已接纳但尚未完成收口                                             |

**调用示例：**

```cpp
const GapHostState state = gap->GetState(); // 读取当前 Gap 状态快照。
const bool isExitDone = // 判断异步退出是否已经完全收口。
    state.analysisState == GapAnalysisState::Idle &&
    !state.isExitPending; // Idle 且非 pending 才表示退出完成。
const GapStatistics& stats = state.statistics; // 取得同一成功批次的统计。
qInfo() // 在 Qt 线程读取统计字段。
    << stats.objectVoxelCount
    << stats.voidVoxelCount
    << stats.objectVolumeMM3
    << stats.voidVolumeMM3
    << stats.porosityRatio; // 一次输出当前成功批次的全部聚合统计。
```

## 6. 关闭与解绑

正常关闭顺序：

1. 禁止 UI 再发送新请求。
2. 如果产品需要展示 Feature 的正常退出完成状态，可先发送 Crop/Gap Exit，并继续处理
   `TimerEvent`；这不是 Session 停止清理的必需前置步骤。
3. 在 Session 所有者线程调用 `Stop()`。Stop 统一解绑 Feature，并停止 timer、executor、
   observer、Hotkey 和 view lease；不要再手工交叉执行 `DetachFeature()`/`AttachTimer(false)`。
4. `Stop()` 返回 `false` 或状态为 `StopPending` 时，保留 Session、Feature、QVTK widget 和
   render window，在后续所有者事件循环中重试 `Stop()`；不得提前释放（`reset`）。
5. 只有 `GetIsStopped() == true` 后，才释放 Qt 保存的 Feature 句柄和 Session。
6. QVTK widget 解除 render window，再释放 VTK smart pointer 和 widget。

**调用示例：**

```cpp
const bool isStopped = session->Stop(); // 必须在 Session 所有者线程显式停止。
if (!isStopped || !session->GetIsStopped()) {
    qWarning() << "Host Stop pending:"
               << static_cast<int>(session->GetStopState());
    return false; // 保留全部对象，由下一次所有者事件循环任务重试。
}

gap.reset(); // Session 已解绑 Feature，此时释放 Qt 自己保存的句柄。
crop.reset();
session.reset(); // Stop 已完成，析构不再承担 VTK 停止清理。
primaryWidget->setRenderWindow( // 解除 QVTK widget 对 GenericOpenGL window 的使用。
    static_cast<vtkGenericOpenGLRenderWindow*>(nullptr)); // 显式传入空 window。
primaryWindow = nullptr; // 最后释放 Qt adapter 持有的 VTK smart pointer。
```

如果 Session 已在非所有者线程析构并进入全局 pending reaper，Qt 所有者线程调用
`VtkAppHostSession::SendPendingStops()` 重试，并以 `GetPendingStopCount() == 0` 作为 reaper
清空条件。该静态 fallback 不替代仍然存活的 Session 对象上的 `Stop()` 重试。

## 7. 请求接纳或结果失败的常见原因

| API/结果 | 常见原因 |
| --- | --- |
| `BuildSession` | topology 为空、id 重复、外部 window、视图配置或所有者线程无效 |
| `SendRequestResult() == false` | 结果回调为空、错误线程、Session 不可用、请求类型未知或执行前拒绝 |
| `HostResult::isSucceeded == false` | `SessionNotReady`、`WrongThread`、`RequestRejected` 或 `OperationFailed`；以 `errorCode/message` 为准 |
| View/Tool 结果失败 | `targetView` 为空/未命中、字段/枚举非法，或误以为 id 未命中会回退 role |
| Load/Reload 结果失败 | 路径为空、geometry 非法、buffer 数量不匹配、数据路由或异步提交失败 |
| Export 结果失败 | 输出目录为空、来源模式不支持、slice 目标或异步写出无效 |
| `AttachFeature` | 未先 Build、Feature 为空/配置非法、id 或对象重复、Feature 拒绝挂载 |
| Crop | 未挂载、正在 publishing、Action 字段缺失、没有已绑定 history 或回调规则错误 |
| Gap | 未挂载、Start 参数非法、已有活动/退出 pending、Overlay/Exit 携带回调 |
| Gap 回调为 false | 私有双 DLL 缺失、runtime 目录非法、模块身份不一致、DefX/结果校验或 overlay 显示失败 |
| `StartImageRead` 未接纳 | callback 为空、已有读取、队列满、正在 Stop 或 Session 不可用 |
| Image Read 结果失败 | 无 image、region/类型无效、预算不足、复制失败或取消 |
| `Stop() == false` | 非所有者线程，或 Feature/view lease/Hotkey 停止失败；检查 `GetStopState()` 并重试 |
| 异步已接纳但不完成 | Host timer 未绑定或 QVTK `TimerEvent` 未持续到达 |

## 8. 相机与光照开放边界

- 当前只开放 `HostViewResetRequest` 这一相机命令；没有相机值 DTO、Set Request 或状态读回字段，也不把 `vtkCamera*` 作为业务接口。
- 当前所谓光照控制实际是 `HostMaterialParams` 对 `vtkVolumeProperty` / `vtkProperty` 的 ambient、diffuse、specular、specularPower、opacity 和 shade/interpolation 响应，不是场景光源对象。
- 当前工程没有显式 `vtkLight` owner、稳定 light id、位置/颜色/强度状态真源、camera-follow 语义或视觉基线，因此不开放 `vtkLight*`，也不新增伪造的灯光 DTO。
- 后续若产品确实需要定向光，应先在 Render 层建立按 view 拥有的显式 light 集合、自动光源关闭规则、重建恢复、ID、读回和失败语义，再新增独立 Host 值对象请求。
