#pragma once

#include "App/AppTypes.h"
#include "App/Services/DataCommitTypes.h"
#include "Host/TrustedDataPort.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class AbstractDataManager;
class AppDataStagePort;

struct LoadCommitRequest final {
    LoadEventKind loadKind = LoadEventKind::None;
    std::uint64_t transactionRevision = 0;
    DataRevisionRef sourceRevision;
    VtkImageGridSnapshot pending;
    std::vector<std::shared_ptr<AppDataStagePort>> stages;
    std::function<bool()> stopViews;
};

// 多 View 数据提交事务：所有 View 先建立候选，再统一切换，最后发布 DataManager current。
class LoadCommitCoordinator final {
public:
    explicit LoadCommitCoordinator(
        std::shared_ptr<AbstractDataManager> dataManager);
    ~LoadCommitCoordinator();

    LoadCommitResult SetLoadCommit(const LoadCommitRequest& request);
    LoadCommitResult SetLoadCancelled(
        std::uint64_t transactionRevision,
        LoadCommitFailure failureReason);

private:
    struct Transaction;
    std::shared_ptr<AbstractDataManager> m_dataManager;
    std::unique_ptr<Transaction> m_transaction;
};
