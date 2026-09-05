#pragma once

#include "Data/DataGraphTypes.h"

#include "Host/Types/HostValueTypes.h"
#include "Host/Types/HostViewTypes.h"

#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class vtkRenderer;
class vtkRenderWindowInteractor;

struct HostHotkeyConfig {
    // context 输入负责窗口内工具切换；command 输入负责数据动作和退出命令。
    bool isContextInputEnabled = false;
    HostViewTargets contextInputViews;
    bool isCommandInputEnabled = false;
    HostViewTargets commandInputViews;
    char modelSwitchKey = 0;
    char dataExportKey = 0;
    char sliceExportKey = 0;
    std::string exitKeySym; // 使用 VTK key symbol，支持 Escape 等非字符键。
    std::string dataExportPath;
    std::optional<HostDataExportFormat> dataExportFormat;
    HostViewTarget dataSourceView;
    std::string sliceExportDir;
    HostViewTarget sliceSourceView;
    std::optional<double> sliceAngleDeg;
};

struct HostTimerConfig {
    bool isTimerEnabled = false; // false 表示卸载当前 host timer handler。
    HostViewTarget targetView{ "", false, HostRenderViewRole::Primary3D };
};

struct HostViewInitConfig {
    // has* 是显式写入位：字段本身保留可用默认值，但只有对应 has* 为 true 时才覆盖策略状态。
    HostRenderMode viewMode = HostRenderMode::IsoSurface; // 首次构建的主策略模式。
    HostMaterialParams material; // 始终作为预初始化材质写入。
    HostVolumeTransferFunction volumeTransferFunction;
    double isoThreshold = 0.0; // hasIso=true 时使用的数据标量阈值。
    HostBackgroundColor background;
    HostWindowLevelParams windowLevel;
    bool hasVolumeTransferFunction = false;
    bool hasIso = false;
    bool hasBackground = false;
    bool hasWindowLevel = false;
};

struct HostWindowConfig {
    std::string title; // context 会尝试写入窗口；外部注入窗口仍可由宿主继续管理标题。
    int width = 600;
    int height = 600;
    int posX = 0;
    int posY = 0;
    bool isAxesVisible = false; // 控制方向轴 overlay 的初始可见性。
    HostViewInitConfig viewInit;
};

enum class HostInputMode : std::uint8_t {
    NativeInteractor,
    HostInjected
};

struct HostRenderViewConfig {
    std::string id; // 会话内唯一稳定标识；HostViewTarget 优先按此值查找。
    HostRenderViewRole role = HostRenderViewRole::Auxiliary; // 允许同 role 多窗口，集合查询按拓扑顺序返回。
    HostWindowConfig window; // 窗口尺寸、位置与初始渲染状态。
    vtkSmartPointer<vtkRenderWindow> renderWindow; // 可选外部窗口；为空时 session 自建并拥有窗口。
    bool isEventLoopEnabled = false; // standalone Start 候选；一个会话必须能解析出唯一启动窗口。
    HostInputMode inputMode = HostInputMode::NativeInteractor; // 构建时冻结；默认由 VTK interactor 接收原生输入。
};

// 上位机读取单视图当前状态的值快照；所有容器均为独立副本，不暴露 VizService/SharedState。
struct HostRenderViewState final {
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    HostRenderMode viewMode = HostRenderMode::IsoSurface;
    HostMaterialParams material;
    HostVolumeTransferFunction volumeTransferFunction;
    double isoThreshold = 0.0;
    HostBackgroundColor background;
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
    HostWindowLevelParams windowLevel;
    std::array<double, 2> scalarRange{ 0.0, 0.0 };
    HostVolumeQuality volumeQuality = HostVolumeQuality::Low;
    bool isFeatureActive = false;
    bool isInteracting = false;
    std::array<double, 3> cursorWorld{ 0.0, 0.0, 0.0 };
    uint32_t visibilityMask = 0;
    bool isAxesVisible = false;
    // View 当前渲染输入的确定修订与 primary Binding 时钟。
    DataRevisionRef dataRevision;
    DataBindingRevision bindingRevision = 0;
};

// Camera 的只读值快照；不携带 vtkCamera identity 或可写能力。
struct HostCameraState final {
    std::array<double, 3> position{ 0.0, 0.0, 1.0 };
    std::array<double, 3> focalPoint{ 0.0, 0.0, 0.0 };
    std::array<double, 3> viewUp{ 0.0, 1.0, 0.0 };
    std::array<double, 2> clippingRange{ 0.1, 1000.0 };
    double parallelScale = 1.0;
    double viewAngle = 30.0;
    bool isParallel = false;
};

// Session 中单个 View 的只读场景投影；缺失项由 optional 明确表达。
struct HostSceneViewState final {
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    bool isAvailable = false;
    std::optional<HostRenderViewState> presentation;
    std::optional<HostCameraState> camera;
    // 仅表示 App presentation 事务，不覆盖 Camera、Feature 或 Overlay。
    std::uint64_t presentationRevision = 0;
    std::vector<std::string> activeFeatureIds;
    // sceneEpoch 是该值快照的 Session 逻辑提交；renderedEpoch 表示该 View
    // 已完成 Render() 的最近提交。后者较小时，状态已提交但帧仍待重试。
    std::uint64_t sceneEpoch = 0;
    std::uint64_t renderedEpoch = 0;
};

struct HostRenderViewEndpoint {
    // endpoint 是对 session 内 VTK 对象的非拥有观察视图，不得跨 session 重建或析构缓存。
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    vtkRenderer* renderer = nullptr;
    vtkRenderWindow* renderWindow = nullptr;
    vtkRenderWindowInteractor* interactor = nullptr;
};

enum class HostStopState : std::uint8_t {
    Stopped,
    Building,
    Running,
    StopRequested,
    Stopping,
    StopPending
};

struct HostSessionConfig {
    std::vector<HostRenderViewConfig> renderViews; // 声明顺序即 topology 顺序，也决定多目标返回与首选窗口顺序。
    // 可选的 owner-thread 投递器。Qt 宿主可映射到自己的事件循环；
    // session 在错误线程最终释放时只投递 Stop，不直接访问 VTK。
    std::function<bool(std::function<void()>)> sendOwnerTask;
    // 立即消费的生命周期诊断；回调不得抛异常或缓存 message 引用。
    std::function<void(const std::string& message)> sendDiagnostic;
};
