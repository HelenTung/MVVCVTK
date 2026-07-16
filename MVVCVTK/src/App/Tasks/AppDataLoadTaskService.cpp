#include "AppDataLoadTaskService.h"
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

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
        if (onComplete) {
            m_fileLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        }
        return std::nullopt;
    }

    if (!m_sharedState->StartLoad(LoadEventKind::File)) {
        std::cerr << "[LoadFileAsync] Loading is already in progress.\n";
        if (onComplete) {
            m_fileLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        }
        return std::nullopt;
    }
    if (!m_dataManager->ClearPending()) {
        m_sharedState->SetFileLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::File);
        if (onComplete) {
            m_fileLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        }
        return std::nullopt;
    }

    m_fileLoadCallbackState.SetCallback(std::move(onComplete));

    auto dataManager = m_dataManager;
    auto sharedState = m_sharedState;

    return std::packaged_task<void()>(
        [dataManager, sharedState, path, spacing, origin]() mutable
        {
            try {
                // 后台任务只负责 I/O 和基础数据快照准备；渲染管线重建统一留给主线程。
                const bool isOk = dataManager->SetDataLoaded(path, spacing, origin);
                if (isOk) {
                    // SetDataLoaded 成功即表示 current 批次有效；发布 metadata 不复制整卷 VTK image。
                    const auto scalarRange = dataManager->GetScalarRange();
                    sharedState->SetFileDataReady(
                        scalarRange[0],
                        scalarRange[1],
                        dataManager->GetSpacing());
                    return;
                }
                std::cerr << "[LoadFileAsync] Failed to load: " << path << "\n";
            }
            catch (const std::exception& error) {
                std::cerr << "[LoadFileAsync] Worker failed: " << error.what() << "\n";
            }
            catch (...) {
                std::cerr << "[LoadFileAsync] Worker failed with an unknown exception.\n";
            }
            sharedState->SetFileLoadFailed();
        });
}

std::optional<std::packaged_task<void()>> AppDataLoadTaskService::BuildReloadFromBufferTask(
    const float* data,
    const std::array<int, 3>& dims,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool isSuccess)> onComplete)
{
    if (!m_sharedState || !m_dataManager || !data) {
        if (onComplete) {
            m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        }
        return std::nullopt;
    }

    std::size_t voxelCount = 1;
    for (const int dim : dims) {
        if (dim <= 0
            || voxelCount > std::numeric_limits<std::size_t>::max()
                / static_cast<std::size_t>(dim)) {
            if (onComplete) {
                m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
            }
            return std::nullopt;
        }
        voxelCount *= static_cast<std::size_t>(dim);
    }
    if (voxelCount > static_cast<std::size_t>(
        std::numeric_limits<std::ptrdiff_t>::max())) {
        if (onComplete) {
            m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        }
        return std::nullopt;
    }
    if (!m_sharedState->StartLoad(LoadEventKind::Reload)) {
        std::cerr << "[ReloadFromBufferAsync] Loading is already in progress.\n";
        if (onComplete) {
            m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        }
        return std::nullopt;
    }
    if (!m_dataManager->ClearPending()) {
        m_sharedState->SetReloadLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::Reload);
        if (onComplete) {
            m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        }
        return std::nullopt;
    }

    // admission 成功后才同步接管裸 buffer；函数返回时 worker 不再依赖调用方生命周期。
    try {
        auto dataSnapshot = std::make_shared<const std::vector<float>>(
            data,
            data + voxelCount);
        auto dataManager = m_dataManager;
        auto sharedState = m_sharedState;
        std::packaged_task<void()> task(
            [dataManager, sharedState, dataSnapshot, dims, spacing, origin]() mutable
            {
                try {
                    // 在后台线程内只构建待提交镜像；真正的 vtkImage 切换由主线程消费。
                    if (dataManager->SetFromBuffer(dataSnapshot->data(), dims, spacing, origin)) {
                        return;
                    }
                }
                catch (const std::exception& error) {
                    std::cerr << "[ReloadFromBufferAsync] Worker failed: " << error.what() << "\n";
                }
                catch (...) {
                    std::cerr << "[ReloadFromBufferAsync] Worker failed with an unknown exception.\n";
                }
                sharedState->SetReloadLoadFailed();
            });
        m_reloadLoadCallbackState.SetCallback(std::move(onComplete));
        return task;
    }
    catch (const std::exception&) {
        m_sharedState->SetReloadLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::Reload);
        if (onComplete) {
            m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        }
        return std::nullopt;
    }
    catch (...) {
        m_sharedState->SetReloadLoadFailed();
        m_sharedState->ResetLoad(LoadEventKind::Reload);
        if (onComplete) {
            m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        }
        return std::nullopt;
    }
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
