#pragma once

// =====================================================================
// OrthogonalCropInteractionBridgeService - 正交裁切交互桥接服务
// =====================================================================

#include "OrthogonalCrop/OrthogonalCropWidgetStateController.h"
#include "OrthogonalCrop/OrthogonalCropBackendRouterService.h"
#include "OrthogonalCropPreviewStrategy/OrthogonalCropPreviewOverlayStrategy.h"
#include "AppService.h"

#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <array>
#include <memory>
#include <utility>
#include <vector>

class OrthogonalCropInteractionBridgeService {
public:
    OrthogonalCropInteractionBridgeService();

    void CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image);
    void SetInputImage(vtkSmartPointer<vtkImageData> image);
    vtkSmartPointer<vtkImageData> GetInputImage() const;

    void CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);
    vtkSmartPointer<vtkPolyData> GetInputPolyData() const;

    void CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource);
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);

    OrthogonalCropDataSource GetActiveDataSource() const;
    std::array<double, 6> GetActiveInputBounds() const;
    OrthogonalCropRequest GetDefaultRequest() const;
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const;
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const;

    void SetDataManager(std::shared_ptr<AbstractDataManager> dataMgr);
    void SetPrimaryInteractor(vtkRenderWindowInteractor* interactor);
    void SetReferenceRenderService(std::shared_ptr<AbstractInteractiveService> referenceService);
    void SetPreviewRenderServices(std::vector<std::shared_ptr<AbstractInteractiveService>> previewRenderServices);

    bool ToggleInteractiveCrop();
    bool ExitInteractiveCrop();
    void ToggleInsidePreview();
    void ToggleOutsidePreview();
    bool ResetInteractiveBoundsToDefault(bool updatePreview = true);
    bool ActivateInteractiveCrop();
    bool ExecuteDemo();
    bool DeactivateInteractiveCrop();
    bool DeactivateDemo();

private:
    template <typename BackendMethod, typename... Args>
    decltype(auto) CallBackend(BackendMethod method, Args&&... args)
    {
        return (m_backend.*method)(std::forward<Args>(args)...);
    }

    template <typename BackendMethod, typename... Args>
    decltype(auto) CallBackend(BackendMethod method, Args&&... args) const
    {
        return (m_backend.*method)(std::forward<Args>(args)...);
    }

    struct PreviewRenderTarget {
        std::shared_ptr<AbstractInteractiveService> service;
        std::shared_ptr<OrthogonalCropPreviewOverlayStrategy> overlayStrategy;
        vtkPolyDataMapper* mainPreviewMapper = nullptr;
        vtkSmartPointer<vtkPolyData> mainPreviewSourcePolyData;
    };

    bool EnsureInputReady();
    std::array<double, 6> GetDefaultInteractiveBounds() const;
    void HandleWidgetBoundsChanged(const std::array<double, 6>& bounds, CropInteractionPhase phase);
    std::array<double, 6> GetTransformedBounds(const std::array<double, 6>& sourceBounds, bool modelToWorld) const;
    std::array<double, 6> GetActiveWorldBounds() const;
    std::array<double, 16> GetWorldToModelMatrix() const;
    OrthogonalCropRequest BuildPreviewRequest() const;
    void UpdatePreviewFromCurrentBounds(bool logStats);
    void TogglePreview(CropRemovalMode removalMode, bool logStats);
    vtkSmartPointer<vtkPlanes> GetCurrentWidgetPlanesForModelInput() const;

    static const char* GetFailureReasonText(OrthogonalCropFailureReason failureReason);
    static const char* GetRemovalModeText(CropRemovalMode removalMode);
    static const char* GetDataSourceText(OrthogonalCropDataSource dataSource);
    static const char* GetResolvedBackendText(OrthogonalCropResolvedBackend backend);

    std::shared_ptr<AbstractInteractiveService> GetFirstPreviewRenderService() const;
    void ClearPreviewRenderTargets();
    void RestorePreviewRenderTargets();
    void AddPreviewRenderService(const std::shared_ptr<AbstractInteractiveService>& service);
    bool SetPreviewServicesDirty(const OrthogonalCropResult& previewResult);
    void RestoreMainPolyDataPreview(PreviewRenderTarget& target);
    bool SetMainPolyDataPreviewApplied(PreviewRenderTarget& target, const OrthogonalCropResult& previewResult);

    OrthogonalCropBackendRouterService m_backend;
    std::shared_ptr<AbstractDataManager> m_dataMgr;
    vtkRenderWindowInteractor* m_primaryInteractor = nullptr;
    bool m_cropInteractionEnabled = false;
    bool m_boundsInitialized = false;
    bool m_previewEnabled = false;
    CropInteractionPhase m_lastInteractionPhase = CropInteractionPhase::Idle;
    CropRemovalMode m_currentRemovalMode = CropRemovalMode::KeepInside;
    std::array<double, 6> m_currentBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    OrthogonalCropWidgetStateController m_widgetStateController;
    std::shared_ptr<AbstractInteractiveService> m_referenceRenderService;
    std::vector<PreviewRenderTarget> m_previewRenderTargets;
};
