#include "AppDataExportTaskService.h"
#include "InteractionComputeService.h"
#include <utility>

AppDataExportTaskService::AppDataExportTaskService(
    std::shared_ptr<AbstractDataManager> dataManager,
    std::shared_ptr<SharedInteractionState> sharedState)
    : m_dataManager(std::move(dataManager))
    , m_sharedState(std::move(sharedState))
{
}

std::optional<std::packaged_task<void()>> AppDataExportTaskService::BuildSaveTransformedDataTask(
    const std::string& path,
    std::function<void(bool success)> onComplete)
{
    SetSaveCallback(std::move(onComplete));

    auto dataManager = m_dataManager;
    auto sharedState = m_sharedState;
    const std::string resolvedPath = path.empty() && dataManager
        ? dataManager->GetDefaultTransformedDataPath()
        : path;

    if (!dataManager || !sharedState || resolvedPath.empty()) {
        SetSaveCallbackReady(false);
        return std::nullopt;
    }

    const std::array<double, 16> modelToWorldMatrixSnapshot = sharedState->GetModelMatrix();
    // 导出后台任务可能比 owning service 活得久；捕获 weak_ptr 可以在 service 已销毁时安全丢弃回调投递。
    std::weak_ptr<AppDataExportTaskService> weakSelf = shared_from_this();

    return std::packaged_task<void()>(
        [dataManager, resolvedPath, modelToWorldMatrixSnapshot, weakSelf]() mutable
        {
            const bool ok = dataManager->SaveTransformedData(resolvedPath, modelToWorldMatrixSnapshot);

            auto self = weakSelf.lock();
            if (self) {
                self->SetSaveCallbackReady(ok);
            }
        });
}

std::optional<std::packaged_task<void()>> AppDataExportTaskService::BuildSaveSliceImagesTask(
    const std::string& path,
    double angle,
    VizMode currentMode,
    std::function<void(bool success)> onComplete)
{
    SetSaveCallback(std::move(onComplete));

    auto dataManager = m_dataManager;
    auto sharedState = m_sharedState;

    if (!dataManager || !sharedState) {
        SetSaveCallbackReady(false);
        return std::nullopt;
    }

    const WindowLevelParams currentWindowLevel = sharedState->GetWindowLevel();
    const std::array<double, 16> modelToWorldMatrixSnapshot = sharedState->GetModelMatrix();
    // 切片导出使用提交瞬间的姿态、窗宽窗位和游标，避免后台执行时被后续交互改成另一帧状态。
    const SliceExportData exportData = InteractionComputeService::GetSliceExportData(
        modelToWorldMatrixSnapshot,
        currentMode,
        sharedState->GetCursorWorld(),
        angle);

    std::weak_ptr<AppDataExportTaskService> weakSelf = shared_from_this();

    return std::packaged_task<void()>(
        [dataManager, path, exportData, currentWindowLevel, weakSelf]() mutable
        {
            const bool ok = dataManager->SaveSliceImages(
                path,
                exportData.orientation,
                currentWindowLevel,
                exportData.matrix);

            auto self = weakSelf.lock();
            if (self) {
                self->SetSaveCallbackReady(ok);
            }
        });
}

bool AppDataExportTaskService::ConsumeSaveCallback()
{
    return m_saveCompletionCallbackState.GetPendingCallbackConsumed();
}

void AppDataExportTaskService::ExecutePendingSaveCallback()
{
    m_saveCompletionCallbackState.ExecutePendingCallback();
}

void AppDataExportTaskService::SetSaveCallback(std::function<void(bool)> callback)
{
    m_saveCompletionCallbackState.SetCallback(std::move(callback));
}

void AppDataExportTaskService::SetSaveCallbackReady(
    bool success,
    std::function<void(bool)> callback)
{
    m_saveCompletionCallbackState.SetCallbackReady(success, std::move(callback));
}
