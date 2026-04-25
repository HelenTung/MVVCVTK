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

    vtkRenderer* m_renderer = nullptr;
    vtkPropPicker* m_picker = nullptr;
    AbstractInteractiveService* m_interactiveService = nullptr;
    ToolMode m_toolMode = ToolMode::Navigation;
    uint64_t m_nextId = 1;
    std::function<void(const MeasurementResult&)> m_resultCallback;
    std::shared_ptr<IMeasurementStrategy> m_activeStrategy; // 当前尚未完成且需要鼠标预览的测量实例
    std::vector<std::shared_ptr<IMeasurementStrategy>> m_finishedStrategies; // 已完成并通过 overlay pipeline 持续显示的测量实例
    int m_lastPreviewX = -1;
    int m_lastPreviewY = -1;
};
