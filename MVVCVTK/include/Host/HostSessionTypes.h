#pragma once

#include "AppTypes.h"

#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

class vtkRenderer;
class vtkRenderWindowInteractor;

// role 是 host/session 层给窗口打的业务标签，不是固定窗口序号。
// 为什么放在公共类型里：Qt / 上位机需要按角色把自己的 QVTKOpenGLNativeWidget 映射回 session，
// 而 feature 只关心“哪个角色能作为参考 / overlay 目标”，不应该知道窗口创建顺序。
enum class HostRenderViewRole {
    Primary3D,
    Composite3D,
    TopDownSlice,
    FrontBackSlice,
    LeftRightSlice,
    Auxiliary
};

// 独立 VTK 调试入口的输入协议。它由 main 或上位机适配层填充，由 host observer 消费。
// 为什么不放进 feature：固定按键是宿主交互事实，feature 只暴露 Activate/Toggle/Exit 等稳定命令。
// 字段默认保持未分配，是为了让 Qt / 上位机创建 session 后不会隐式继承调试程序的快捷键。
struct HostHotkeyBindings {
    // RenderContext 层的模型变换模式切换键，只服务独立 VTK 调试窗口。
    char modelTransformToggleKey = 0;
    // 调试导出键：保存当前变换后的体数据，真实上位机应走显式命令。
    char saveTransformedDataKey = 0;
    // 调试导出键：按当前切片角度保存 slice 图片。
    char saveSliceImagesKey = 0;
    // 裁切盒交互模式切换键；按键只触发 host 命令，不进入裁切插件。
    char cropToggleKey = 0;
    // 平面裁切交互模式切换键；与 cropToggleKey 分开是为了保持两种 widget 状态边界。
    char planarCropToggleKey = 0;
    // 孔隙 overlay 键：未进入显示模式时进入，已进入时只切换显隐。
    char gapOverlayToggleKey = 0;
    // 调试预览键：保留平面法向内侧。
    char keepInsidePreviewKey = 0;
    // 调试预览键：移除平面法向内侧。
    char removeInsidePreviewKey = 0;
    // 调试提交键的主键位；observer 额外要求 Ctrl，避免裸数字键触发 VTK 内建行为。
    char submitKey = 0;
    // 使用 key symbol 而不是 char，是因为 Escape 这类控制键没有稳定可打印字符。
    std::string exitKeySym;
};

// 初始体数据的物理几何配置，由 main / 上位机数据命令提供，由 VtkAppHostSession 加载链路消费。
// RAW 体数据没有自描述的物理坐标，所以 spacing/origin 必须作为同一组外部事实传入。
// 这里不提供默认构造，是为了避免 session 在缺少上位机元数据时悄悄使用假 geometry。
struct InitialVolumeGeometryConfig {
    explicit InitialVolumeGeometryConfig(
        std::array<float, 3> spacingValues,
        std::array<float, 3> originValues)
        : spacing(spacingValues)
        , origin(originValues)
    {
    }

    // 体素间距，按 input image 的 X/Y/Z 轴顺序传入；裁切和孔隙体积计算都依赖这个尺度。
    std::array<float, 3> spacing;
    // 输入模型坐标原点，和 spacing 一起决定 index -> physical point 的仿射位置。
    std::array<float, 3> origin;
};

// 初始加载只是 host 启动阶段的可选数据命令，不是算法或 feature 的隐式前置条件。
// 路径和 geometry 默认都为空，是因为它们属于上位机 / 调试入口的外部事实，不能写进通用 session 配置。
struct InitialVolumeLoadConfig {
    // true 表示宿主明确要求 session 启动后加载 filePath；false 表示等待后续数据命令。
    bool enableInitialLoad = false;
    // 外部体数据路径。空路径即使 enableInitialLoad=true 也会被拒绝，避免 DataManager 进入不明确 I/O。
    std::string filePath;
    // RAW 必需的物理几何元数据；optional 表示“宿主尚未下发”，不是使用默认值。
    std::optional<InitialVolumeGeometryConfig> geometry;
    // 直方图仅服务调试诊断，放在 host 加载配置里，不让图像分析服务记住某个 UI 偏好。
    int histogramBinCount = 2048;
    // 空路径表示不导出直方图图片；这样不会因为默认写死路径导致 vtkPNGWriter 报错。
    std::string histogramFilePath = "";
};

// 每个 config 描述一个宿主视图的创建/接管意图，由上位机 / main 填充，由 HostRenderViewSet 消费。
// 为什么只放窗口事实：窗口数量、role、是否参与预览属于 host 拓扑，不能固化到 feature 插件。
struct HostRenderViewConfig {
    // 稳定视图标识，供 Qt widget 映射和后续命令按 id 定向；为空时仅由独立调试入口生成兜底 id。
    std::string id;
    // 视图业务角色，供裁切 reference、overlay 目标和主循环承载点选择。
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    // 单个窗口的渲染初始配置，仍复用现有 WindowConfig，避免 HostRenderViewSet 重写渲染细节。
    WindowConfig window;
    // Qt host 可注入 vtkGenericOpenGLRenderWindow；host 只接收 VTK 基类，避免把 Qt widget 类型带入业务层。
    vtkSmartPointer<vtkRenderWindow> renderWindow;
    // true 表示当请求允许使用配置默认预览窗口时，此视图可接收裁切 preview overlay。
    bool includeInCropPreview = true;
    // 独立 VTK host 只能选择一个 interactor 进入阻塞 Start；Qt host 不依赖这个字段。
    bool startStandaloneEventLoop = false;
};

// endpoint 是 Initialize 之后给宿主层使用的非拥有观察视图。
// 这里不暴露 service，是为了避免 Qt / 上位机绕过 host/session 直接改 feature 或 core 状态。
struct HostRenderViewEndpoint {
    // 与 HostRenderViewConfig::id 对应，外部宿主用它把 endpoint 绑定到自己的 widget。
    std::string id;
    // 与 HostRenderViewConfig::role 对应，外部宿主可按角色查找主 3D 或切片窗口。
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    // endpoint 只暴露非拥有指针；真实生命周期由 VtkAppHostSession/外部 Qt window 共同持有。
    vtkRenderer* renderer = nullptr;
    vtkRenderWindow* renderWindow = nullptr;
    vtkRenderWindowInteractor* interactor = nullptr;
};

// RenderContext 热键只服务独立 VTK 调试宿主，例如模型变换和本地导出。
// 这一层不触发 feature，只让 StdRenderContext 暴露现有工具能力；Qt / 上位机应默认关闭。
struct HostRenderContextInputConfig {
    // false 时不向 RenderContext 安装任何 host 按键回调。
    bool enableStandaloneHotkeys = false;
    // 按 id 指定哪些窗口接收 RenderContext 调试键；空列表不代表所有窗口。
    std::vector<std::string> targetViewIds;
    // 按 role 指定哪些窗口接收 RenderContext 调试键；与 id 取并集。
    std::vector<HostRenderViewRole> targetViewRoles;
};

// 裁切激活请求把“参考窗口”和“预览窗口”拆开描述。
// 调用方：上位机命令或 standalone hotkey；消费方：HostFeatureBindings::ConfigureOrthogonalCrop。
// 参考窗口提供坐标互转与 widget interactor，预览窗口只接收 overlay/dirty 刷新。
struct HostOrthogonalCropActivationRequest {
    // 优先级最高的参考窗口选择方式；适合 Qt 已经拿到具体窗口 id 的场景。
    std::string referenceViewId;
    // referenceViewId 为空时，是否允许按 role 选择第一个参考窗口。
    bool useReferenceRole = false;
    // role fallback 的参考窗口角色，默认 Primary3D 只是语义默认，不代表固定第一个窗口。
    HostRenderViewRole referenceRole = HostRenderViewRole::Primary3D;
    // 显式预览窗口 id 列表；空列表表示调用方没有指定，不自动全选。
    std::vector<std::string> previewViewIds;
    // 显式预览窗口 role 列表；与 previewViewIds 取并集。
    std::vector<HostRenderViewRole> previewViewRoles;
    // true 才允许退回 HostRenderViewConfig::includeInCropPreview，避免漏配请求时污染所有窗口。
    bool useConfiguredPreviewViews = false;
};

// 裁切预览命令只表达“保留哪一侧”的业务意图，底层 CropRemovalMode 仍由裁切模块维护。
enum class HostCropPreviewMode {
    KeepInside,
    RemoveInside
};

// 孔隙分析显示请求只决定 overlay 投递目标；是否运行算法由显式进入显示模式后再触发。
// 这样“显示/隐藏 overlay”和“是否退出孔隙分析模式”可以分开处理。
struct HostGapAnalysisActivationRequest {
    // 显式 overlay 目标 id 列表；空列表不代表所有窗口。
    std::vector<std::string> targetViewIds;
    // 显式 overlay 目标 role 列表；与 targetViewIds 取并集。
    std::vector<HostRenderViewRole> targetViewRoles;
    // true 才允许退回默认 overlay role，防止未指定目标时全局接管。
    bool useDefaultOverlayRoles = false;
};

// 独立 VTK host 的 feature 输入绑定，由 main 配置，由 HostFeatureBindings 安装到指定窗口。
// 热键监听范围和 feature 激活目标分开写，是为了避免“哪个窗口收键”被误当成“feature 作用在哪些窗口”。
struct HostCommandInputConfig {
    // false 时不安装 standalone feature observer；Qt / 上位机应走 VtkAppHostSession 的显式命令。
    bool enableStandaloneHotkeys = false;
    // 哪些窗口监听 standalone feature 按键；空列表不代表所有窗口。
    std::vector<std::string> targetViewIds;
    // 哪些 role 监听 standalone feature 按键；与 targetViewIds 取并集。
    std::vector<HostRenderViewRole> targetViewRoles;
    // hotkey 触发裁切时复用的业务目标请求，和监听范围相互独立。
    HostOrthogonalCropActivationRequest orthogonalCropRequest;
    // hotkey 触发孔隙显示时复用的业务目标请求，和监听范围相互独立。
    HostGapAnalysisActivationRequest gapAnalysisRequest;
};
