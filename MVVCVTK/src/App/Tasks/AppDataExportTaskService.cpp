#include "AppDataExportTaskService.h"
#include "InteractionComputeService.h"
#include <iostream>
#include <utility>

AppDataExportTaskService::AppDataExportTaskService(
    std::shared_ptr<AbstractDataManager> dataManager,
    std::shared_ptr<SharedInteractionState> sharedState)
    : m_dataManager(std::move(dataManager))
    , m_sharedState(std::move(sharedState))
{
}

std::optional<std::packaged_task<void()>> AppDataExportTaskService::BuildDataTask(
    const std::string& path,
    std::function<void(bool isSuccess)> onComplete)
{
    SetSaveCallback(std::move(onComplete));

    auto dataManager = m_dataManager;
    auto sharedState = m_sharedState;

    if (!dataManager || !sharedState) {
        SetSaveCallbackReady(false);
        return std::nullopt;
    }

    if (path.empty()) {
        std::cerr << "[Export] Transformed data export skipped: output path is empty." << std::endl;
        SetSaveCallbackReady(false);
        return std::nullopt;
    }

    const std::array<double, 16> modelToWorld = sharedState->GetModelMatrix();
    // 导出后台任务可能比 owning service 活得久；捕获 weak_ptr 可以在 service 已销毁时安全丢弃回调投递。
    std::weak_ptr<AppDataExportTaskService> weakSelf = shared_from_this();

    return std::packaged_task<void()>(
        [dataManager, path, modelToWorld, weakSelf]() mutable
        {
            const bool isOk = dataManager->ExportData(path, modelToWorld);

            auto self = weakSelf.lock();
            if (self) {
                self->SetSaveCallbackReady(isOk);
            }
        });
}

std::optional<std::packaged_task<void()>> AppDataExportTaskService::BuildSlicesTask(
    const std::string& path,
    std::optional<double> rotationAngleDeg,
    VizMode currentMode,
    std::function<void(bool isSuccess)> onComplete)
{
    SetSaveCallback(std::move(onComplete));

    auto dataManager = m_dataManager;
    auto sharedState = m_sharedState;

    if (!dataManager || !sharedState) {
        SetSaveCallbackReady(false);
        return std::nullopt;
    }

    if (path.empty()) {
        std::cerr << "[Export] Slice image export skipped: output directory is empty." << std::endl;
        SetSaveCallbackReady(false);
        return std::nullopt;
    }

    // 切片导出只接受真实切片视图模式；3D 窗口误触不能退回 Top_down，否则上位机看到的是“成功但方向错误”的隐性失败。
    if (InteractionComputeService::GetSliceAxis(currentMode) < 0) {
        SetSaveCallbackReady(false);
        return std::nullopt;
    }

    const WindowLevelParams currentWindowLevel = sharedState->GetWindowLevel();
    const std::array<double, 16> modelToWorld = sharedState->GetModelMatrix();
    const std::array<double, 3> cursorWorldSnapshot = sharedState->GetCursorWorld();
    // 切片导出使用提交瞬间的姿态、窗宽窗位、游标和可选上位机角度，避免后台执行时被后续交互改成另一帧状态。
    const std::optional<SliceExportData> exportData = InteractionComputeService::GetSliceExportData(
        modelToWorld,
        currentMode,
        cursorWorldSnapshot,
        rotationAngleDeg);
    if (!exportData) {
        SetSaveCallbackReady(false);
        return std::nullopt;
    }

    std::weak_ptr<AppDataExportTaskService> weakSelf = shared_from_this();

    return std::packaged_task<void()>(
        [dataManager, path, exportData, currentWindowLevel, weakSelf]() mutable
        {
            const bool isOk = dataManager->ExportSlices(
                path,
                exportData->orientation,
                currentWindowLevel,
                exportData->matrix);

            auto self = weakSelf.lock();
            if (self) {
                self->SetSaveCallbackReady(isOk);
            }
        });
}

bool AppDataExportTaskService::GetSaveCallback()
{
    return m_saveCallback.GetCallbackConsumed();
}

void AppDataExportTaskService::SendSaveCallback()
{
    m_saveCallback.SendCallback();
}

void AppDataExportTaskService::SetSaveCallback(std::function<void(bool)> callback)
{
    m_saveCallback.SetCallback(std::move(callback));
}

void AppDataExportTaskService::SetSaveCallbackReady(
    bool isSuccess,
    std::function<void(bool)> callback)
{
    m_saveCallback.SetCallbackReady(isSuccess, std::move(callback));
}
