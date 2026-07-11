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
    void SetFileLoadState(LoadState state);
    LoadState GetFileLoadState() const;
    void SetReloadLoadState(LoadState state);
    LoadState GetReloadLoadState() const;
    void SetDataTrustedState(LoadState state);
    LoadState GetDataTrustedState() const;
    LoadEventKind GetPendingLoadEventKind() const;
    void SetFileLoadStarted();
    void SetReloadLoadStarted();
    void SetFileDataReady(
        double rangeMin,
        double rangeMax,
        const std::array<double, 3>& spacing);
    void SetReloadDataReady(
        double rangeMin,
        double rangeMax,
        const std::array<double, 3>& spacing);
    void SetFileLoadFailed();
    void SetReloadLoadFailed();
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
