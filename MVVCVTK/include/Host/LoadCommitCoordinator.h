#pragma once

#include "App/AppTypes.h"

#include <functional>
#include <memory>
#include <vector>

class AbstractDataManager;
class AppDataStagePort;

struct LoadCommitRequest final {
    LoadEventKind loadKind = LoadEventKind::None;
    std::vector<std::shared_ptr<AppDataStagePort>> stages;
    std::function<bool()> stopViews;
};

// 多 View 数据提交事务：所有 View 先建立候选，再统一切换，最后发布 DataManager current。
class LoadCommitCoordinator final {
public:
    explicit LoadCommitCoordinator(
        std::shared_ptr<AbstractDataManager> dataManager);

    bool SetLoadCommit(const LoadCommitRequest& request) const;

private:
    std::shared_ptr<AbstractDataManager> m_dataManager;
};
