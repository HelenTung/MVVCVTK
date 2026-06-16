#pragma once

// =====================================================================
// OrthogonalCropSubmitWorkflow - 正交裁切提交流程协调器
// 说明: 负责把裁切 submit payload 提交到主数据 reload 通道，并在 reload
//       主线程重建与同步完成后通知 bridge 收尾。
// =====================================================================

#include "OrthogonalCropInteractionBridgeService.h"
#include "AppInterfaces.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

class OrthogonalCropSubmitWorkflow
    : public std::enable_shared_from_this<OrthogonalCropSubmitWorkflow> {
public:
    using ReloadSubmitter = std::function<bool(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool success)> onComplete)>;

    OrthogonalCropSubmitWorkflow(
        std::shared_ptr<OrthogonalCropInteractionBridgeService> bridge,
        ReloadSubmitter reloadSubmitter,
        std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<IVisualConfigService> visualConfigService);

    void ApplySubmit();

private:
    void HandleReloadComplete(bool success);

    std::shared_ptr<OrthogonalCropInteractionBridgeService> m_bridge;
    ReloadSubmitter m_reloadSubmitter;
    std::shared_ptr<AbstractDataManager> m_dataMgr;
    std::shared_ptr<IVisualConfigService> m_visualConfigService;
    std::shared_ptr<std::vector<float>> m_reloadBuffer;
};
