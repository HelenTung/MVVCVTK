#include "AppDataLoadTaskService.h"

#include <exception>
#include <iostream>
#include <utility>

AppDataLoadTaskService::AppDataLoadTaskService(
    std::shared_ptr<AbstractDataManager> dataManager)
    : m_dataManager(std::move(dataManager))
{
}

std::optional<std::packaged_task<bool()>>
AppDataLoadTaskService::BuildLoadFileTask(
    std::string path,
    VolumeLayout layout)
{
    if (!m_dataManager || path.empty()) return std::nullopt;
    auto dataManager = m_dataManager;
    return std::packaged_task<bool()>(
        [dataManager, path = std::move(path), layout = std::move(layout)]() mutable
        {
            try {
                return dataManager->SetDataLoaded(path, layout);
            }
            catch (const std::exception& error) {
                std::cerr << "[LoadFileAsync] Worker failed: "
                    << error.what() << '\n';
            }
            catch (...) {
                std::cerr << "[LoadFileAsync] Worker failed with an unknown exception.\n";
            }
            return false;
        });
}

std::optional<std::packaged_task<bool()>>
AppDataLoadTaskService::BuildReloadTask(VolumeBuffer buffer)
{
    if (!m_dataManager) return std::nullopt;
    auto dataManager = m_dataManager;
    return std::packaged_task<bool()>(
        [dataManager, buffer = std::move(buffer)]() mutable
        {
            try {
                return dataManager->SetFromBuffer(buffer);
            }
            catch (const std::exception& error) {
                std::cerr << "[ReloadFromBufferAsync] Worker failed: "
                    << error.what() << '\n';
            }
            catch (...) {
                std::cerr << "[ReloadFromBufferAsync] Worker failed with an unknown exception.\n";
            }
            return false;
        });
}
