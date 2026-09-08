#include "Host/LoadCommitCoordinator.h"

#include "App/Services/AppPorts.h"
#include "Data/DataManager.h"

#include <algorithm>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <iostream>
#include <utility>

struct LoadCommitCoordinator::Transaction final {
    LoadCommitRequest request;
    std::size_t attemptedCommits = 0;
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
    const std::size_t committedCount) noexcept
{
    bool isClosed = true;
    if (resetCommitted) {
        for (std::size_t index = committedCount; index > 0; --index) {
            try {
                isClosed = request.stages[index - 1]->ResetViewStage(request.transactionRevision) && isClosed;
            } catch (...) { isClosed = false; }
        }
    }
    for (auto stage = request.stages.rbegin();
        stage != request.stages.rend(); ++stage) {
        try {
            isClosed = (*stage)->ClearDataStage(request.transactionRevision) && isClosed;
        } catch (...) { isClosed = false; }
    }
    if (!isClosed) {
        std::cerr << "[Host] Data stage rollback did not fully close.\n";
        try { if (request.stopViews) (void)request.stopViews(); } catch (...) {}
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

bool LoadCommitCoordinator::GetIsPending() const noexcept
{
    return m_transaction != nullptr;
}

LoadCommitResult LoadCommitCoordinator::SetLoadCommit(
    const LoadCommitRequest& request)
{
    if (m_isAdvancing) return GetResult(request, LoadCommitStatus::Failed, LoadCommitFailure::InvalidRequest);
    struct AdvanceGuard final {
        bool& value;
        explicit AdvanceGuard(bool& flag) : value(flag) { value = true; }
        ~AdvanceGuard() { value = false; }
    } guard(m_isAdvancing);
    try { return AdvanceLoadCommit(request); }
    catch (...) {
        if (m_transaction) {
            (void)ClearStages(m_transaction->request, true, m_transaction->attemptedCommits);
            m_transaction.reset();
        }
        return GetResult(request, LoadCommitStatus::Failed, LoadCommitFailure::StageFailed);
    }
}

LoadCommitResult LoadCommitCoordinator::AdvanceLoadCommit(const LoadCommitRequest& request)
{
    const auto input=request.renderInput?request.renderInput:VtkRenderInputView::FromImage(request.pending);
    const bool hasValidRequest =
        ((request.ownerId == 0 && !request.onPublish
            && (request.loadKind == LoadEventKind::File || request.loadKind == LoadEventKind::Reload))
            || (request.ownerId != 0 && request.onPublish && request.loadKind == LoadEventKind::None))
        && request.transactionRevision != 0
        && GetDataRevisionRefValid(request.sourceRevision)
        && input && input->GetValid()
        && input->data && input->data->self == request.sourceRevision
        && (!request.renderInput || (request.ownerId!=0 && !request.pending))
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
    if (!request.onPublish && (!currentStage || currentStage->image != request.pending)) {
        return GetResult(
            request,
            LoadCommitStatus::Failed,
            LoadCommitFailure::StaleInput);
    }

    if (!m_transaction) {
        auto transaction = std::make_unique<Transaction>();
        transaction->request = request;
        m_transaction = std::move(transaction);
        for (const auto& stage : m_transaction->request.stages) {
            const auto status = stage->StartRenderInputStage(
                input,
                m_transaction->request.transactionRevision);
            if (status == DataStageStatus::Failed
                || status == DataStageStatus::Cancelled
                || status == DataStageStatus::Idle) {
                (void)ClearStages(m_transaction->request, false, 0);
                m_transaction.reset();
                return GetResult(
                    request,
                    status == DataStageStatus::Cancelled
                        ? LoadCommitStatus::Cancelled
                        : LoadCommitStatus::Failed,
                    status == DataStageStatus::Cancelled
                        ? LoadCommitFailure::Cancelled
                        : LoadCommitFailure::StageFailed);
            }
        }
        return GetResult(
            request,
            LoadCommitStatus::Preparing,
            LoadCommitFailure::None);
    }

    auto& active = m_transaction->request;
    if (active.ownerId != request.ownerId && request.ownerId != 0) {
        return GetResult(request, LoadCommitStatus::Failed, LoadCommitFailure::InvalidRequest);
    }
    if (active.transactionRevision != request.transactionRevision || active.ownerId != request.ownerId) {
        const auto stale = active;
        (void)ClearStages(stale, false, 0);
        m_transaction.reset();
        return AdvanceLoadCommit(request);
    }
    if (active.pending != request.pending || active.renderInput != request.renderInput
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
        const auto status = stage->SetRenderInputStageReady(
            input, active.transactionRevision);
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

    // 最终提交后只进行 noexcept 收尾，先复制终态所需的请求/闭包。
    const auto completedRequest = active;
    std::size_t committedCount = 0;
    for (const auto& stage : active.stages) {
        ++m_transaction->attemptedCommits;
        if (!stage->SetRenderInputViewStage(
                input, active.transactionRevision)) {
            break;
        }
        ++committedCount;
    }
    if (committedCount != active.stages.size()) {
        const auto terminal = active;
        (void)ClearStages(terminal, true, m_transaction->attemptedCommits);
        m_transaction.reset();
        return GetResult(
            terminal,
            LoadCommitStatus::Failed,
            LoadCommitFailure::CommitFailed);
    }

    VtkImageGridSnapshot published;
    const bool isPublished = active.onPublish ? active.onPublish()
        : (m_dataManager->SetLoadCommit(currentStage, published)
            && published && published->data && published->data->self == active.sourceRevision);
    if (!isPublished) {
        const auto terminal = active;
        (void)ClearStages(terminal, true, m_transaction->attemptedCommits);
        m_transaction.reset();
        return GetResult(
            terminal,
            LoadCommitStatus::Failed,
            LoadCommitFailure::PublishFailed);
    }

    const auto& terminal = completedRequest;
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
    const LoadCommitFailure failureReason,
    const std::uint64_t ownerId)
{
    LoadCommitResult result;
    result.status = LoadCommitStatus::Cancelled;
    result.failureReason = failureReason;
    result.transactionRevision = transactionRevision;
    if (m_isAdvancing) {
        result.status = LoadCommitStatus::Failed;
        result.failureReason = LoadCommitFailure::CommitFailed;
        return result;
    }
    if (!m_transaction || m_transaction->request.ownerId != ownerId
        || m_transaction->request.transactionRevision
            != transactionRevision) {
        return result;
    }
    struct CancelGuard final {
        bool& value;
        explicit CancelGuard(bool& flag) : value(flag) { value = true; }
        ~CancelGuard() { value = false; }
    } guard(m_isAdvancing);
    const auto& terminal = m_transaction->request;
    result.sourceRevision = terminal.sourceRevision;
    (void)ClearStages(terminal, false, 0);
    m_transaction.reset();
    return result;
}
