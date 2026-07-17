#pragma once

#include "AppInterfaces.h"
#include "Data/VolumeTypes.h"
#include <future>
#include <memory>
#include <optional>
#include <string>

// 任务只准备 DataManager pending，不发布终态、不执行业务回调。
class AppDataLoadTaskService
{
public:
    explicit AppDataLoadTaskService(
        std::shared_ptr<AbstractDataManager> dataManager);

    std::optional<std::packaged_task<bool()>> BuildLoadFileTask(
        std::string path,
        VolumeLayout layout);

    std::optional<std::packaged_task<bool()>> BuildReloadTask(
        VolumeBuffer buffer);

private:
    // 与构造出的 packaged_task 共享拥有 DataManager；保证后台 I/O 期间数据入口存活。
    std::shared_ptr<AbstractDataManager> m_dataManager;
};
