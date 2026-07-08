#pragma once

#include "AppInterfaces.h"
#include "AppState.h"
#include "AppTaskCallbackState.h"
#include <future>
#include <memory>
#include <optional>
#include <string>

// 导出任务需要在调用线程拍下视觉状态快照，再把重采样 / I/O 放到后台执行。
// 本 service 只负责构造任务和持有保存回调状态，不直接启动线程；
// 线程托管仍由 VizService 统一完成，避免导出、加载各自发明一套生命周期策略。
class AppDataExportTaskService
    : public std::enable_shared_from_this<AppDataExportTaskService>
{
public:
    AppDataExportTaskService(std::shared_ptr<AbstractDataManager> dataManager,
        std::shared_ptr<SharedInteractionState> sharedState);

    std::optional<std::packaged_task<void()>> BuildDataTask(
        const std::string& path,
        std::function<void(bool isSuccess)> onComplete);

    std::optional<std::packaged_task<void()>> BuildSlicesTask(
        const std::string& path,
        std::optional<double> rotationAngleDeg,
        VizMode currentMode,
        std::function<void(bool isSuccess)> onComplete);

    bool ResetSaveCallback();
    void SendSaveCallback();

private:
    void SetSaveCallback(std::function<void(bool)> callback);
    void SetSaveCallbackReady(bool isSuccess, std::function<void(bool)> callback = nullptr);

    std::shared_ptr<AbstractDataManager> m_dataManager;
    std::shared_ptr<SharedInteractionState> m_sharedState;
    AppTaskCallbackState m_saveCallback; // 保存完成后由主线程执行业务回调
};
