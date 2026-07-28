#include "AppDataExportTaskService.h"
#include "InteractionComputeService.h"

#include <exception>
#include <iostream>
#include <utility>

AppDataExportTaskService::AppDataExportTaskService(
    std::shared_ptr<AbstractDataManager> dataManager,
    std::shared_ptr<SharedInteractionState> sharedState)
    : m_dataManager(std::move(dataManager))
    , m_sharedState(std::move(sharedState))
{
}

std::optional<std::packaged_task<bool()>>
AppDataExportTaskService::BuildDataTask(
    std::string outputDir,
    std::string extension)
{
    if (!m_dataManager || !m_sharedState
        || outputDir.empty() || extension.empty()) {
        return std::nullopt;
    }
    const auto imageSnapshot =
        m_dataManager->GetImageSnapshot();
    if (!imageSnapshot || !imageSnapshot->image
        || imageSnapshot->image->GetNumberOfPoints() == 0) {
        return std::nullopt;
    }
    auto dataManager = m_dataManager;
    const auto modelToWorld = m_sharedState->GetModelMatrix();
    const double isoValue =
        m_sharedState->GetIsoValue();
    return std::packaged_task<bool()>(
        [dataManager, imageSnapshot,
         outputDir = std::move(outputDir),
         extension = std::move(extension),
         isoValue, modelToWorld]() mutable
        {
            try {
                return dataManager->ExportData(
                    imageSnapshot, outputDir, extension,
                    isoValue,
                    modelToWorld);
            }
            catch (const std::exception& error) {
                std::cerr << "[Export] Worker failed: " << error.what() << '\n';
            }
            catch (...) {
                std::cerr << "[Export] Worker failed with an unknown exception.\n";
            }
            return false;
        });
}

std::optional<std::packaged_task<bool()>>
AppDataExportTaskService::BuildSlicesTask(
    std::string path,
    std::optional<double> rotationAngleDeg,
    VizMode currentMode)
{
    if (!m_dataManager || !m_sharedState || path.empty()
        || InteractionComputeService::GetSliceAxis(currentMode) < 0) {
        return std::nullopt;
    }
    const auto windowLevel = m_sharedState->GetWindowLevel();
    const auto modelToWorld = m_sharedState->GetModelMatrix();
    const auto cursorWorld = m_sharedState->GetCursorWorld();
    auto exportData = InteractionComputeService::GetSliceExportData(
        modelToWorld, currentMode, cursorWorld, rotationAngleDeg);
    if (!exportData) return std::nullopt;

    auto dataManager = m_dataManager;
    return std::packaged_task<bool()>(
        [dataManager, path = std::move(path),
         exportData = std::move(*exportData), windowLevel]() mutable
        {
            try {
                return dataManager->ExportSlices(
                    path, exportData.orientation, windowLevel, exportData.matrix);
            }
            catch (const std::exception& error) {
                std::cerr << "[Export] Worker failed: " << error.what() << '\n';
            }
            catch (...) {
                std::cerr << "[Export] Worker failed with an unknown exception.\n";
            }
            return false;
        });
}
