# MVVCVTK Qt Host API 使用手册

本文面向 Qt Widgets 上位机调用方，按“接口、说明、适用场景、参数、返回值、调用示例”
逐项说明公开 Host API。

## 1. 公共约定

### 1.1 公开头文件

| 头文件 | 公开内容 |
| --- | --- |
| `Host/VtkAppHostSession.h` | Session 生命周期、Feature/Timer 挂载、主体请求入口和 endpoint |
| `Host/Types/HostRequestTypes.h` | Load、Reload、View、Tool、Data Export、Slice Export 请求 |
| `Host/Types/HostValueTypes.h` | 视图目标、渲染参数和导出格式 |
| `Host/Types/HostSessionTypes.h` | Session、视图和外部 render window 配置 |
| `Host/Types/HostInputTypes.h` | Timer 和 standalone 热键配置 |
| `Host/CropHostFeature.h` | Crop 配置、请求、状态和回调 |
| `Host/GapHostFeature.h` | Gap 配置、请求、状态和回调 |

### 1.2 调用线程

- Session 构建、主体 `SendRequest()`、Feature `SendRequest()` 和 `GetState()` 都必须在
  创建 Session 的 Qt GUI/VTK owner thread 调用。
- Qt 只进入 `QApplication::exec()`，不调用 `VtkAppHostSession::Start()` 或
  `vtkRenderWindowInteractor::Start()`。
- Core 不负责切换到 Qt 线程。回调更新 Qt 对象时，调用方使用 `QPointer` 和
  `Qt::QueuedConnection`。
- Feature 异步完成和退出收口依赖 QVTK `TimerEvent` 持续到达。

### 1.3 返回值与回调

| 返回或回调 | 含义 |
| --- | --- |
| `bool == true` | 同步请求已提交，或异步请求已启动 |
| `bool == false` | 请求没有成功进入对应操作 |
| `HostCompleteCallback` | `void(bool isSuccess)`；供 Load、Reload、Data Export、Slice Export 使用 |
| `CropBuildCallback` | `void(CropBuildResult result)`；只供 Crop `BuildResult` 使用 |
| `GapHostCallback` | `void(bool isSuccess)`；只供 Gap `Start` 使用 |

View Set/Reset、Tool Set/Switch 不接受 callback。Crop 除 `BuildResult` 外不接受 callback；
Gap Overlay/Exit 不接受 callback。

可复用的 Qt completion：

```cpp
const QPointer<QMainWindow> owner(this); // 弱引用 Qt 窗口，避免异步完成后访问已析构对象。
const HostCompleteCallback onHostComplete = // 保存主体异步请求共用的完成回调。
    [owner](bool isSuccess) { // 接收 Host 报告的最终成功状态。
        if (!owner) { // 检查 Qt 对象是否仍然存活。
            return; // 对象已销毁时丢弃本次 UI 更新。
        }
        QMetaObject::invokeMethod( // 把 UI 更新排队到 owner 所在线程。
            owner.data(), // 指定接收 queued invocation 的 Qt 对象。
            [owner, isSuccess] { // 捕获生命周期门禁和最终结果。
                if (owner) { // 执行排队任务前再次检查对象生命期。
                    qInfo() << "Host completed:" << isSuccess; // 在 Qt 线程消费结果。
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

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `viewId` | `std::string` | 非空时优先按稳定 id 查找 |
| `isViewRoleUsed` | `bool` | `viewId` 为空时，是否启用 role 查找 |
| `viewRole` | `HostRenderViewRole` | role 查找使用的角色 |

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
5. View、Tool 和 Slice Export 要求有效目标；只有 Data Export 的空 `sourceView` 会回退
   到 Primary 视图。

## 2. Session API

### 2.1 创建 Session 与 `BuildSession`

| 项目 | 内容 |
| --- | --- |
| 接口 | `explicit VtkAppHostSession(HostSessionConfig config);` |
| 接口 | `bool BuildSession();` |
| 说明 | 构造并幂等构建一次 Host 会话 |
| 适用场景 | QVTK widget 已绑定外部 `vtkGenericOpenGLRenderWindow` 后 |
| 调用线程 | Qt GUI/VTK owner thread |

**参数：**

| 参数 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `config` | `HostSessionConfig` | 是 | `renderViews` 非空；声明顺序即 topology 顺序 |
| `renderViews[].id` | `std::string` | 是 | Session 内非空且唯一 |
| `renderViews[].role` | `HostRenderViewRole` | 是 | 视图业务角色 |
| `renderViews[].renderWindow` | `vtkSmartPointer<vtkRenderWindow>` | 否 | Qt 复用 QVTK 外部 window 时提供同一实例；为空时 Session 自建 |

**返回值：**

- `true`：首次构建成功，或同一 owner thread 上的重复构建复用既有 Session。
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

auto session = // Qt 以 unique_ptr 强持有整个 Host Session。
    std::make_unique<VtkAppHostSession>(std::move(config)); // 移交一次性 Session 配置。
const bool isBuilt = session->BuildSession(); // 在 owner thread 显式构建 Session。
if (!isBuilt) { // 检查构建结果。
    return false; // 构建失败时停止后续 Host 调用。
}
```

### 2.2 `AttachTimer`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool AttachTimer(const HostTimerConfig& config);` |
| 说明 | 在指定视图的 context 上绑定或卸载 Host timer hook |
| 适用场景 | Crop/Gap 异步完成、Feature tick 和退出收口 |
| 前置条件 | Session 可成功构建；启用时 `targetView` 必须命中有效 context |

**参数：**

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `isTimerEnabled` | `bool` | 是 | `true` 绑定；`false` 卸载当前 hook |
| `targetView` | `HostViewTarget` | 启用时是 | timer 事件来源视图 |

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool AttachHotkeys(const HostHotkeyConfig& config);` |
| 说明 | 替换 standalone VTK interactor 的主体热键入口 |
| 适用场景 | standalone 或显式保留 VTK 调试热键的宿主 |
| Qt 约束 | 生产 Qt action 直接发送具体 Request，通常不调用本接口 |

**参数：**

`HostHotkeyConfig` 可配置 context 输入、主体命令输入、工具切换键、数据/切片导出键、
退出键以及对应导出路径和来源视图。

**返回值：**

- `true`：热键配置已接入。
- `false`：Session 构建、owner thread 或输入配置无效。

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool AttachFeature(const std::shared_ptr<HostFeature>& feature);` |
| 说明 | 把一个 Feature 挂载到已经构建的 Session |
| 适用场景 | 接入 `CropHostFeature`、`GapHostFeature` 或其它 Host Feature |
| 前置条件 | 必须先 `BuildSession()`；Feature id 非空且未重复 |

**参数：**

| 参数 | 类型 | 必填 | 所有权 |
| --- | --- | --- | --- |
| `feature` | `std::shared_ptr<HostFeature>` | 是 | Session 保存 weak 引用；Qt 必须继续强持有 |

**返回值：**

- `true`：Feature 已挂载。
- `false`：Session 未构建、线程错误、Feature 无效、id 重复或 Feature 拒绝挂载。

**调用示例：**

```cpp
auto crop = std::make_shared<CropHostFeature>( // Qt 创建并强持有 Crop Feature。
    std::move(cropConfig)); // 把一次性 Crop 配置移交给 Feature。
const bool isCropAttached = // 保存立即挂载结果。
    session->AttachFeature(crop); // Session 只登记 Feature 的弱引用和输入绑定。
```

### 2.5 `DetachFeature`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool DetachFeature(const HostFeature& feature);` |
| 说明 | 从 Session 解绑一个已挂载 Feature |
| 适用场景 | Feature 停用、替换或 Qt 窗口关闭 |
| 前置条件 | owner thread；Feature 已挂载 |

**返回值：**

- `true`：Feature 已完成解绑。
- `false`：Session/线程/Feature 状态不满足解绑条件。

**调用示例：**

```cpp
const bool isCropDetached = // 保存 Crop 解绑结果。
    session->DetachFeature(*crop); // 先让 Session 移除输入和 Feature 注册。
if (isCropDetached) { // 只在解绑成功后释放 Qt 强引用。
    crop.reset(); // 销毁或释放 Qt 持有的 Crop Feature。
}
```

### 2.6 `Start`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool Start();` |
| 说明 | 渲染全部视图并进入阻塞的 standalone VTK event loop |
| 适用场景 | 仅 standalone executable |
| Qt 约束 | Qt 禁止调用；Qt 使用 `QApplication::exec()` |

**返回值：**

- `true`：standalone 循环正常退出。
- `false`：Session、启动视图或重复启动状态无效。

**standalone 调用示例：**

```cpp
const bool isStarted = session->Start(); // 仅 standalone 进入阻塞 VTK event loop。
```

### 2.7 `SendRequest`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool SendRequest(HostRequest&& request, HostCompleteCallback onComplete = nullptr);` |
| 说明 | 主体具体 Request 的唯一发送入口 |
| 适用场景 | Load、Reload、View、Tool、Data Export、Slice Export |
| 前置条件 | owner thread；调用方构造具体派生 Request |

**参数：**

| 参数 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `request` | `HostRequest&&` | 是 | 必须是 8 类已知具体 Request；发送后不再复用 |
| `onComplete` | `HostCompleteCallback` | 否 | 只允许四类异步数据请求使用 |

**返回值：**

- `true`：同步请求已提交，或异步请求已启动。
- `false`：Request 类型、线程、字段、目标或 callback 规则不合法。

**调用示例：**

```cpp
HostViewResetRequest request; // 创建一个具体的相机复位请求。
request.targetView = primary3D; // 指定必须命中的目标视图。
const bool isAccepted = // 保存同步请求的立即结果。
    session->SendRequest(std::move(request)); // 移交具体 Request，不传 callback。
```

### 2.8 `GetRenderViewEndpoints`

| 项目 | 内容 |
| --- | --- |
| 接口 | `const std::vector<HostRenderViewEndpoint>& GetRenderViewEndpoints();` |
| 说明 | 返回 Session 全部 endpoint 的只读引用 |
| 适用场景 | 枚举 Qt 已接入的全部 Host 视图 |
| 参数 | 无 |
| 返回值 | 构建失败时返回空集合；成功时按 topology 顺序返回 |
| 生命周期 | 引用和元素只在 Session topology 不变且存活期间有效 |

**调用示例：**

```cpp
const auto& endpoints = // 取得 Session 内部 endpoint 集合的只读引用。
    session->GetRenderViewEndpoints(); // 调用方不获得 endpoint 所指 VTK 对象的所有权。
for (const HostRenderViewEndpoint& endpoint : endpoints) { // 按 topology 顺序遍历。
    qInfo() << QString::fromStdString(endpoint.id); // 读取稳定 view id。
}
```

### 2.9 `GetRenderViewEndpoint`

| 项目 | 内容 |
| --- | --- |
| 接口 | `const HostRenderViewEndpoint* GetRenderViewEndpoint(const std::string& viewId);` |
| 说明 | 按稳定 id 查询一个 endpoint |
| 适用场景 | 核对 Qt widget 与 Host 是否使用同一 render window/interactor |
| 参数 | `viewId`：Session 内稳定视图 id |
| 返回值 | 命中时返回非拥有指针；失败时返回 `nullptr` |

**调用示例：**

```cpp
const HostRenderViewEndpoint* endpoint = // 声明非拥有 endpoint 指针。
    session->GetRenderViewEndpoint("primary-3d"); // 按稳定 id 查询主视图。
if (!endpoint || // 检查 id 是否命中。
    endpoint->renderWindow != primaryWindow.GetPointer()) { // 核对 Qt 与 Host 使用同一 window。
    return false; // window 不一致时拒绝继续接入。
}
```

### 2.10 `GetPrimaryEndpoint`

| 项目 | 内容 |
| --- | --- |
| 接口 | `const HostRenderViewEndpoint* GetPrimaryEndpoint();` |
| 说明 | 查询 Session 的首选 3D endpoint |
| 适用场景 | 获取默认渲染、TimerEvent 或状态核对使用的 3D endpoint |
| 参数 | 无 |
| 返回值 | 优先 Primary3D，其次 Composite3D，再退到首视图；失败返回 `nullptr` |

**调用示例：**

```cpp
const HostRenderViewEndpoint* primary = // 声明非拥有首选 endpoint 指针。
    session->GetPrimaryEndpoint(); // 按远端优先级解析首选视图。
if (!primary) { // 检查 Session 是否存在可用 endpoint。
    return false; // 没有可用视图时停止后续调用。
}
```

## 3. 主体 Request API

### 3.1 `HostLoadRequest`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool VtkAppHostSession::SendRequest(HostRequest&& request, HostCompleteCallback onComplete = nullptr);` |
| Request 类型 | `HostLoadRequest` |
| 说明 | 从 UTF-8 文件路径异步加载体数据 |
| 适用场景 | RAW 或 Data 层支持的体数据文件 |
| callback | 可选；报告最终加载结果 |

**参数：**

| 字段 | 类型 | 必填 | 约束 |
| --- | --- | --- | --- |
| `filePath` | `std::string` | 是 | 非空 UTF-8 路径 |
| `geometry.dimensions` | `std::array<int, 3>` | 是 | X/Y/Z；全零时仅 RAW 可从文件名末尾 `NxMxK` 推导 |
| `geometry.spacing` | `std::array<float, 3>` | 是 | 三轴物理间距 |
| `geometry.origin` | `std::array<float, 3>` | 是 | 输入体数据物理原点 |

**返回值：**

- `true`：加载任务已启动。
- `false`：路径、geometry、Primary view、线程或任务启动失败。

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
const bool isAccepted = session->SendRequest( // 启动异步加载并保存立即结果。
    std::move(request), // 移交 Request 及其路径和 geometry。
    onHostComplete); // 复用 Qt 生命周期安全的完成回调。
```

### 3.2 `HostReloadRequest`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool VtkAppHostSession::SendRequest(HostRequest&& request, HostCompleteCallback onComplete = nullptr);` |
| Request 类型 | `HostReloadRequest` |
| 说明 | 从调用方拥有的 float32 buffer 异步重载体数据 |
| 适用场景 | Qt 已在内存中准备好完整体素数据 |
| callback | 可选；报告最终重载结果 |

**参数：**

| 字段 | 类型 | 必填 | 约束 |
| --- | --- | --- | --- |
| `voxels` | `std::vector<float>` | 是 | X 最快、随后 Y/Z；Request 接管存储 |
| `geometry.dimensions` | `std::array<int, 3>` | 是 | 乘积必须等于 `voxels.size()` |
| `geometry.spacing` | `std::array<float, 3>` | 是 | 三轴物理间距 |
| `geometry.origin` | `std::array<float, 3>` | 是 | 输入体数据物理原点 |

**返回值：**

- `true`：重载任务已启动。
- `false`：buffer/layout、Primary view、线程或任务启动失败。

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
const bool isAccepted = session->SendRequest( // 启动异步 buffer 重载。
    std::move(request), // 移交 Request 和 voxels 所有权。
    onHostComplete); // 接收最终成功或失败结果。
```

### 3.3 `HostViewSetRequest`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool VtkAppHostSession::SendRequest(HostRequest&& request, HostCompleteCallback onComplete = nullptr);` |
| Request 类型 | `HostViewSetRequest` |
| 说明 | 以 partial patch 修改目标视图或 Session 共享显示状态 |
| 适用场景 | 模式、材质、ISO、窗宽窗位、cursor、显隐、质量等运行时调整 |
| callback | 禁止 |

**参数：**

`targetView` 必填。其余字段均为 optional；未填写的字段保留当前状态。

| 字段 | 最短赋值语句 | 主要约束 |
| --- | --- | --- |
| `mode` | `request.mode = HostRenderMode::IsoSurface;` | 有效 `HostRenderMode` |
| `material` | `request.material = HostMaterialParams{0.15, 0.75, 0.35, 24.0, 0.9, true};` | finite；opacity `[0,1]` |
| `materialPreset` | `request.materialPreset = HostMaterialPreset::Glossy;` | 不能同时给 material/opacity |
| `opacity` | `request.opacity = 0.4;` | `[0,1]` |
| `transferNodes` | `request.transferNodes = std::vector<HostTransferNode>{{0.0, 0.0, 0.0, 0.0, 0.0}, {1.0, 1.0, 1.0, 1.0, 1.0}};` | 非空；归一化分量 `[0,1]` |
| `transferPreset` | `request.transferPreset = HostTransferPreset::Percentile;` | 不能同时给 transferNodes |
| `iso` | `request.iso = 420.0;` | finite |
| `background` | `request.background = HostBackgroundColor{0.08, 0.08, 0.12};` | RGB `[0,1]` |
| `spacing` | `request.spacing = std::array<double, 3>{0.4, 0.4, 1.0};` | finite 且三轴大于零 |
| `windowLevel` | `request.windowLevel = HostWindowLevelParams{600.0, 120.0};` | width 大于零；center finite |
| `volumeQuality` | `request.volumeQuality = HostVolumeQualityParams{HostVolumeQuality::Custom, 1000, 0.7, true};` | 只允许 Volume/CompositeVolume |
| `gradientOpacity` | `request.gradientOpacity = std::vector<HostGradientOpacityNode>{{0.0, 0.0}, {120.0, 1.0}};` | 只允许 Volume/CompositeVolume |
| `isDenoiseOn` | `request.isDenoiseOn = true;` | 只允许 Volume/CompositeVolume |
| `cursor` | `request.cursor = HostCursorParams{{x, y, z}, -1};` | world finite；axis 为 `-1/0/1/2` |
| `visibility` | `request.visibility = visibility;` | 每个 optional 只修改一个共享显隐位 |
| `isAxesVisible` | `request.isAxesVisible = false;` | 只作用目标 context |

Request 会先整体校验；任一已提供字段非法时返回 `false`，不产生部分写入。

**返回值：**

- `true`：整个 patch 已提交。
- `false`：目标、字段、互斥关系、候选 mode、线程或 callback 规则无效。

**只调节 ISO：**

```cpp
HostViewSetRequest request; // 创建只包含本次修改的 View patch。
request.targetView = primary3D; // 指定必须命中的有效路由目标。
request.iso = 420.0; // 只写入新的等值面阈值，不构造其它当前值。
const bool isAccepted = // 保存整个 patch 的同步结果。
    session->SendRequest(std::move(request)); // 移交 patch，不传 callback。
```

**调节窗宽窗位：**

```cpp
HostViewSetRequest request; // 创建只包含窗宽窗位修改的 View patch。
request.targetView = topDown; // 指定有效写入锚点；该字段不能省略。
request.windowLevel = HostWindowLevelParams{ // 写入 Session 共享窗宽窗位。
    600.0, // windowWidth：显示灰阶窗口宽度，必须大于零。
    120.0 // windowCenter：显示灰阶窗口中心，必须 finite。
};
const bool isAccepted = // 保存整个 patch 的同步结果。
    session->SendRequest(std::move(request)); // 发送 targetView + windowLevel。
```

`topDown` 不是固定要求，任意有效视图都可以作为写入锚点；但只设置 `windowLevel` 而不设置
`targetView` 会返回 `false`。WW/WC 是 Session 共享状态，切片视图会同步消费该值。

**设置 Volume 质量、gradient opacity 和 denoise：**

```cpp
HostViewSetRequest request; // 创建一个原子 View patch。
request.targetView = primary3D; // 指定需要修改的 3D 视图。
request.mode = HostRenderMode::Volume; // 在同一 patch 中先给出合法候选 mode。
request.volumeQuality = HostVolumeQualityParams{ // 设置自定义体渲染质量。
    HostVolumeQuality::Custom, // 启用自定义质量参数。
    1000, // 设置重采样最大轴尺寸。
    0.7, // 设置采样距离。
    true // 启用 jitter。
};
request.gradientOpacity = // 设置梯度不透明度节点。
    std::vector<HostGradientOpacityNode>{
        { 0.0, 0.0 }, // 低梯度映射为透明。
        { 120.0, 1.0 } // 高梯度映射为不透明。
    };
request.isDenoiseOn = true; // 在同一 Volume patch 中启用去噪。
const bool isAccepted = // 保存原子 patch 的同步结果。
    session->SendRequest(std::move(request)); // 任一字段非法时整笔请求失败。
```

**设置共享显隐：**

```cpp
HostVisibilityParams visibility; // 创建共享业务元素显隐 patch。
visibility.isPlanes3DVisible = true; // 显示 3D 彩色参考切平面。
visibility.isCrosshairVisible = false; // 隐藏切片十字线。
visibility.isRulerVisible = true; // 显示 3D cube axes 标尺。

HostViewSetRequest request; // 创建 View patch 请求。
request.targetView = primary3D; // 指定有效写入入口。
request.visibility = visibility; // 只写入三个已显式提供的显隐位。
const bool isAccepted = // 保存显隐 patch 的同步结果。
    session->SendRequest(std::move(request)); // 发送请求且不传 callback。
```

### 3.4 `HostViewResetRequest`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool VtkAppHostSession::SendRequest(HostRequest&& request, HostCompleteCallback onComplete = nullptr);` |
| Request 类型 | `HostViewResetRequest` |
| 说明 | 复位目标视图相机 |
| 参数 | `targetView`：必填且必须命中有效 context/service |
| callback | 禁止 |

**返回值：**

- `true`：目标相机已复位并标记刷新。
- `false`：目标、线程或 callback 规则无效。

**调用示例：**

```cpp
HostViewResetRequest request; // 创建相机复位请求。
request.targetView = primary3D; // 指定需要复位的目标视图。
const bool isAccepted = // 保存同步复位结果。
    session->SendRequest(std::move(request)); // 发送一次性命令，不传 callback。
```

### 3.5 `HostToolSetRequest`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool VtkAppHostSession::SendRequest(HostRequest&& request, HostCompleteCallback onComplete = nullptr);` |
| Request 类型 | `HostToolSetRequest` |
| 说明 | 显式设置目标视图工具模式 |
| 参数 | `targetView` 必填；`toolMode` 为 Navigation 或 ModelTransform |
| callback | 禁止 |

**返回值：**

- `true`：目标工具模式已写入。
- `false`：目标、枚举、线程或 callback 规则无效。

**调用示例：**

```cpp
HostToolSetRequest request; // 创建显式工具模式请求。
request.targetView = primary3D; // 指定目标交互 context。
request.toolMode = HostToolMode::ModelTransform; // 写入模型变换模式。
const bool isAccepted = // 保存同步写入结果。
    session->SendRequest(std::move(request)); // 发送请求且不传 callback。
```

### 3.6 `HostToolSwitchRequest`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool VtkAppHostSession::SendRequest(HostRequest&& request, HostCompleteCallback onComplete = nullptr);` |
| Request 类型 | `HostToolSwitchRequest` |
| 说明 | 在 Navigation 和 ModelTransform 之间切换 |
| 参数 | `targetView`：必填且必须命中有效 context |
| callback | 禁止 |

**返回值：**

- `true`：工具模式已切换。
- `false`：目标、线程或 callback 规则无效。

**调用示例：**

```cpp
HostToolSwitchRequest request; // 创建二态工具切换请求。
request.targetView = primary3D; // 指定接收切换动作的目标 context。
const bool isAccepted = // 保存同步切换结果。
    session->SendRequest(std::move(request)); // 发送请求且不传 callback。
```

### 3.7 `HostDataExportRequest`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool VtkAppHostSession::SendRequest(HostRequest&& request, HostCompleteCallback onComplete = nullptr);` |
| Request 类型 | `HostDataExportRequest` |
| 说明 | 异步导出 RAW 或等值面网格 |
| 适用场景 | 保存当前数据快照为 RAW、PLY、STL 或 OBJ |
| callback | 可选；报告最终写出结果 |

**参数：**

| 字段 | 类型 | 必填 | 约束 |
| --- | --- | --- | --- |
| `outputPath` | `std::string` | 是 | UTF-8 输出目录，不是完整文件名 |
| `format` | `std::optional<HostDataExportFormat>` | 否 | Raw/Ply/Stl/Obj |
| `sourceView` | `HostViewTarget` | 否 | 空目标回退 Primary 视图 |

`format` 缺省时，Volume/CompositeVolume 推断 RAW，IsoSurface/CompositeIsoSurface
推断 PLY；slice mode 不支持 Data Export。

**返回值：**

- `true`：导出任务已启动。
- `false`：目录、来源模式、目标、线程或任务启动失败。

**调用示例：**

```cpp
HostDataExportRequest request; // 创建数据导出请求。
request.outputPath = // 写入 UTF-8 输出目录。
    outputDir.toUtf8().toStdString(); // 创建拥有自身存储的目录字符串。
request.format = HostDataExportFormat::Ply; // 显式选择 PLY 网格格式。
request.sourceView = primary3D; // 冻结主 3D 视图对应的数据/ISO/变换状态。
const bool isAccepted = session->SendRequest( // 启动异步导出任务。
    std::move(request), // 移交导出目录、格式和来源选择器。
    onHostComplete); // 接收最终文件写出结果。
```

Data 层生成 `<dimX>x<dimY>x<dimZ>_transform.ext`。PLY/STL/OBJ 不携带界面材质、
传递函数或纹理，Request 也没有颜色参数；屏幕预览颜色不是网格文件的颜色契约。

### 3.8 `HostSliceExportRequest`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool VtkAppHostSession::SendRequest(HostRequest&& request, HostCompleteCallback onComplete = nullptr);` |
| Request 类型 | `HostSliceExportRequest` |
| 说明 | 按目标切片方向异步逐层导出 PNG |
| callback | 可选；报告最终写出结果 |

**参数：**

| 字段 | 类型 | 必填 | 约束 |
| --- | --- | --- | --- |
| `outputDir` | `std::string` | 是 | UTF-8 输出目录 |
| `sourceView` | `HostViewTarget` | 是 | 必须命中 slice role |
| `angleDeg` | `std::optional<double>` | 否 | 平面内旋转角；必须 finite |

**返回值：**

- `true`：切片导出任务已启动。
- `false`：目录、切片目标、角度、线程或任务启动失败。

**调用示例：**

```cpp
HostSliceExportRequest request; // 创建切片导出请求。
request.outputDir = // 写入 UTF-8 输出目录。
    outputDir.toUtf8().toStdString(); // 创建拥有自身存储的目录字符串。
request.sourceView = topDown; // 指定必须命中的俯视切片来源。
request.angleDeg = 30.0; // 在当前切片方向上增加 30 度平面内旋转。
const bool isAccepted = session->SendRequest( // 启动异步逐层 PNG 导出。
    std::move(request), // 移交目录、来源和可选角度。
    onHostComplete); // 接收最终导出结果。
```

## 4. CropHostFeature API

### 4.1 创建并挂载 Crop

| 项目 | 内容 |
| --- | --- |
| 接口 | `explicit CropHostFeature(CropHostConfig config);` |
| 说明 | 创建 Crop Feature 并通过 Session 挂载 |
| 适用场景 | Qt 需要 Box/Plane 交互、history 或结果物化 |
| 所有权 | Qt 强持有 `shared_ptr<CropHostFeature>`；Session 只保存 weak 引用 |
| 前置条件 | Session 已 `BuildSession()` |

**参数：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `defaultTarget` | `CropHostTarget` | 热键或默认动作使用的 Crop 目标 |
| `inputViews` | `HostViewTargets` | 可选 VTK interactor 输入视图 |
| `keys` | `CropHostKeys` | 可选 Crop 热键 |
| `defaultTarget.referenceView` | `HostViewTarget` | Crop 参考 3D 视图 |
| `defaultTarget.targetViews` | `HostViewTargets` | Crop 效果作用的视图集合 |
| `defaultTarget.isTargetViewsUsed` | `bool` | 是否显式使用 `targetViews` |
| `defaultTarget.isStatusVisible` | `bool` | 是否输出 Crop 状态信息 |
| `defaultTarget.source` | `CropHostSource` | CurrentImage 或 RegisteredPolyData |

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

| 方法 | Qt 调用规则 |
| --- | --- |
| `GetFeatureId()` | 可查询稳定 id，业务通常无需调用 |
| `AttachHost(const HostFeatureContext&)` | 框架钩子；Qt 不直接调用 |
| `DetachHost()` | 框架钩子；Qt 使用 `session->DetachFeature(*crop)` |
| `OnHostTick()` | 框架钩子；由 Session timer 驱动 |
| `SendRequest(CropHostRequest, CropBuildCallback)` | Qt 唯一 Crop 业务入口 |
| `GetState()` | Qt 在 owner thread 读取状态 |

### 4.3 `CropHostAction::Start`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::Start` |
| 说明 | 建立或更新 Crop binding，不创建新的 Box/Plane widget |
| 参数 | `target` 必填 |
| callback | 禁止 |

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::Box` |
| 说明 | 建立 binding 并启动一个新的 Box 裁切 widget |
| 参数 | `target` 必填 |
| callback | 禁止 |

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::Plane` |
| 说明 | 建立 binding 并启动一个新的 Plane 裁切 widget |
| 参数 | `target` 必填 |
| callback | 禁止 |

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::Mode` |
| 说明 | 设置 Crop 当前编辑模式 |
| 参数 | `target` 和 `removalMode` 必填 |
| callback | 禁止 |

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::Previous` |
| 说明 | 回退到上一个有效 Crop history 节点 |
| 参数 | 无；复用当前 active binding |
| callback | 禁止 |

**返回值：**

- `true`：Previous 已在当前 history 上执行。
- `false`：没有 active binding、线程错误或正在 publishing。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop history 请求。
request.action = CropHostAction::Previous; // 选择回退动作。
const bool isAccepted = // 保存回退结果。
    crop->SendRequest(std::move(request)); // 使用当前 active binding，不重复传 target。
```

### 4.8 `CropHostAction::Next`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::Next` |
| 说明 | 前进到下一个有效 Crop history 节点 |
| 参数 | 无；复用当前 active binding |
| callback | 禁止 |

**返回值：**

- `true`：Next 已在当前 history 上执行。
- `false`：没有 active binding、线程错误或正在 publishing。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop history 请求。
request.action = CropHostAction::Next; // 选择前进动作。
const bool isAccepted = // 保存前进结果。
    crop->SendRequest(std::move(request)); // 使用当前 active binding，不重复传 target。
```

### 4.9 `CropHostAction::Node`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::Node` |
| 说明 | 跳转到指定 Crop history 节点数 |
| 参数 | `nodeCount` 必填；复用当前 active binding |
| callback | 禁止 |

**返回值：**

- `true`：已跳转到指定有效节点数。
- `false`：缺少 `nodeCount`、没有 active binding、线程错误或正在 publishing。

**调用示例：**

```cpp
CropHostRequest request; // 创建 Crop history 跳转请求。
request.action = CropHostAction::Node; // 选择按节点数跳转。
request.nodeCount = 2; // 指定目标有效节点数。
const bool isAccepted = // 保存节点跳转结果。
    crop->SendRequest(std::move(request)); // 使用当前 active binding 执行跳转。
```

Previous、Next 和 Node 在尚未建立 active binding 或已经 Exit 后返回 `false`。

### 4.10 `CropHostAction::BuildResult`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::BuildResult` |
| 说明 | 异步物化当前 Crop 结果 |
| 参数 | `target` 必填 |
| callback | 必填；这是唯一允许 callback 的 Crop Action |

**返回值：**

- `true`：物化任务已启动。
- `false`：目标、输入、线程、publishing 状态或 callback 规则无效。

`CropBuildResult` 提供 `isSucceeded`、`failureReason`、输入版本、节点数以及结果
`imageData`/`maskImage`/`polyData`。输入版本变化时返回
`CropFailure::VersionMismatch`，旧结果不覆盖新数据。

`CropFailure`：

| 值 | 含义 |
| --- | --- |
| `None` | 没有失败 |
| `NoImage` | image 路径没有绑定输入图像 |
| `NoPolyData` | PolyData 路径没有绑定输入网格 |
| `BadBounds` | 请求 bounds 本身非法 |
| `OutOfBounds` | bounds 超出输入数据范围 |
| `NoBackend` | 请求组合没有可执行后端 |
| `BadBuildMode` | image 物化语义不受当前后端支持 |
| `LowRam` | 内存不足 |
| `MaskFailed` | 三维 mask 生成失败 |
| `ImageFailed` | 输出 image 生成失败 |
| `ClipFailed` | Box outline PolyData 生成失败 |
| `BadInput` | 历史、快照或物化参数无效 |
| `EmptyResult` | 没有保留任何有效点 |
| `Busy` | 已有物化任务执行中 |
| `VersionMismatch` | 输入版本或数据源已经变化 |
| `WorkerStartFailed` | worker 未能启动 |
| `WorkerFailed` | worker 异常终止 |

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::SetPolyData` |
| 说明 | 注册 Crop 可选择的 PolyData 输入 |
| 参数 | 非空 `polyData`；大于零且严格递增的 `sourceVersion` |
| callback | 禁止 |

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::ClearPolyData` |
| 说明 | 清除已注册 PolyData 和版本 |
| 参数 | 无 |
| callback | 禁止 |

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::RestoreOriginal` |
| 说明 | 恢复首次成功物化前保存的原始 image |
| 参数 | 无 |
| callback | 禁止 |

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool CropHostFeature::SendRequest(CropHostRequest request, CropBuildCallback onComplete = nullptr);` |
| Action | `CropHostAction::Exit` |
| 说明 | 退出当前 Crop 编辑并清除活动视图 |
| 参数 | 无 |
| callback | 禁止 |

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

| 项目 | 内容 |
| --- | --- |
| 接口 | `CropHostState GetState() const;` |
| 说明 | 读取 Crop history、活动状态和 publishing 状态 |
| 调用线程 | owner thread |
| 返回值 | 未挂载或线程错误时返回默认状态 |

**调用示例：**

```cpp
const CropHostState state = crop->GetState(); // 读取当前 Crop UI 状态快照。
const bool canBuild = // 计算物化按钮是否可用。
    state.isActive && !state.isPublishing; // 仅活动且未发布时允许物化。
const bool canGoPrevious = // 计算回退按钮是否可用。
    state.isActive && state.history.nodeCount > 0; // 至少存在一个有效节点时允许回退。
```

## 5. GapHostFeature API

### 5.1 创建并挂载 Gap

| 项目 | 内容 |
| --- | --- |
| 接口 | `explicit GapHostFeature(GapHostConfig config);` |
| 说明 | 创建 Gap Feature 并通过 Session 挂载 |
| 适用场景 | Qt 需要启动 Gap 分析、切换 overlay 或读取统计 |
| 所有权 | Qt 强持有 `shared_ptr<GapHostFeature>`；Session 只保存 weak 引用 |
| 前置条件 | 有效 `defaultStart`、非空 `inputViews`、Overlay/Exit 有效 chord |

**参数：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `defaultStart` | `GapHostStartParams` | 热键或默认启动使用的完整分析参数 |
| `inputViews` | `HostViewTargets` | 接收 Gap Feature 输入的视图集合，不能为空 |
| `keys` | `GapHostKeys` | Overlay 与 Exit chord，二者都必须有效 |
| `defaultStart.targetViews` | `HostViewTargets` | 分析结果作用的 mesh/slice 视图 |
| `defaultStart.surface` | `GapSurfaceConfig` | ISO 来源与阈值 |
| `defaultStart.voidParams` | `GapVoidParams` | 空洞候选、体积和腐蚀参数 |

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
gapStart.surface.isoMode = // 选择数据范围比例 ISO。
    GapIsoMode::DataRangeRatio; // 由输入 scalar range 解析实际阈值。
gapStart.surface.dataRangeRatio = 0.55; // 设置范围内 55% 的 ISO。
gapStart.voidParams.grayMin = 0.0f; // 保持通过校验的灰度下限。
gapStart.voidParams.grayMax = 0.15f; // 设置当前生效的候选灰度上限。
gapStart.voidParams.minVolumeMM3 = 0.0001; // 设置最小保留体积。
gapStart.voidParams.erosionIterations = 2; // 设置六邻域腐蚀轮数。

GapHostConfig gapConfig; // 创建 Gap Feature 一次性配置。
gapConfig.defaultStart = gapStart; // 设置热键和默认启动使用的完整参数。
gapConfig.inputViews.viewIds = { "primary-3d" }; // 设置接收 Feature 输入的视图。
gapConfig.keys.switchOverlay.keyCode = 'j'; // 配置必需的 Overlay chord。
gapConfig.keys.exit.keySym = "Escape"; // 配置必需的 Exit chord。

auto gap = std::make_shared<GapHostFeature>( // Qt 创建并强持有 Gap Feature。
    std::move(gapConfig)); // 移交一次性 Gap 配置。
const bool isGapAttached = // 保存 Feature 立即挂载结果。
    session->AttachFeature(gap); // 由 Session 注入 HostFeatureContext。
```

### 5.2 Gap 框架钩子索引

以下方法是 `HostFeature` 框架钩子，不是 Qt 业务调用入口。Qt 只调用
`session->AttachFeature()`、`session->DetachFeature()`、Gap `SendRequest()` 和
`GetState()`。

| 方法 | Qt 调用规则 |
| --- | --- |
| `GetFeatureId()` | 可查询稳定 id，业务通常无需调用 |
| `AttachHost(const HostFeatureContext&)` | 框架钩子；Qt 不直接调用 |
| `DetachHost()` | 框架钩子；Qt 使用 `session->DetachFeature(*gap)` |
| `OnHostTick()` | 框架钩子；由 Session timer 驱动 |
| `SendRequest(GapHostRequest, GapHostCallback)` | Qt 唯一 Gap 业务入口 |
| `GetState()` | Qt 在 owner thread 读取状态和统计 |

### 5.3 `GapHostAction::Start`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool GapHostFeature::SendRequest(GapHostRequest request, GapHostCallback onComplete = nullptr);` |
| Action | `GapHostAction::Start` |
| 说明 | 使用完整 `GapHostStartParams` 启动一批新分析 |
| 参数 | `start` 必填 |
| callback | 可选；报告本批分析结果 |

**Start 参数：**

| 字段 | 说明与约束 |
| --- | --- |
| `targetViews` | 非空；至少解析出一个可用 mesh 或 slice 目标 |
| `surface.isoMode` | DataRangeRatio 或 AbsoluteValue |
| `surface.dataRangeRatio` | ratio 模式下必须位于 `[0,1]` |
| `surface.absoluteIsoValue` | absolute 模式下直接使用的输入标量阈值 |
| `voidParams.grayMax` | 当前生效的候选灰度上限 |
| `voidParams.minVolumeMM3` | 当前生效；大于等于零 |
| `voidParams.erosionIterations` | 当前生效；大于等于零 |
| `grayMin/angleThresholdDeg/tensorWindowSize` | 当前预留但仍参与有限值和范围校验 |

**返回值：**

- `true`：Gap 分析任务已启动。
- `false`：Feature、线程、输入快照、目标、参数或当前会话状态无效。

**按数据范围比例启动：**

```cpp
GapHostRequest request; // 创建 Gap 请求。
request.action = GapHostAction::Start; // 选择启动新分析。
request.start = gapStart; // 提供完整 Start 参数，而不是局部 patch。

const QPointer<QMainWindow> owner(this); // 保存 Qt 窗口生命期门禁。
const bool isAccepted = gap->SendRequest( // 启动 Gap 异步分析。
    std::move(request), // 移交完整 Start 参数。
    [owner](bool isSuccess) { // 接收本批分析最终结果。
        if (!owner) { // 检查 Qt 对象是否仍然存活。
            return; // 对象已销毁时丢弃结果。
        }
        QMetaObject::invokeMethod( // 把 UI 更新排队到 Qt 线程。
            owner.data(), // 指定 queued invocation 的接收对象。
            [owner, isSuccess] { // 捕获生命期门禁和最终结果。
                if (owner) { // 执行前再次检查对象。
                    qInfo() << "Gap completed:" << isSuccess; // 在 Qt 线程消费结果。
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

### 5.4 `GapHostAction::Overlay`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool GapHostFeature::SendRequest(GapHostRequest request, GapHostCallback onComplete = nullptr);` |
| Action | `GapHostAction::Overlay` |
| 说明 | 切换当前 Gap overlay 显示状态 |
| 参数 | 无；复用当前活动 Gap 会话 |
| callback | 禁止 |

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

### 5.5 `GapHostAction::Exit`

| 项目 | 内容 |
| --- | --- |
| 接口 | `bool GapHostFeature::SendRequest(GapHostRequest request, GapHostCallback onComplete = nullptr);` |
| Action | `GapHostAction::Exit` |
| 说明 | 退出当前 Gap 会话 |
| 参数 | 无 |
| callback | 禁止 |

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

Exit 接纳后继续 pump TimerEvent，直到 `isExitPending == false`。

### 5.6 `GapHostFeature::GetState`

| 项目 | 内容 |
| --- | --- |
| 接口 | `GapHostState GetState() const;` |
| 说明 | 读取分析状态、当前批次统计、活动状态和退出 pending |
| 调用线程 | owner thread |
| 返回值 | 未挂载、线程错误或无活动/pending 会话时返回默认状态 |

**状态字段：**

| 字段 | 说明 |
| --- | --- |
| `analysisState` | Idle、Running、Succeeded、Failed |
| `statistics` | object/void 体素数、体积和 porosity |
| `isViewActive` | 当前存在可接受 Overlay/Exit 的活动会话 |
| `isExitPending` | Exit 已接纳但尚未完成收口 |

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
2. 向活动 Crop/Gap 发送 Exit，并继续 pump 必要 TimerEvent。
3. 调用 `DetachFeature()`。
4. 释放 Qt 持有的 Feature `shared_ptr`。
5. 用 `AttachTimer()` 卸载 Host timer hook。
6. 释放 Session。
7. QVTK widget 解除 render window，再释放 VTK smart pointer 和 widget。

**调用示例：**

```cpp
CropHostRequest cropExit; // 创建 Crop 退出请求。
cropExit.action = CropHostAction::Exit; // 选择 Crop Exit。
const bool isCropExitAccepted = // 保存 Crop 退出结果。
    crop->SendRequest(std::move(cropExit)); // 禁止新请求前尽力退出 Crop。

GapHostRequest gapExit; // 创建 Gap 退出请求。
gapExit.action = GapHostAction::Exit; // 选择 Gap Exit。
const bool isGapExitAccepted = // 保存 Gap 退出接纳结果。
    gap->SendRequest(std::move(gapExit)); // 后续继续 pump pending tick。

const bool isGapDetached = // 保存 Gap 解绑结果。
    session->DetachFeature(*gap); // 先从 Session 移除 Gap。
const bool isCropDetached = // 保存 Crop 解绑结果。
    session->DetachFeature(*crop); // 再从 Session 移除 Crop。

gap.reset(); // 释放 Qt 持有的 Gap 强引用。
crop.reset(); // 释放 Qt 持有的 Crop 强引用。

HostTimerConfig timer; // 创建 timer 卸载配置。
timer.isTimerEnabled = false; // 请求移除当前 Host timer hook。
const bool isTimerDetached = // 保存 timer 卸载结果。
    session->AttachTimer(timer); // 在 Session 仍存活时卸载 hook。

session.reset(); // 在 QVTK window/widget 之前销毁 Session。
primaryWidget->setRenderWindow( // 解除 QVTK widget 对 GenericOpenGL window 的使用。
    static_cast<vtkGenericOpenGLRenderWindow*>(nullptr)); // 显式传入空 window。
primaryWindow = nullptr; // 最后释放 Qt adapter 持有的 VTK smart pointer。
```

## 7. `false` 的常见原因

| API | 常见原因 |
| --- | --- |
| `BuildSession` | topology 为空、id 重复、外部 window 或视图配置无效 |
| 主体 `SendRequest` | 非 owner thread、未知具体 Request、字段非法、目标未命中、callback 不允许 |
| View/Tool | `targetView` 为空；id 未命中时误以为会回退 role |
| Load/Reload | 路径为空、geometry 非法、buffer 数量不匹配 |
| Export | 输出目录为空、来源模式不支持、slice 目标无效 |
| `AttachFeature` | 未先 Build、Feature 配置非法、id 重复或 Qt 未用 `shared_ptr` 强持有 |
| Crop | 未挂载、正在 publishing、Action 字段缺失、无 active binding、callback 规则错误 |
| Gap | 未挂载、Start 参数非法、已有活动/退出 pending、Overlay/Exit 携带 callback |
| 异步已接纳但不完成 | Host timer 未绑定或 QVTK `TimerEvent` 未持续到达 |
