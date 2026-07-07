#include "Host/HostCoreServices.h"

#include "AppState.h"
#include "DataManager.h"
#include "Interaction/CropBridge.h"
#include "Services/GapAnalysisService.h"
#include "VolumeAnalysisService.h"

HostCoreServices BuildHostCoreServices()
{
    // 1. 先创建数据与状态真源，让所有视图和 feature 共享同一份会话事实。
    // 2. 再创建依赖这些真源的分析/交互服务；它们不持有窗口，因此后续 Qt 改窗口拓扑时不需要重建 core。
    HostCoreServices core;
    core.sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    core.sharedStateBroadcaster = std::make_shared<SharedStateBroadcaster>();
    core.sharedState = std::make_shared<SharedInteractionState>(core.sharedStateBroadcaster);
    core.imageAnalysis = std::make_shared<VolumeAnalysisService>(core.sharedDataMgr);
    core.gapAnalysis = std::make_shared<GapAnalysisService>();
    core.orthogonalCropBridge = std::make_shared<CropBridge>();
    return core;
}
