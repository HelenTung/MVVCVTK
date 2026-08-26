#pragma once

#include "AppStateEvents.h"
#include "AppTypes.h"
#include "App/Services/DataCommitTypes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// 单个 AppRuntime 独占的展示状态；不同 View 不共享材质、TF、背景、窗宽窗位或显隐。
// owner-thread 更新通过 shadow/commit 关闭事务，中途失败不会暴露半状态。
class ViewPresentationState {
public:
    explicit ViewPresentationState(
        std::shared_ptr<IStateEventSink> eventSink = nullptr);
    ~ViewPresentationState();

    ViewPresentationState(const ViewPresentationState&) = delete;
    ViewPresentationState& operator=(const ViewPresentationState&) = delete;
    ViewPresentationState(ViewPresentationState&&) = delete;
    ViewPresentationState& operator=(ViewPresentationState&&) = delete;

    bool StartUpdate();
    bool SetUpdateCommit(
        bool isCommitted,
        UpdateFlags& pendingFlags);
    void SendUpdateFlags(UpdateFlags flags) noexcept;
    void SetPreInitConfig(const PreInitConfig& config);
    void SetTFNodes(const std::vector<TFNode>& nodes);
    void GetTFNodes(std::vector<TFNode>& destination) const;
    void SetTransferPresetIntent(TransferPreset preset);
    bool SetTransferPresetNodes(
        TransferPreset preset,
        DataVersion dataVersion,
        const std::vector<TFNode>& nodes);
    TransferPreset GetTransferPreset() const;
    void SetIsoValue(double value);
    double GetIsoValue() const;
    void SetMaterial(const MaterialParams& material);
    MaterialParams GetMaterial() const;
    void SetBackground(const BackgroundColor& background);
    BackgroundColor GetBackground() const;
    void SetWindowLevel(double windowWidth, double windowCenter);
    bool SetAutoWindowLevel(const WindowLevelParams& windowLevel);
    bool ResetWindowLevel(const WindowLevelParams& windowLevel);
    WindowLevelParams GetWindowLevel() const;
    WindowLevelMode GetWindowLevelMode() const;
    void SetElementVisible(uint32_t flagBit, bool isVisible);
    uint32_t GetVisibilityMask() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// 跨视图共享的交互状态门面；实现细节收口在 cpp，避免状态锁与比较策略扩散到调用方。
class SharedInteractionState {
public:
    explicit SharedInteractionState(
        std::shared_ptr<IStateEventSink> eventSink = nullptr);
    ~SharedInteractionState();

    SharedInteractionState(const SharedInteractionState&) = delete;
    SharedInteractionState& operator=(const SharedInteractionState&) = delete;
    SharedInteractionState(SharedInteractionState&&) = delete;
    SharedInteractionState& operator=(SharedInteractionState&&) = delete;

    void SetEventSink(std::shared_ptr<IStateEventSink> eventSink);
    // owner-thread View 补偿事务的事件屏障：事务期间暂缓所有通知，
    // 成功时合并 owner/external flags，失败时仅保留并发线程产生的 flags。
    bool StartViewUpdate();
    bool SetViewUpdateCommit(bool isCommitted);
    // 新事务路径先关闭屏障并取走 flags，再由调用方释放自身锁后广播。
    bool SetViewUpdateCommit(
        bool isCommitted,
        UpdateFlags& pendingFlags);
    void SendViewUpdateFlags(UpdateFlags flags) noexcept;
    LoadState GetFileLoadState() const;
    LoadState GetReloadLoadState() const;
    LoadState GetDataTrustedState() const;
    // 在同一锁区内检查并发布唯一 load 事务，避免 File/Reload 并发穿透状态检查。
    bool StartLoad(LoadEventKind loadEventKind);
    // 终态消费、任务构造失败或服务销毁兜底时释放对应事务；释放前不会开放下一次接纳。
    bool ResetLoad(LoadEventKind loadEventKind);
    bool SetFileDataReady(
        double rangeMin,
        double rangeMax,
        const std::array<double, 3>& spacing);
    bool SetReloadDataReady(
        double rangeMin,
        double rangeMax,
        const std::array<double, 3>& spacing);
    // 非 load 的数据版本替换：只广播结构刷新，不占用 File/Reload admission。
    bool SetImageDataReady(
        double rangeMin,
        double rangeMax,
        const std::array<double, 3>& spacing);
    // DataManager current 已最终发布后的无失败入口；一次锁内同步提交
    // version/range/spacing/cursor/load state，锁外仅广播一个聚合事件。
    void SetDataReady(const DataReadyState& state) noexcept;
    DataVersion GetDataVersion() const;
    bool SetFileLoadFailed();
    bool SetReloadLoadFailed();
    void SetPreInitConfig(const PreInitConfig& config);
    void SetModelMatrix(const std::array<double, 16>& modelToWorldMatrix);
    std::array<double, 16> GetModelMatrix() const;
    void SetScalarRange(double rangeMin, double rangeMax);
    std::array<double, 2> GetScalarRange() const;
    // 兼容既有调用方；新代码应使用与 SetScalarRange 对称的 GetScalarRange。
    std::array<double, 2> GetDataRange() const;
    void SetTFNodes(const std::vector<TFNode>& nodes);
    void GetTFNodes(std::vector<TFNode>& destination) const;
    void SetTransferPresetIntent(TransferPreset preset);
    bool SetTransferPresetNodes(
        TransferPreset preset,
        DataVersion dataVersion,
        const std::vector<TFNode>& nodes);
    TransferPreset GetTransferPreset() const;
    void SetIsoValue(double value);
    double GetIsoValue() const;
    void SetMaterial(const MaterialParams& material);
    MaterialParams GetMaterial() const;
    void SetBackground(const BackgroundColor& background);
    BackgroundColor GetBackground() const;
    void SetSpacing(double spacingX, double spacingY, double spacingZ);
    // spacing 同时属于 DataManager 物理元信息与 Session 共享状态；
    // 数据写入在状态锁内完成，避免 View 补偿覆盖并发 Session 写入。
    bool SetSpacingData(
        const std::array<double, 3>& spacing,
        const std::function<bool(
            const std::array<double, 3>&)>& setData);
    std::array<double, 3> GetSpacing() const;
    void SetWindowLevel(double windowWidth, double windowCenter);
    WindowLevelParams GetWindowLevel() const;
    bool SetInteracting(
        const InteractionSource& source,
        bool isInteracting);
    bool GetIsInteracting() const;
    void SetCursorWorld(double worldX, double worldY, double worldZ);
    void SetCursorRawWorld(double worldX, double worldY, double worldZ);
    std::array<double, 3> GetCursorRawWorld() const;
    void SetCursorAxis(int axis);
    int GetCursorAxis() const;
    std::array<double, 3> GetCursorWorld() const;
    void SetElementVisible(uint32_t flagBit, bool isVisible);
    uint32_t GetVisibilityMask() const;

private:
    friend class ViewPresentationState;

    bool SetAutoWindowLevel(const WindowLevelParams& windowLevel);
    bool ResetWindowLevel(const WindowLevelParams& windowLevel);
    WindowLevelMode GetWindowLevelMode() const;

    class Impl;
    std::unique_ptr<Impl> m_impl;
};
