#pragma once

#include "AppStateEvents.h"
#include "AppTypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

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
    bool SetFileLoadFailed();
    bool SetReloadLoadFailed();
    void SetPreInitConfig(const PreInitConfig& config);
    void SetModelMatrix(const std::array<double, 16>& modelToWorldMatrix);
    std::array<double, 16> GetModelMatrix() const;
    void SetScalarRange(double rangeMin, double rangeMax);
    std::array<double, 2> GetDataRange() const;
    void SetTFNodes(const std::vector<TFNode>& nodes);
    void GetTFNodes(std::vector<TFNode>& destination) const;
    void SetIsoValue(double value);
    double GetIsoValue() const;
    void SetMaterial(const MaterialParams& material);
    MaterialParams GetMaterial() const;
    void SetBackground(const BackgroundColor& background);
    BackgroundColor GetBackground() const;
    void SetSpacing(double spacingX, double spacingY, double spacingZ);
    std::array<double, 3> GetSpacing() const;
    void SetWindowLevel(double windowWidth, double windowCenter);
    WindowLevelParams GetWindowLevel() const;
    void SetInteracting(bool isInteracting);
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
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
