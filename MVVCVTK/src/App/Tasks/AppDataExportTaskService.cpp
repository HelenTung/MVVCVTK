#include "AppDataExportTaskService.h"
#include "InteractionComputeService.h"

#include <exception>
#include <iostream>
#include <utility>

AppDataExportTaskService::AppDataExportTaskService(
    std::shared_ptr<AbstractDataManager> dataManager,
    std::shared_ptr<SharedInteractionState> sharedState,
    std::shared_ptr<ViewPresentationState> viewState)
    : m_dataManager(std::move(dataManager))
    , m_sharedState(std::move(sharedState))
    , m_viewState(std::move(viewState))
{
}

std::optional<std::packaged_task<bool(TaskStopToken)>>
AppDataExportTaskService::BuildDataTask(
    std::string outputDir,
    std::string extension)
{
    if (!m_dataManager || !m_sharedState || !m_viewState
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
    DataExportParams params;
    params.extension = std::move(extension);
    params.isoValue = m_viewState->GetIsoValue();
    params.modelToWorld = m_sharedState->GetModelMatrix();
    params.scalarRange = m_sharedState->GetDataRange();
    m_viewState->GetTFNodes(params.tfNodes);
    return std::packaged_task<bool(TaskStopToken)>(
        [dataManager, imageSnapshot,
         outputDir = std::move(outputDir),
         params = std::move(params)](
            TaskStopToken stopToken) mutable
        {
            try {
                return dataManager->ExportData(
                    imageSnapshot, outputDir, params, stopToken);
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

std::optional<std::packaged_task<bool(TaskStopToken)>>
AppDataExportTaskService::BuildSlicesTask(
    std::string path,
    std::optional<double> rotationAngleDeg,
    VizMode currentMode)
{
    if (!m_dataManager || !m_sharedState || !m_viewState || path.empty()
        || InteractionComputeService::GetSliceAxis(currentMode) < 0) {
        return std::nullopt;
    }
    const auto windowLevel = m_viewState->GetWindowLevel();
    const auto modelToWorld = m_sharedState->GetModelMatrix();
    const auto cursorWorld = m_sharedState->GetCursorWorld();
    auto exportData = InteractionComputeService::GetSliceExportData(
        modelToWorld, currentMode, cursorWorld, rotationAngleDeg);
    if (!exportData) return std::nullopt;

    auto dataManager = m_dataManager;
    return std::packaged_task<bool(TaskStopToken)>(
        [dataManager, path = std::move(path),
         exportData = std::move(*exportData), windowLevel](
            TaskStopToken stopToken) mutable
        {
            try {
                return dataManager->ExportSlices(
                    path,
                    exportData.orientation,
                    windowLevel,
                    exportData.matrix,
                    stopToken);
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
