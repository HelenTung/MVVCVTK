#pragma once

#include "AppInterfaces.h"
#include "AppState.h"
#include "AppTaskCallbackState.h"
#include <array>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

class AppDataLoadTaskService
{
public:
    AppDataLoadTaskService(std::shared_ptr<AbstractDataManager> dataManager,
        std::shared_ptr<SharedInteractionState> sharedState);

    std::optional<std::packaged_task<void()>> BuildLoadFileTask(
        const std::string& path,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool success)> onComplete);

    std::optional<std::packaged_task<void()>> BuildReloadFromBufferTask(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool success)> onComplete);

    void SetFileLoadCallbackReady(bool success);
    void SetReloadLoadCallbackReady(bool success);
    bool ConsumeFileLoadCallback();
    bool ConsumeReloadLoadCallback();
    void ExecutePendingFileLoadCallback();
    void ExecutePendingReloadLoadCallback();

private:
    bool GetAnyLoadRunning() const;

    std::shared_ptr<AbstractDataManager> m_dataManager;
    std::shared_ptr<SharedInteractionState> m_sharedState;
    AppTaskCallbackState m_fileLoadCallbackState; // 文件流加载回调状态
    AppTaskCallbackState m_reloadLoadCallbackState; // 重载回调状态
};
