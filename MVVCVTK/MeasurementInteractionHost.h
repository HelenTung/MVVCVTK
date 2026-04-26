#pragma once
#include "AppInterfaces.h"
#include "IMeasurementStrategy.h"
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <memory>
#include <unordered_map>

class MeasurementInteractionHost : public std::enable_shared_from_this<MeasurementInteractionHost> {
public:
    explicit MeasurementInteractionHost(std::shared_ptr<IMeasurementService> service);

    void SetContext(vtkRenderer* renderer,
        vtkPropPicker* picker,
        AbstractInteractiveService* interactiveService);
    bool GetClickHandled(int x, int y);
    bool GetMouseMoveHandled(int x, int y);
    void SetStateSynced();

private:
    std::shared_ptr<IMeasurementStrategy> GetStrategyCreated(MeasurementType type, uint64_t id) const;
    void SetOverlaysSynced(const std::vector<MeasurementSessionState>& sessions);
    void SetRenderRequested(bool immediate = false) const;
    bool GetPickedPositions(int x, int y, double worldPos[3], double modelPos[3]) const;

    std::shared_ptr<IMeasurementService> m_measurementService;      // 当前窗口绑定的共享测量业务服务接口
    vtkRenderer* m_renderer = nullptr;                               // 当前 VTK 宿主窗口的渲染器
    vtkPropPicker* m_picker = nullptr;                               // 当前 VTK 宿主窗口的拾取器
    AbstractInteractiveService* m_interactiveService = nullptr;      // 当前 VTK 宿主窗口的交互服务，用于 overlay 挂载与脏标记同步
    std::unordered_map<uint64_t, std::shared_ptr<IMeasurementStrategy>> m_strategyById; // 当前宿主窗口内按会话 id 维护的本地测量 overlay 映射
    int m_lastPreviewX = -1;                                          // 当前宿主窗口上一次处理过的预览屏幕 x，避免重复拾取
    int m_lastPreviewY = -1;                                          // 当前宿主窗口上一次处理过的预览屏幕 y，避免重复拾取
};
