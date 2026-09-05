#pragma once

#include "App/AppTypes.h"
#include "App/Services/DataCommitTypes.h"
#include "Host/TrustedDataPort.h"
#include "Data/VolumeTypes.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class TaskAdmissionResult : std::uint8_t {
    Accepted,
    InvalidRequest,
    Busy,
    QueueFull,
    Stopping,
    Unavailable
};

class AppDataPort {
public:
    virtual ~AppDataPort() = default;

    // callback 只属于 Accepted 请求；拒绝原因由返回值同步给出，不排入完成队列。
    virtual TaskAdmissionResult LoadFileAsync(
        std::string path,
        VolumeLayout layout,
        std::function<void(bool)> onComplete) = 0;
    virtual TaskAdmissionResult ReloadFromBufferAsync(
        VolumeBuffer buffer,
        std::function<void(bool)> onComplete) = 0;
    virtual TaskAdmissionResult ExportDataAsync(
        std::string outputDir,
        std::string extension,
        std::function<void(bool)> onComplete) = 0;
    virtual TaskAdmissionResult ExportSlicesAsync(
        std::string path,
        std::optional<double> rotationAngleDeg,
        std::function<void(bool)> onComplete) = 0;
};

// 组合根先广播取消，再用同一个绝对 deadline 等待所有固定 worker 退出。
class AppTaskControlPort {
public:
    virtual ~AppTaskControlPort() = default;

    virtual bool SetTaskStopping() = 0;
    virtual bool StopTasks(
        std::chrono::steady_clock::time_point deadline) = 0;
};

struct AppVisibilityUpdate final {
    std::optional<bool> isPlanes3DVisible;
    std::optional<bool> isCrosshairVisible;
    std::optional<bool> isRulerVisible;
};

// App 内部使用类型化轴语义；Qt/Host DTO 的既有 int 字段只在 Router 边界校验并转换。
enum class AppCursorAxis : int {
    Free = -1,
    X = 0,
    Y = 1,
    Z = 2
};

struct AppViewUpdate final {
    // 全部字段只修改当前 AppRuntime/Context 对应的渲染实例。
    std::optional<VizMode> mode;
    std::optional<MaterialParams> material;
    std::optional<double> opacity;
    std::optional<VolumeTransferFunction>
        volumeTransferFunction;
    std::optional<double> isoThreshold;
    std::optional<BackgroundColor> background;
    std::optional<WindowLevelParams> windowLevel;
    std::optional<WindowLevelMode> windowLevelMode;
    std::optional<VolumeQuality> volumeQuality;
    std::optional<std::vector<GradientOpacityNode>> gradientOpacity;
    std::optional<bool> isDenoiseOn;
    std::optional<AppVisibilityUpdate> visibility;
};

struct AppViewState final {
    VizMode mode = VizMode::Volume;
    MaterialParams material;
    VolumeTransferFunction volumeTransferFunction;
    bool isTransferAuto = true;
    double isoThreshold = 0.0;
    BackgroundColor background;
    std::array<double, 3> spacing{};
    WindowLevelParams windowLevel;
    WindowLevelMode windowLevelMode = WindowLevelMode::Auto;
    std::array<double, 2> scalarRange{};
    VolumeQuality volumeQuality = VolumeQuality::Low;
    std::vector<GradientOpacityNode> gradientOpacity;
    bool isFeatureActive = false;
    bool isDenoiseOn = false;
    bool isInteracting = false;
    std::array<double, 3> cursorWorld{};
    AppCursorAxis cursorAxis = AppCursorAxis::Free;
    std::uint32_t visibilityMask = 0;
    // 当前 View 已提交 Strategy 使用的数据版本；允许 Host 诊断发布顺序，不暴露 Strategy 对象。
    DataRevisionRef dataRevision;
    DataBindingRevision bindingRevision = 0;
    // 每次成功提交完整 View 事务后单调递增，用于拒绝陈旧补偿。
    std::uint64_t revision = 0;
};

// Session 级命令只承载数据坐标与跨视图联动状态，不包含任一 View 的展示参数。
struct AppSessionUpdate final {
    std::optional<std::array<double, 3>> spacing;
    std::optional<std::array<double, 3>> cursorWorld;
    AppCursorAxis cursorAxis = AppCursorAxis::Free;
};

class AppSessionPort {
public:
    virtual ~AppSessionPort() = default;
    virtual bool SendSessionUpdate(
        const AppSessionUpdate& update) = 0;
};

class AppViewPort {
public:
    virtual ~AppViewPort() = default;

    virtual bool SetViewConfig(const PreInitConfig& config) = 0;
    virtual bool SendViewUpdate(const AppViewUpdate& update) = 0;
    // 仅在 expectedRevision 仍为当前版本时恢复完整值快照。
    virtual bool SetViewState(
        const AppViewState& state,
        std::uint64_t expectedRevision) = 0;
    virtual AppViewState GetViewState() const = 0;
};

class AppFeaturePort {
public:
    virtual ~AppFeaturePort() = default;

    virtual bool SetFeatureActive(
        const FeatureSource& source,
        bool isActive) = 0;
};

// Host 组合根只编排候选数据事务，不取得 AppRuntime 或 Strategy identity。
class AppDataStagePort {
public:
    virtual ~AppDataStagePort() = default;

    virtual DataStageStatus StartDataStage(
        const VtkImageGridSnapshot& snapshot,
        std::uint64_t transactionRevision) = 0;
    virtual DataStageStatus SetDataStageReady(
        const VtkImageGridSnapshot& snapshot,
        std::uint64_t transactionRevision) = 0;
    virtual DataStageStatus GetDataStageStatus(
        std::uint64_t transactionRevision) const = 0;
    virtual bool SetViewStage(
        const VtkImageGridSnapshot& snapshot,
        std::uint64_t transactionRevision) = 0;
    virtual bool ResetViewStage(
        std::uint64_t transactionRevision) = 0;
    virtual bool ClearDataStage(
        std::uint64_t transactionRevision) = 0;
    virtual void SetDataStageComplete(
        std::uint64_t transactionRevision) noexcept = 0;
};

struct AppPorts final {
    std::shared_ptr<AppDataPort> data;
    std::shared_ptr<AppViewPort> view;
    std::shared_ptr<AppSessionPort> session;
    std::shared_ptr<AppFeaturePort> feature;
};
