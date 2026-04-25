#pragma once
#include "AngleMeasurementStrategy.h"
#include "AppInterfaces.h"
#include "LengthMeasurementStrategy.h"
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <functional>
#include <memory>
#include <vector>

class MeasurementService : public IMeasurementService {
public:
    MeasurementService() = default;

    void SetToolMode(ToolMode mode) override;
    void SetResultCallback(std::function<void(const MeasurementResult&)> callback) override;
    std::vector<MeasurementResult> GetResults() const override;
    bool SetResultVisible(uint64_t id, bool show) override;
    bool SetResultsFileSaved(const std::string& path = {}) const override;
    void SetResultsCleared() override;

    void SetRenderContext(vtkRenderer* renderer, vtkPropPicker* picker);
    void SetInteractiveService(AbstractInteractiveService* service);
    bool GetClickHandled(int x, int y);
    bool GetMouseMoveHandled(int x, int y);

private:
    std::shared_ptr<IMeasurementStrategy> GetStrategyCreated();
    void SetPendingStrategyCleared();
    void SetHistoryStyleUpdated(MeasurementType type);
    void SetRenderRequested(bool immediate = false) const;

    vtkRenderer* m_renderer = nullptr;                        // 当前测量服务绑定的渲染器，用于命中检测与即时刷新
    vtkPropPicker* m_picker = nullptr;                       // 当前测量服务使用的 VTK 拾取器，只允许有效命中进入测量
    AbstractInteractiveService* m_interactiveService = nullptr; // 当前测量服务回调的主交互服务，用于 overlay 生命周期与脏标记同步
    ToolMode m_toolMode = ToolMode::Navigation;
    uint64_t m_nextId = 1;                                   // 当前测量结果自增 id，保证历史记录可独立显隐
    std::function<void(const MeasurementResult&)> m_resultCallback; // 当前测量完成后抛给业务层的统一回调
    std::shared_ptr<IMeasurementStrategy> m_activeStrategy; // 当前尚未完成且需要鼠标预览的测量实例
    std::vector<std::shared_ptr<IMeasurementStrategy>> m_finishedStrategies; // 已完成并通过 overlay pipeline 持续显示的测量实例
    int m_lastPreviewX = -1;                                 // 当前预览阶段上一次处理过的屏幕 x，避免重复鼠标位置重复计算
    int m_lastPreviewY = -1;                                 // 当前预览阶段上一次处理过的屏幕 y，避免重复鼠标位置重复计算
};
