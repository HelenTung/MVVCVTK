#include "Host/LoadCommitCoordinator.h"

#include "App/Services/AppPorts.h"
#include "Data/DataManager.h"

#include <iostream>
#include <utility>

LoadCommitCoordinator::LoadCommitCoordinator(
    std::shared_ptr<AbstractDataManager> dataManager)
    : m_dataManager(std::move(dataManager))
{
}

bool LoadCommitCoordinator::SetLoadCommit(
    const LoadCommitRequest& request) const
{
    if ((request.loadKind != LoadEventKind::File
            && request.loadKind != LoadEventKind::Reload)
        || !m_dataManager || request.stages.empty()) {
        return false;
    }

    const TrustedImageSnapshot pending = m_dataManager->GetPendingSnapshot();
    if (!pending || !pending->image
        || !m_dataManager->GetImageSnapshot()) {
        return false;
    }

    std::vector<std::shared_ptr<AppDataStagePort>> stages;
    stages.reserve(request.stages.size());
    for (const auto& stage : request.stages) {
        if (!stage || !stage->BuildDataStage(pending)) {
            for (auto current = stages.rbegin();
                current != stages.rend(); ++current) {
                (void)(*current)->ClearDataStage();
            }
            return false;
        }
        stages.push_back(stage);
    }

    std::size_t committedCount = 0;
    for (const auto& stage : stages) {
        if (!stage->SetViewStage(pending)) break;
        ++committedCount;
    }
    if (committedCount != stages.size()) {
        bool isReset = true;
        for (std::size_t index = committedCount; index > 0; --index) {
            isReset = stages[index - 1]->ResetViewStage() && isReset;
        }
        for (auto stage = stages.rbegin();
            stage != stages.rend(); ++stage) {
            isReset = (*stage)->ClearDataStage() && isReset;
        }
        if (!isReset) {
            std::cerr
                << "[Host] Data stage rollback did not fully close.\n";
            if (request.stopViews) (void)request.stopViews();
        }
        return false;
    }

    TrustedImageSnapshot published;
    if (!m_dataManager->SetCurrentFromPending(pending, published)
        || !published) {
        bool isReset = true;
        for (auto stage = stages.rbegin();
            stage != stages.rend(); ++stage) {
            isReset = (*stage)->ResetViewStage() && isReset;
        }
        for (auto stage = stages.rbegin();
            stage != stages.rend(); ++stage) {
            isReset = (*stage)->ClearDataStage() && isReset;
        }
        if (!isReset) {
            std::cerr
                << "[Host] Data publish rollback did not fully close.\n";
            if (request.stopViews) (void)request.stopViews();
        }
        return false;
    }

    // View 只在 owner thread 使用候选；DataManager 最后发布同一个 owner 后，
    // 其他线程才可观察新版本。此后只允许 noexcept 状态接管，禁止再调用
    // 可失败的 rollback 清理，从协议上消除“已发布但回调失败/残留 stage”。
    for (const auto& stage : stages) {
        stage->SetDataStageComplete();
    }
    return true;
}
