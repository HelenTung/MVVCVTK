#include "Host/LoadCommitCoordinator.h"

#include "App/Services/AppPorts.h"
#include "Data/DataManager.h"

#include <algorithm>
#include <vtkImageData.h>
#include <iostream>
#include <utility>

struct LoadCommitCoordinator::Transaction final {
    LoadCommitRequest request;
};

namespace {

LoadCommitResult GetResult(
    const LoadCommitRequest& request,
    const LoadCommitStatus status,
    const LoadCommitFailure failure)
{
    return LoadCommitResult{
        status,
        failure,
        request.transactionRevision,
        request.sourceRevision
    };
}

bool GetSameStages(
    const std::vector<std::shared_ptr<AppDataStagePort>>& left,
    const std::vector<std::shared_ptr<AppDataStagePort>>& right)
{
    return left.size() == right.size()
        && std::equal(
            left.begin(), left.end(), right.begin(),
            [](const auto& first, const auto& second) {
                return first == second;
            });
}

bool ClearStages(
    const LoadCommitRequest& request,
    const bool resetCommitted,
    const std::size_t committedCount)
{
    bool isClosed = true;
    if (resetCommitted) {
        for (std::size_t index = committedCount; index > 0; --index) {
            isClosed = request.stages[index - 1]->ResetViewStage(
                request.transactionRevision) && isClosed;
        }
    }
    for (auto stage = request.stages.rbegin();
        stage != request.stages.rend(); ++stage) {
        isClosed = (*stage)->ClearDataStage(
            request.transactionRevision) && isClosed;
    }
    if (!isClosed) {
        std::cerr << "[Host] Data stage rollback did not fully close.\n";
        if (request.stopViews) (void)request.stopViews();
    }
    return isClosed;
}

} // namespace

LoadCommitCoordinator::LoadCommitCoordinator(
    std::shared_ptr<AbstractDataManager> dataManager)
    : m_dataManager(std::move(dataManager))
{
}

LoadCommitCoordinator::~LoadCommitCoordinator() = default;

LoadCommitResult LoadCommitCoordinator::SetLoadCommit(
    const LoadCommitRequest& request)
{
    const bool hasValidRequest =
        (request.loadKind == LoadEventKind::File
            || request.loadKind == LoadEventKind::Reload)
        && request.transactionRevision != 0
        && GetDataRevisionRefValid(request.sourceRevision)
        && request.pending
        && request.pending->image
        && request.pending->data
        && request.pending->data->self == request.sourceRevision
        && m_dataManager
        && !request.stages.empty()
        && std::all_of(
            request.stages.begin(), request.stages.end(),
            [](const auto& stage) { return stage != nullptr; });
    if (!hasValidRequest) {
        return GetResult(
            request,
            LoadCommitStatus::Failed,
            LoadCommitFailure::InvalidRequest);
    }

    const auto currentStage = m_dataManager->GetLoadStage();
    if (!currentStage || currentStage->image != request.pending) {
        return GetResult(
            request,
            LoadCommitStatus::Failed,
            LoadCommitFailure::StaleInput);
    }

    if (!m_transaction) {
        auto transaction = std::make_unique<Transaction>();
        transaction->request = request;
        std::size_t startedCount = 0;
        for (const auto& stage : transaction->request.stages) {
            const auto status = stage->StartDataStage(
                transaction->request.pending,
                transaction->request.transactionRevision);
            if (status == DataStageStatus::Failed
                || status == DataStageStatus::Cancelled
                || status == DataStageStatus::Idle) {
                for (std::size_t index = startedCount;
                    index > 0; --index) {
                    (void)transaction->request.stages[index - 1]
                        ->ClearDataStage(
                            transaction->request.transactionRevision);
                }
                return GetResult(
                    request,
                    status == DataStageStatus::Cancelled
                        ? LoadCommitStatus::Cancelled
                        : LoadCommitStatus::Failed,
                    status == DataStageStatus::Cancelled
                        ? LoadCommitFailure::Cancelled
                        : LoadCommitFailure::StageFailed);
            }
            ++startedCount;
        }
        m_transaction = std::move(transaction);
        return GetResult(
            request,
            LoadCommitStatus::Preparing,
            LoadCommitFailure::None);
    }

    auto& active = m_transaction->request;
    if (active.transactionRevision != request.transactionRevision) {
        const auto stale = active;
        (void)ClearStages(stale, false, 0);
        m_transaction.reset();
        return SetLoadCommit(request);
    }
    if (active.pending != request.pending
        || active.sourceRevision != request.sourceRevision
        || !GetSameStages(active.stages, request.stages)) {
        const auto stale = active;
        (void)ClearStages(stale, false, 0);
        m_transaction.reset();
        return GetResult(
            request,
            LoadCommitStatus::Cancelled,
            LoadCommitFailure::StaleInput);
    }

    bool areReady = true;
    for (const auto& stage : active.stages) {
        const auto status = stage->SetDataStageReady(
            active.pending, active.transactionRevision);
        if (status == DataStageStatus::Failed
            || status == DataStageStatus::Cancelled) {
            const auto terminal = active;
            (void)ClearStages(terminal, false, 0);
            m_transaction.reset();
            return GetResult(
                terminal,
                status == DataStageStatus::Cancelled
                    ? LoadCommitStatus::Cancelled
                    : LoadCommitStatus::Failed,
                status == DataStageStatus::Cancelled
                    ? LoadCommitFailure::Cancelled
                    : LoadCommitFailure::StageFailed);
        }
        areReady = areReady && status == DataStageStatus::Ready;
    }
    if (!areReady) {
        return GetResult(
            active,
            LoadCommitStatus::Preparing,
            LoadCommitFailure::None);
    }

    std::size_t committedCount = 0;
    for (const auto& stage : active.stages) {
        if (!stage->SetViewStage(
                active.pending, active.transactionRevision)) {
            break;
        }
        ++committedCount;
    }
    if (committedCount != active.stages.size()) {
        const auto terminal = active;
        (void)ClearStages(terminal, true, committedCount);
        m_transaction.reset();
        return GetResult(
            terminal,
            LoadCommitStatus::Failed,
            LoadCommitFailure::CommitFailed);
    }

    VtkImageGridSnapshot published;
    if (!m_dataManager->SetLoadCommit(currentStage, published)
        || !published || !published->data
        || published->data->self != active.sourceRevision) {
        const auto terminal = active;
        (void)ClearStages(terminal, true, committedCount);
        m_transaction.reset();
        return GetResult(
            terminal,
            LoadCommitStatus::Failed,
            LoadCommitFailure::PublishFailed);
    }

    const auto terminal = active;
    for (const auto& stage : terminal.stages) {
        stage->SetDataStageComplete(terminal.transactionRevision);
    }
    m_transaction.reset();
    return GetResult(
        terminal,
        LoadCommitStatus::Succeeded,
        LoadCommitFailure::None);
}

LoadCommitResult LoadCommitCoordinator::SetLoadCancelled(
    const std::uint64_t transactionRevision,
    const LoadCommitFailure failureReason)
{
    LoadCommitResult result;
    result.status = LoadCommitStatus::Cancelled;
    result.failureReason = failureReason;
    result.transactionRevision = transactionRevision;
    if (!m_transaction
        || m_transaction->request.transactionRevision
            != transactionRevision) {
        return result;
    }
    const auto terminal = m_transaction->request;
    result.sourceRevision = terminal.sourceRevision;
    (void)ClearStages(terminal, false, 0);
    m_transaction.reset();
    return result;
}
