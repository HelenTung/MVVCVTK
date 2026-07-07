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

// 加载链路需要先写 LoadState，再构造后台 I/O 任务，最后把回调延迟到主线程策略同步之后执行。
// 这里不直接启动线程，是为了让 VizService 继续统一托管 future 和 detach 策略；
// 这样加载细节可以拆出，同时主线程收口顺序仍只有一个编排入口。
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
    AppTaskCallbackState m_fileLoadCallbackState; // 文件流加载完成后由主线程执行业务回调
    AppTaskCallbackState m_reloadLoadCallbackState; // reload 完成后由主线程执行 submit / UI 回调
};
