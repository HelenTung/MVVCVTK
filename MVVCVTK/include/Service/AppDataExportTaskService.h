#pragma once

#include "AppInterfaces.h"
#include "AppState.h"
#include "AppTaskCallbackState.h"
#include <future>
#include <memory>
#include <optional>
#include <string>

class AppDataExportTaskService
    : public std::enable_shared_from_this<AppDataExportTaskService>
{
public:
    AppDataExportTaskService(std::shared_ptr<AbstractDataManager> dataManager,
        std::shared_ptr<SharedInteractionState> sharedState);

    std::optional<std::packaged_task<void()>> BuildSaveTransformedDataTask(
        const std::string& path,
        std::function<void(bool success)> onComplete);

    std::optional<std::packaged_task<void()>> BuildSaveSliceImagesTask(
        const std::string& path,
        double angle,
        VizMode currentMode,
        std::function<void(bool success)> onComplete);

    bool ConsumeSaveCallback();
    void ExecutePendingSaveCallback();

private:
    void SetSaveCallback(std::function<void(bool)> callback);
    void SetSaveCallbackReady(bool success, std::function<void(bool)> callback = nullptr);

    std::shared_ptr<AbstractDataManager> m_dataManager;
    std::shared_ptr<SharedInteractionState> m_sharedState;
    AppTaskCallbackState m_saveCompletionCallbackState; // 保存完成回调状态
};
