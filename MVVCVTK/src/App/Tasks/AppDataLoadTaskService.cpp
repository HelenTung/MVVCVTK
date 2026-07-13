#include "AppDataLoadTaskService.h"
#include <iostream>
#include <utility>

AppDataLoadTaskService::AppDataLoadTaskService(
    std::shared_ptr<AbstractDataManager> dataManager,
    std::shared_ptr<SharedInteractionState> sharedState)
    : m_dataManager(std::move(dataManager))
    , m_sharedState(std::move(sharedState))
{
}

std::optional<std::packaged_task<void()>> AppDataLoadTaskService::BuildLoadFileTask(
    const std::string& path,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool isSuccess)> onComplete)
{
    if (!m_sharedState || !m_dataManager) {
        m_fileLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        return std::nullopt;
    }

    if (GetAnyLoadRunning()) {
        std::cerr << "[LoadFileAsync] Loading is already in progress.\n";
        m_fileLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        return std::nullopt;
    }

    m_fileLoadCallbackState.SetCallback(std::move(onComplete));
    // 先写 Loading 状态再返回任务，避免调用方启动线程后 UI 仍短暂看到 Idle。
    m_sharedState->SetFileLoadStarted();

    auto dataManager = m_dataManager;
    auto sharedState = m_sharedState;

    return std::packaged_task<void()>(
        [dataManager, sharedState, path, spacing, origin]() mutable
        {
            // 后台任务只负责 I/O 和基础数据快照准备；渲染管线重建统一留给主线程。
            const bool isOk = dataManager->SetDataLoaded(path, spacing, origin);

            if (isOk) {
                // 文件加载路径直接生成当前 vtkImage；这里可以只发布 DataReady，不碰 renderer / strategy。
                const auto imageState = dataManager->GetImageState();
                if (imageState.image) {
                    sharedState->SetFileDataReady(
                        imageState.scalarRange[0],
                        imageState.scalarRange[1],
                        imageState.spacing);
                }
                else {
                    std::cerr << "[LoadFileAsync] GetVtkImage() returned null after load.\n";
                    sharedState->SetFileLoadFailed();
                }
            }
            else {
                std::cerr << "[LoadFileAsync] Failed to load: " << path << "\n";
                sharedState->SetFileLoadFailed();
            }
        });
}

std::optional<std::packaged_task<void()>> AppDataLoadTaskService::BuildReloadFromBufferTask(
    const float* data,
    const std::array<int, 3>& dims,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool isSuccess)> onComplete)
{
    if (!m_sharedState || !m_dataManager) {
        m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        return std::nullopt;
    }

    if (GetAnyLoadRunning()) {
        std::cerr << "[ReloadFromBufferAsync] Loading is already in progress.\n";
        m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        return std::nullopt;
    }

    m_reloadLoadCallbackState.SetCallback(std::move(onComplete));
    // reload 先进入 pending image 通道；真正替换当前 vtkImage 要等主线程统一消费。
    m_sharedState->SetReloadLoadStarted();

    auto dataManager = m_dataManager;
    auto sharedState = m_sharedState;

    return std::packaged_task<void()>(
        [dataManager, sharedState, data, dims, spacing, origin]() mutable
        {
            // 在后台线程内只构建待提交镜像；真正的 vtkImage 切换由主线程消费。
            const bool isOk = dataManager->SetFromBuffer(data, dims, spacing, origin);

            if (!isOk) {
                sharedState->SetReloadLoadFailed();
            }
        });
}

void AppDataLoadTaskService::SetFileLoadCallbackReady(bool isSuccess)
{
    m_fileLoadCallbackState.SetCallbackReady(isSuccess);
}

void AppDataLoadTaskService::SetReloadReady(bool isSuccess)
{
    m_reloadLoadCallbackState.SetCallbackReady(isSuccess);
}

bool AppDataLoadTaskService::ResetFileCallback()
{
    return m_fileLoadCallbackState.ResetCallback();
}

bool AppDataLoadTaskService::ResetReloadCallback()
{
    return m_reloadLoadCallbackState.ResetCallback();
}

void AppDataLoadTaskService::SendFileLoadCallback()
{
    m_fileLoadCallbackState.SendCallback();
}

void AppDataLoadTaskService::SendReloadCallback()
{
    m_reloadLoadCallbackState.SendCallback();
}

bool AppDataLoadTaskService::GetAnyLoadRunning() const
{
    return m_sharedState
        && (m_sharedState->GetFileLoadState() == LoadState::Loading
            || m_sharedState->GetReloadLoadState() == LoadState::Loading);
}
