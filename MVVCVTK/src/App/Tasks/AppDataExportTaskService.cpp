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
    auto dataManager = m_dataManager;
    auto sharedState = m_sharedState;

    if (!dataManager || !sharedState) {
        SetSaveCallbackReady(false, std::move(onComplete));
        return std::nullopt;
    }

    if (path.empty()) {
        std::cerr << "[Export] Transformed data export skipped: output path is empty." << std::endl;
        SetSaveCallbackReady(false, std::move(onComplete));
        return std::nullopt;
    }

    // 调用线程先快照 4x4 model-to-world 矩阵（world = M * model），后台不再读取后续交互状态。
    const std::array<double, 16> modelToWorld = sharedState->GetModelMatrix();
    // 导出后台任务可能比 owning service 活得久；捕获 weak_ptr 可以在 service 已销毁时安全丢弃回调投递。
    std::weak_ptr<AppDataExportTaskService> weakSelf = shared_from_this();

    // onComplete 随 packaged_task 移动；任务完成后再随本任务的 bool 结果一起投递到 callback 状态。
    return std::packaged_task<void()>(
        [dataManager, path, modelToWorld, weakSelf, onComplete = std::move(onComplete)]() mutable
        {
            const bool isOk = dataManager->ExportData(path, modelToWorld);

            auto self = weakSelf.lock();
            if (self) {
                // 后台线程只发布完成 payload，未知业务回调由后续 VizService::SendUpdates 消费。
                self->SetSaveCallbackReady(isOk, std::move(onComplete));
            }
        });
}

std::optional<std::packaged_task<void()>> AppDataExportTaskService::BuildSlicesTask(
    const std::string& path,
    std::optional<double> rotationAngleDeg,
    VizMode currentMode,
    std::function<void(bool isSuccess)> onComplete)
{
    auto dataManager = m_dataManager;
    auto sharedState = m_sharedState;

    if (!dataManager || !sharedState) {
        SetSaveCallbackReady(false, std::move(onComplete));
        return std::nullopt;
    }

    if (path.empty()) {
        std::cerr << "[Export] Slice image export skipped: output directory is empty." << std::endl;
        SetSaveCallbackReady(false, std::move(onComplete));
        return std::nullopt;
    }

    // 切片导出只接受真实切片视图模式；3D 窗口误触不能退回 Top_down，否则上位机看到的是“成功但方向错误”的隐性失败。
    if (InteractionComputeService::GetSliceAxis(currentMode) < 0) {
        SetSaveCallbackReady(false, std::move(onComplete));
        return std::nullopt;
    }

    const WindowLevelParams currentWindowLevel = sharedState->GetWindowLevel();
    const std::array<double, 16> modelToWorld = sharedState->GetModelMatrix();
    const std::array<double, 3> cursorWorldSnapshot = sharedState->GetCursorWorld();
    // 切片导出使用提交瞬间的姿态、窗宽窗位、世界坐标游标和可选角度（度），避免后台读取到另一帧状态。
    const std::optional<SliceExportData> exportData = InteractionComputeService::GetSliceExportData(
        modelToWorld,
        currentMode,
        cursorWorldSnapshot,
        rotationAngleDeg);
    if (!exportData) {
        SetSaveCallbackReady(false, std::move(onComplete));
        return std::nullopt;
    }

    std::weak_ptr<AppDataExportTaskService> weakSelf = shared_from_this();

    // onComplete 与上述快照共同移入任务，确保每个完成结果始终跟随自己的 callback。
    return std::packaged_task<void()>(
        [dataManager, path, exportData, currentWindowLevel, weakSelf, onComplete = std::move(onComplete)]() mutable
        {
            const bool isOk = dataManager->ExportSlices(
                path,
                exportData->orientation,
                currentWindowLevel,
                exportData->matrix);

            auto self = weakSelf.lock();
            if (self) {
                // 多个任务同时完成时由 AppTaskCallbackState 按投递顺序串接，不在后台执行 callback。
                self->SetSaveCallbackReady(isOk, std::move(onComplete));
            }
        });
}

bool AppDataExportTaskService::ResetSaveCallback()
{
    // Timer 只领取门铃；闭包和 bool 结果仍留在受锁保护的 payload 槽。
    return m_saveCallback.ResetCallback();
}

void AppDataExportTaskService::SendSaveCallback()
{
    // callback 状态先在锁内移动 payload，再在锁外调用未知业务代码。
    m_saveCallback.SendCallback();
}

void AppDataExportTaskService::SetSaveCallbackReady(
    bool isSuccess,
    std::function<void(bool)> callback)
{
    // 统一把后台成功/失败快照和随任务移动的 callback 发布到同一个完成通道。
    m_saveCallback.SetCallbackReady(isSuccess, std::move(callback));
}
