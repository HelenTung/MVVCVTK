#pragma once

#include "App/Services/AppPorts.h"
#include "App/Services/FeatureViewService.h"
#include "Data/DataService.h"
#include "Interaction/InteractionPorts.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Contracts/RenderBindPort.h"
#include "Render/Contracts/RenderStrategyFactory.h"
#include "Render/Internal/RenderResourceCoordinator.h"
#include "Platform/TaskStopToken.h"

#include <functional>
#include <future>
#include <memory>
#include <thread>

class IStateEventSource;
class SharedInteractionState;
class HistogramConverter;

using AppTaskWork = std::packaged_task<bool(TaskStopToken)>;
using AppWorkerWork = std::function<void()>;
using AppWorkerStart = std::function<std::thread(AppWorkerWork)>;

class AppTaskExecutor;

// Host 为整组 View 注入同一个 executor；独立 App 可省略并由工厂创建私有实例。
std::shared_ptr<AppTaskExecutor> CreateAppTaskExecutor(
    AppWorkerStart workerStart = {});
// Image read 复用固定 export lane，但不暴露 executor 实现或任务队列。
TaskAdmissionResult SendReadTask(
    const std::shared_ptr<AppTaskExecutor>& executor,
    AppTaskWork work);
// Render 产品复用独立的单 worker lane；bool 只表示 lane 是否接纳。
bool SendRenderTask(
    const std::shared_ptr<AppTaskExecutor>& executor,
    RenderLaneWork work);

struct AppServiceArgs final {
    std::shared_ptr<AbstractDataManager> dataManager;
    std::shared_ptr<SharedInteractionState> interactionState;
    std::shared_ptr<IStateEventSource> eventSource;
    // 同一 Host Session 的 View 注入同一实例；converter 内部互斥复用 histogram cache。
    std::shared_ptr<HistogramConverter> histogram;
    // 只在构造固定 worker 时调用；生产默认创建 std::thread，测试可注入启动失败。
    AppWorkerStart workerStart;
    std::shared_ptr<AppTaskExecutor> taskExecutor;
    std::shared_ptr<RenderStrategyServices> renderServices;
    StrategyCreate strategyCreate;
    // Host 可选注入跨视图统一提交；未注入时保留单 AppRuntime 的独立使用路径。
    std::function<LoadCommitResult(
        LoadEventKind,
        std::uint64_t,
        const TrustedImageSnapshot&)> setLoadCommit;
    std::function<LoadCommitResult(
        std::uint64_t,
        LoadCommitFailure)> setLoadCancelled;
};

// 该结果只在 Host 组合入口解包；每个字段均为不同 adapter identity。
struct AppFactoryResult final {
    AppPorts app;
    InteractionPorts interaction;
    std::shared_ptr<AppDataStagePort> dataStage;
    std::shared_ptr<RenderBindPort> renderBind;
    std::shared_ptr<FeatureViewService> featureView;
    std::shared_ptr<OverlayService> overlay;
    std::shared_ptr<AppTaskControlPort> taskControl;
};

AppFactoryResult CreateAppPorts(AppServiceArgs args);
