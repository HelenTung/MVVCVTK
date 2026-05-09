#pragma once
#include "AppInterfaces.h"
#include "MeasurementComputeService.h"
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

struct MeasurementObserverEntry {
    std::weak_ptr<void> owner;          // 当前测量观察者的生命周期凭证，用于清理已释放宿主
    std::function<void()> callback;     // 当前测量状态变化后回调宿主同步 overlay 的通知函数
};

class MeasurementService : public IMeasurementService {
public:
    MeasurementService() = default;

    void SetToolMode(ToolMode mode) override;
    bool SetMeasurementPointAdded(const double worldPos[3], const double modelPos[3]) override;
    bool SetMeasurementPreviewPointUpdated(const double worldPos[3], const double modelPos[3]) override;
    void SetMeasurementPreviewCleared() override;
    void SetResultCallback(std::function<void(const MeasurementResult&)> callback) override;
    std::vector<MeasurementResult> GetResults() const override;
    std::vector<MeasurementSessionState> GetSessionStates() const override;
    bool SetResultVisible(uint64_t id, bool show) override;
    bool SetResultsFileSaved(const std::string& path = {}) const override;
    void SetResultsCleared() override;
    void SetMeasurementObserver(std::shared_ptr<void> owner,
        std::function<void()> callback) override;

private:
    int GetActiveSessionIndex() const;
    MeasurementSessionState GetSessionCreated();
    void SetHistoryStyleUpdated(MeasurementType type);
    void SetObserversNotified();
    static bool GetIsMeasureMode(ToolMode mode);

    mutable std::mutex m_mutex;
    ToolMode m_toolMode = ToolMode::Navigation;
    uint64_t m_nextId = 1;                                      // 当前测量结果自增 id，保证共享会话池中的历史记录可独立显隐
    std::function<void(const MeasurementResult&)> m_resultCallback; // 当前测量完成后抛给业务层的统一回调
    std::vector<MeasurementSessionState> m_sessions;            // 当前共享测量会话池，包含历史会话与一个未完成会话
    std::vector<MeasurementObserverEntry> m_observers;          // 当前已注册的测量宿主观察者，用于多窗口同步显示
};
