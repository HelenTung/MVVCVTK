#pragma once

// =====================================================================
// OrthogonalCropInteractionBridgeService — 正交裁切交互桥接服务
// =====================================================================
// 结构对齐 GapAnalysis：
// - BackendRouterService：按 vtkImageData / vtkPolyData 选择各自裁切实现；
// - WidgetStateController：只负责 3D 裁切盒 UI 状态；
// - InteractionBridgeService：仅做热键桥接、bounds 同步与结果投递，不耦合底层裁切流程。

#include "OrthogonalCrop/OrthogonalCropWidgetStateController.h"
#include "OrthogonalCrop/OrthogonalCropBackendRouterService.h"
#include "AppService.h"

#include <vtkCommand.h>
#include <vtkRenderWindowInteractor.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

class OrthogonalCropInteractionBridgeService : public OrthogonalCropBackendRouterService {
public:
    OrthogonalCropInteractionBridgeService()
    {
        m_widgetStateController.SetBoundsChangedCallback(
            [this](const std::array<double, 6>& bounds, CropInteractionPhase phase) {
                HandleWidgetBoundsChanged(bounds, phase);
            });
    }

    void SetDataManager(std::shared_ptr<AbstractDataManager> dataMgr)
    {
        m_dataMgr = std::move(dataMgr);
    }

    void SetPrimaryInteractor(vtkRenderWindowInteractor* interactor)
    {
        m_primaryInteractor = interactor;
        m_widgetStateController.SetInteractor(interactor);
    }

    void SetReferenceRenderService(std::shared_ptr<AbstractInteractiveService> referenceService)
    {
        m_referenceRenderService = std::move(referenceService);
        if (!m_referenceRenderService && !m_previewRenderServices.empty()) {
            m_referenceRenderService = m_previewRenderServices.front();
        }
    }

    void SetPreviewRenderServices(std::vector<std::shared_ptr<AbstractInteractiveService>> previewRenderServices)
    {
        m_previewRenderServices.clear();
        m_previewRenderServices.reserve(previewRenderServices.size());

        for (const auto& service : previewRenderServices) {
            AddPreviewRenderService(service);
        }

        if (!m_referenceRenderService && !m_previewRenderServices.empty()) {
            m_referenceRenderService = m_previewRenderServices.front();
        }
    }

    void HandleHotkey(vtkRenderWindowInteractor* interactor, unsigned long eventId)
    {
        if (!interactor) {
            return;
        }

        const std::string keySym = interactor->GetKeySym() ? interactor->GetKeySym() : "";
        const char keyCode = interactor->GetKeyCode();

        const bool isInsideKey = keyCode == '1' || keySym == "1";
        const bool isOutsideKey = keyCode == '2' || keySym == "2";
        const bool isToggleKey = keyCode == 'o' || keyCode == 'O' || keySym == "o" || keySym == "O";

        if (eventId == vtkCommand::KeyPressEvent) {
            if (isToggleKey) {
                ExecuteDemo();
                return;
            }

            if (keySym == "Escape") {
                DeactivateDemo();
                return;
            }

            if (isInsideKey) {
                m_keepInsideShortcutDown = true;
                UpdateRemovalModeFromShortcuts(true);
                return;
            }

            if (isOutsideKey) {
                m_keepOutsideShortcutDown = true;
                UpdateRemovalModeFromShortcuts(true);
                return;
            }
        }

        if (eventId == vtkCommand::KeyReleaseEvent) {
            if (isInsideKey) {
                m_keepInsideShortcutDown = false;
                UpdateRemovalModeFromShortcuts(true);
                return;
            }

            if (isOutsideKey) {
                m_keepOutsideShortcutDown = false;
                UpdateRemovalModeFromShortcuts(true);
                return;
            }
        }
    }

    bool ResetInteractiveBoundsToDefault(bool updatePreview = true)
    {
        if (!EnsureInputReady()) {
            return false;
        }

        m_currentBounds = GetDefaultInteractiveBounds();
        m_boundsInitialized = true;
        m_lastInteractionPhase = CropInteractionPhase::Released;
        m_widgetStateController.SetReferenceBounds(GetActiveWorldBounds());
        m_widgetStateController.SetWidgetBounds(m_currentBounds);
        if (updatePreview) {
            UpdatePreviewFromCurrentBounds(true);
        }
        return true;
    }

    bool ExecuteDemo()
    {
        if (!EnsureInputReady()) {
            std::cerr << "[Main] Orthogonal crop trigger failed: no active image/polydata input is available yet." << std::endl;
            return false;
        }

        if (m_cropInteractionEnabled) {
            DeactivateDemo();
            return true;
        }

        if (!m_primaryInteractor) {
            std::cerr << "[Main] Orthogonal crop widget init failed: primary interactor missing." << std::endl;
            return false;
        }

        if (!m_boundsInitialized) {
            m_currentBounds = GetDefaultInteractiveBounds();
            m_boundsInitialized = true;
        }

        m_widgetStateController.SetInteractor(m_primaryInteractor);
        m_widgetStateController.SetReferenceBounds(GetActiveWorldBounds());
        m_widgetStateController.SetWidgetBounds(m_currentBounds);
        if (!m_widgetStateController.SetEnabled(true)) {
            std::cerr << "[Main] Orthogonal crop widget init failed: vtkBoxWidget2 could not be enabled." << std::endl;
            return false;
        }

        m_cropInteractionEnabled = true;
        m_lastInteractionPhase = CropInteractionPhase::Released;
        UpdatePreviewFromCurrentBounds(true);
        std::cout << "[Main] Orthogonal crop widget active. UI uses vtkBoxWidget2, backend = "
            << GetDataSourceText(GetActiveDataSource())
            << ". Hold 1 to keep inside, hold 2 to keep outside, press O or Esc to exit." << std::endl;
        return true;
    }

    bool DeactivateDemo()
    {
        if (!m_cropInteractionEnabled) {
            return false;
        }

        m_widgetStateController.SetEnabled(false);
        m_cropInteractionEnabled = false;
        m_keepInsideShortcutDown = false;
        m_keepOutsideShortcutDown = false;
        m_currentRemovalMode = m_defaultRemovalMode;
        m_lastInteractionPhase = CropInteractionPhase::Released;
        if (m_boundsInitialized) {
            UpdatePreviewFromCurrentBounds(false);
        }
        std::cout << "[Main] Orthogonal crop widget deactivated. 3D navigation restored." << std::endl;
        return true;
    }

private:
    static std::array<std::array<double, 3>, 8> GetCornersFromBounds(const std::array<double, 6>& bounds)
    {
        return {{
            { bounds[0], bounds[2], bounds[4] },
            { bounds[1], bounds[2], bounds[4] },
            { bounds[1], bounds[3], bounds[4] },
            { bounds[0], bounds[3], bounds[4] },
            { bounds[0], bounds[2], bounds[5] },
            { bounds[1], bounds[2], bounds[5] },
            { bounds[1], bounds[3], bounds[5] },
            { bounds[0], bounds[3], bounds[5] }
        }};
    }

    bool EnsureInputReady()
    {
        if (GetActiveDataSource() != OrthogonalCropDataSource::Auto) {
            return true;
        }

        if (!GetInputImage() && m_dataMgr) {
            SetInputImage(m_dataMgr->GetVtkImage());
        }

        return GetActiveDataSource() != OrthogonalCropDataSource::Auto;
    }

    std::array<double, 6> GetDefaultInteractiveBounds() const
    {
        std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        const auto sourceBounds = GetActiveWorldBounds();
        if (!(sourceBounds[0] < sourceBounds[1]
            && sourceBounds[2] < sourceBounds[3]
            && sourceBounds[4] < sourceBounds[5])) {
            return bounds;
        }

        const std::array<double, 3> center = {
            (sourceBounds[0] + sourceBounds[1]) * 0.5,
            (sourceBounds[2] + sourceBounds[3]) * 0.5,
            (sourceBounds[4] + sourceBounds[5]) * 0.5
        };
        const std::array<double, 3> dimensions = {
            (sourceBounds[1] - sourceBounds[0]) * 0.30,
            (sourceBounds[3] - sourceBounds[2]) * 0.24,
            (sourceBounds[5] - sourceBounds[4]) * 0.36
        };

        bounds[0] = center[0] - dimensions[0] * 0.5;
        bounds[1] = center[0] + dimensions[0] * 0.5;
        bounds[2] = center[1] - dimensions[1] * 0.5;
        bounds[3] = center[1] + dimensions[1] * 0.5;
        bounds[4] = center[2] - dimensions[2] * 0.5;
        bounds[5] = center[2] + dimensions[2] * 0.5;
        return bounds;
    }

    void HandleWidgetBoundsChanged(const std::array<double, 6>& bounds, CropInteractionPhase phase)
    {
        if (!m_cropInteractionEnabled) {
            return;
        }

        m_currentBounds = bounds;
        m_boundsInitialized = true;
        m_lastInteractionPhase = phase;
        if (phase == CropInteractionPhase::Released) {
            UpdatePreviewFromCurrentBounds(true);
        }
    }

    void GetWorldPositionFromModel(const double modelPos[3], double worldPos[3]) const
    {
        if (m_referenceRenderService) {
            m_referenceRenderService->GetWorldPositionFromModel(modelPos, worldPos);
            return;
        }

        worldPos[0] = modelPos[0];
        worldPos[1] = modelPos[1];
        worldPos[2] = modelPos[2];
    }

    void GetModelPositionFromWorld(const double worldPos[3], double modelPos[3]) const
    {
        if (m_referenceRenderService) {
            m_referenceRenderService->GetModelPositionFromWorld(worldPos, modelPos);
            return;
        }

        modelPos[0] = worldPos[0];
        modelPos[1] = worldPos[1];
        modelPos[2] = worldPos[2];
    }

    std::array<double, 6> GetTransformedBounds(const std::array<double, 6>& sourceBounds, bool modelToWorld) const
    {
        std::array<double, 6> transformedBounds = {
            std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()
        };

        const auto corners = GetCornersFromBounds(sourceBounds);
        for (const auto& corner : corners) {
            double transformedPoint[3] = { 0.0, 0.0, 0.0 };
            if (modelToWorld) {
                GetWorldPositionFromModel(corner.data(), transformedPoint);
            }
            else {
                GetModelPositionFromWorld(corner.data(), transformedPoint);
            }

            transformedBounds[0] = std::min(transformedBounds[0], transformedPoint[0]);
            transformedBounds[1] = std::max(transformedBounds[1], transformedPoint[0]);
            transformedBounds[2] = std::min(transformedBounds[2], transformedPoint[1]);
            transformedBounds[3] = std::max(transformedBounds[3], transformedPoint[1]);
            transformedBounds[4] = std::min(transformedBounds[4], transformedPoint[2]);
            transformedBounds[5] = std::max(transformedBounds[5], transformedPoint[2]);
        }

        return transformedBounds;
    }

    std::array<double, 6> GetActiveWorldBounds() const
    {
        const auto modelBounds = GetActiveInputBounds();
        if (!(modelBounds[0] < modelBounds[1]
            && modelBounds[2] < modelBounds[3]
            && modelBounds[4] < modelBounds[5])) {
            return modelBounds;
        }

        return GetTransformedBounds(modelBounds, true);
    }

    std::array<double, 6> GetModelBoundsFromWorldBounds(const std::array<double, 6>& worldBounds) const
    {
        return GetTransformedBounds(worldBounds, false);
    }

    OrthogonalCropRequest BuildPreviewRequest() const
    {
        auto previewRequest = GetDefaultRequest();
        previewRequest.SetBoundsMode(CropBoundsMode::MinMaxCoordinates);
        previewRequest.SetRasBounds(GetModelBoundsFromWorldBounds(m_currentBounds));
        previewRequest.SetExecutionMode(CropExecutionMode::VirtualCrop);
        previewRequest.SetRemovalMode(m_currentRemovalMode);

        auto cropState = previewRequest.GetCropStateModel();
        cropState.SetCropEnabled(m_cropInteractionEnabled);
        cropState.SetInteractionPhase(m_lastInteractionPhase);
        previewRequest.SetCropStateModel(cropState);
        return previewRequest;
    }

    void UpdatePreviewFromCurrentBounds(bool logStats)
    {
        if (!m_boundsInitialized) {
            return;
        }

        const auto previewRequest = BuildPreviewRequest();
        const auto previewResult = GetResult(previewRequest);
        const auto& previewStats = previewResult.GetStatistics();
        if (previewResult.GetFailureReason() != OrthogonalCropFailureReason::None) {
            if (logStats) {
                std::cerr << "[Main] Orthogonal crop preview failed: "
                    << GetFailureReasonText(previewResult.GetFailureReason())
                    << " - " << previewResult.GetMessage() << std::endl;
            }
            return;
        }

        if (!previewResult.GetSucceeded()) {
            if (logStats) {
                std::cerr << "[Main] Orthogonal crop preview result warning: "
                    << GetFailureReasonText(previewResult.GetFailureReason())
                    << " - " << previewResult.GetMessage() << std::endl;
            }
            return;
        }

        SetPreviewServicesDirty();

        if (logStats) {
            const auto dataSource = previewResult.GetResolvedDataSource() != OrthogonalCropDataSource::Auto
                ? previewResult.GetResolvedDataSource()
                : previewStats.GetResolvedDataSource();
            const auto backend = previewResult.GetResolvedBackend() != OrthogonalCropResolvedBackend::None
                ? previewResult.GetResolvedBackend()
                : previewStats.GetResolvedBackend();
            std::cout
                << "[Main] Orthogonal crop preview updated. source = "
                << GetDataSourceText(dataSource)
                << ", backend = "
                << GetResolvedBackendText(backend)
                << ", inside = "
                << previewStats.GetInsideVoxelCount()
                << ", output = "
                << previewStats.GetOutputVoxelCount()
                << ", removal = "
                << GetRemovalModeText(m_currentRemovalMode)
                << ", bounds = ["
                << m_currentBounds[0] << ", " << m_currentBounds[1] << "; "
                << m_currentBounds[2] << ", " << m_currentBounds[3] << "; "
                << m_currentBounds[4] << ", " << m_currentBounds[5] << "]"
                << std::endl;
        }
    }

    void UpdateRemovalModeFromShortcuts(bool logStats)
    {
        CropRemovalMode nextRemovalMode = m_defaultRemovalMode;
        if (m_keepOutsideShortcutDown) {
            nextRemovalMode = CropRemovalMode::RemoveInside;
        }
        else if (m_keepInsideShortcutDown) {
            nextRemovalMode = CropRemovalMode::KeepInside;
        }

        if (nextRemovalMode == m_currentRemovalMode) {
            return;
        }

        m_currentRemovalMode = nextRemovalMode;
        std::cout << "[Main] Orthogonal crop removal mode: "
            << GetRemovalModeText(m_currentRemovalMode)
            << std::endl;

        if (m_cropInteractionEnabled
            && m_lastInteractionPhase != CropInteractionPhase::Dragging) {
            UpdatePreviewFromCurrentBounds(logStats);
        }
    }

    static const char* GetFailureReasonText(OrthogonalCropFailureReason failureReason)
    {
        switch (failureReason) {
        case OrthogonalCropFailureReason::None:
            return "None";
        case OrthogonalCropFailureReason::InputImageMissing:
            return "InputImageMissing";
        case OrthogonalCropFailureReason::InputPolyDataMissing:
            return "InputPolyDataMissing";
        case OrthogonalCropFailureReason::InvalidBounds:
            return "InvalidBounds";
        case OrthogonalCropFailureReason::BoundsOutOfRange:
            return "BoundsOutOfRange";
        case OrthogonalCropFailureReason::PhysicalRemoveInsideUnsupported:
            return "PhysicalRemoveInsideUnsupported";
        case OrthogonalCropFailureReason::InsufficientRam:
            return "InsufficientRam";
        case OrthogonalCropFailureReason::VirtualMaskCreationFailed:
            return "VirtualMaskCreationFailed";
        case OrthogonalCropFailureReason::DerivedImageCreationFailed:
            return "DerivedImageCreationFailed";
        case OrthogonalCropFailureReason::DerivedPolyDataCreationFailed:
            return "DerivedPolyDataCreationFailed";
        }

        return "Unknown";
    }

    static const char* GetRemovalModeText(CropRemovalMode removalMode)
    {
        switch (removalMode) {
        case CropRemovalMode::KeepInside:
            return "KeepInside";
        case CropRemovalMode::RemoveInside:
            return "RemoveInside";
        }

        return "Unknown";
    }

    static const char* GetDataSourceText(OrthogonalCropDataSource dataSource)
    {
        switch (dataSource) {
        case OrthogonalCropDataSource::ImageData:
            return "ImageData";
        case OrthogonalCropDataSource::PolyData:
            return "PolyData";
        case OrthogonalCropDataSource::Auto:
        default:
            return "Auto";
        }
    }

    static const char* GetResolvedBackendText(OrthogonalCropResolvedBackend backend)
    {
        switch (backend) {
        case OrthogonalCropResolvedBackend::ImageVirtualMask:
            return "ImageVirtualMask";
        case OrthogonalCropResolvedBackend::ImageExtractVOI:
            return "ImageExtractVOI";
        case OrthogonalCropResolvedBackend::ImageMapperCropping:
            return "ImageMapperCropping";
        case OrthogonalCropResolvedBackend::PolyDataClipDataSet:
            return "PolyDataClipDataSet";
        case OrthogonalCropResolvedBackend::None:
        default:
            return "None";
        }
    }

    void AddPreviewRenderService(const std::shared_ptr<AbstractInteractiveService>& service)
    {
        if (!service) {
            return;
        }

        if (std::find(m_previewRenderServices.begin(), m_previewRenderServices.end(), service) != m_previewRenderServices.end()) {
            return;
        }

        m_previewRenderServices.push_back(service);
    }

    void SetPreviewServicesDirty()
    {
        for (const auto& service : m_previewRenderServices) {
            if (service) {
                service->SetDirtyMarked();
            }
        }
    }

    std::shared_ptr<AbstractDataManager> m_dataMgr;
    vtkRenderWindowInteractor* m_primaryInteractor = nullptr;
    bool m_cropInteractionEnabled = false;
    bool m_boundsInitialized = false;
    bool m_keepInsideShortcutDown = false;
    bool m_keepOutsideShortcutDown = false;
    CropInteractionPhase m_lastInteractionPhase = CropInteractionPhase::Idle;
    CropRemovalMode m_defaultRemovalMode = CropRemovalMode::KeepInside;
    CropRemovalMode m_currentRemovalMode = CropRemovalMode::KeepInside;
    std::array<double, 6> m_currentBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    OrthogonalCropWidgetStateController m_widgetStateController;
    std::shared_ptr<AbstractInteractiveService> m_referenceRenderService;
    std::vector<std::shared_ptr<AbstractInteractiveService>> m_previewRenderServices;
};
