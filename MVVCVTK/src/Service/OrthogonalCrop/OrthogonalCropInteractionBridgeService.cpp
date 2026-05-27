// =====================================================================
// Path: MVVCVTK/src/Service/OrthogonalCrop/OrthogonalCropInteractionBridgeService.cpp
// 分类: Service / Interaction Bridge Implementation
// 说明: 维护交互态、构造 preview request、把结果分发给 overlay 与 3D 主模型预览。
// =====================================================================

#include "OrthogonalCrop/OrthogonalCropInteractionBridgeService.h"

#include <vtkActor.h>
#include <vtkBoundingBox.h>
#include <vtkMatrix4x4.h>
#include <vtkPlanes.h>
#include <vtkTransform.h>

#include <algorithm>
#include <iostream>
#include <utility>

OrthogonalCropInteractionBridgeService::OrthogonalCropInteractionBridgeService()
{
    // widget controller 只负责发 bounds/phase；桥接层在这里接管后续业务流程。
    m_widgetStateController.SetBoundsChangedCallback(
        [this](const std::array<double, 6>& bounds, CropInteractionPhase phase) {
            HandleWidgetBoundsChanged(bounds, phase);
        });
}

void OrthogonalCropInteractionBridgeService::CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    CallBackend(&OrthogonalCropBackendRouterService::CropPreInit_SetInputImage, std::move(image));
}

void OrthogonalCropInteractionBridgeService::SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    CropPreInit_SetInputImage(std::move(image));
}

vtkSmartPointer<vtkImageData> OrthogonalCropInteractionBridgeService::GetInputImage() const
{
    return CallBackend(&OrthogonalCropBackendRouterService::GetInputImage);
}

void OrthogonalCropInteractionBridgeService::CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    CallBackend(&OrthogonalCropBackendRouterService::CropPreInit_SetInputPolyData, std::move(polyData));
}

void OrthogonalCropInteractionBridgeService::SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    CropPreInit_SetInputPolyData(std::move(polyData));
}

vtkSmartPointer<vtkPolyData> OrthogonalCropInteractionBridgeService::GetInputPolyData() const
{
    return CallBackend(&OrthogonalCropBackendRouterService::GetInputPolyData);
}

void OrthogonalCropInteractionBridgeService::CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    CallBackend(&OrthogonalCropBackendRouterService::CropPreInit_SetPreferredDataSource, dataSource);
}

void OrthogonalCropInteractionBridgeService::SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    CropPreInit_SetPreferredDataSource(dataSource);
}

OrthogonalCropDataSource OrthogonalCropInteractionBridgeService::GetActiveDataSource() const
{
    return CallBackend(&OrthogonalCropBackendRouterService::GetActiveDataSource);
}

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetActiveInputBounds() const
{
    return CallBackend(&OrthogonalCropBackendRouterService::GetActiveInputBounds);
}

OrthogonalCropRequest OrthogonalCropInteractionBridgeService::GetDefaultRequest() const
{
    return CallBackend(&OrthogonalCropBackendRouterService::GetDefaultRequest);
}

OrthogonalCropStatistics OrthogonalCropInteractionBridgeService::GetStatistics(const OrthogonalCropRequest& request) const
{
    return CallBackend(&OrthogonalCropBackendRouterService::GetStatistics, request);
}

OrthogonalCropResult OrthogonalCropInteractionBridgeService::GetResult(const OrthogonalCropRequest& request) const
{
    return CallBackend(&OrthogonalCropBackendRouterService::GetResult, request);
}

void OrthogonalCropInteractionBridgeService::SetDataManager(std::shared_ptr<AbstractDataManager> dataMgr)
{
    m_dataMgr = std::move(dataMgr);
}

void OrthogonalCropInteractionBridgeService::SetPrimaryInteractor(vtkRenderWindowInteractor* interactor)
{
    m_primaryInteractor = interactor;
    m_widgetStateController.SetInteractor(interactor);
}

void OrthogonalCropInteractionBridgeService::SetReferenceRenderService(std::shared_ptr<AbstractInteractiveService> referenceService)
{
    m_referenceRenderService = std::move(referenceService);
    if (!m_referenceRenderService) {
        m_referenceRenderService = GetFirstPreviewRenderService();
    }
}

void OrthogonalCropInteractionBridgeService::SetPreviewRenderServices(std::vector<std::shared_ptr<AbstractInteractiveService>> previewRenderServices)
{
    ClearPreviewRenderTargets();
    m_previewRenderTargets.reserve(previewRenderServices.size());

    for (const auto& service : previewRenderServices) {
        AddPreviewRenderService(service);
    }

    if (!m_referenceRenderService) {
        m_referenceRenderService = GetFirstPreviewRenderService();
    }
}

bool OrthogonalCropInteractionBridgeService::ToggleInteractiveCrop()
{
    return ActivateInteractiveCrop();
}

bool OrthogonalCropInteractionBridgeService::ExitInteractiveCrop()
{
    return DeactivateInteractiveCrop();
}

void OrthogonalCropInteractionBridgeService::ToggleInsidePreview()
{
    TogglePreview(CropRemovalMode::KeepInside, true);
}

void OrthogonalCropInteractionBridgeService::ToggleOutsidePreview()
{
    TogglePreview(CropRemovalMode::RemoveInside, true);
}

bool OrthogonalCropInteractionBridgeService::ResetInteractiveBoundsToDefault(bool updatePreview)
{
    if (!EnsureInputReady()) {
        return false;
    }

    // 重置不仅要回写当前 bounds，还要同步 widget 与 phase，确保后续预览从一致状态起步。
    m_currentBounds = GetDefaultInteractiveBounds();
    m_boundsInitialized = true;
    m_lastInteractionPhase = CropInteractionPhase::Released;
    m_widgetStateController.SetReferenceBounds(GetActiveWorldBounds());
    m_widgetStateController.SetWidgetBounds(m_currentBounds);
    if (updatePreview && m_previewEnabled) {
        UpdatePreviewFromCurrentBounds(true);
    }
    else if (updatePreview) {
        RestorePreviewRenderTargets();
    }
    return true;
}

bool OrthogonalCropInteractionBridgeService::ActivateInteractiveCrop()
{
    if (!EnsureInputReady()) {
        std::cerr << "[Main] Orthogonal crop trigger failed: no active image/polydata input is available yet." << std::endl;
        return false;
    }

    if (m_cropInteractionEnabled) {
        DeactivateInteractiveCrop();
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

    // 进入交互模式时，widget 使用世界坐标 bounds；真正执行时再转回模型空间。
    m_widgetStateController.SetInteractor(m_primaryInteractor);
    m_widgetStateController.SetReferenceBounds(GetActiveWorldBounds());
    m_widgetStateController.SetWidgetBounds(m_currentBounds);
    if (!m_widgetStateController.SetEnabled(true)) {
        std::cerr << "[Main] Orthogonal crop widget init failed: vtkBoxWidget2 could not be enabled." << std::endl;
        return false;
    }

    m_cropInteractionEnabled = true;
    m_lastInteractionPhase = CropInteractionPhase::Released;
    RestorePreviewRenderTargets();
    std::cout << "[Main] Orthogonal crop widget active. UI uses vtkBoxWidget2, backend = "
        << GetDataSourceText(GetActiveDataSource())
        << ". Press 1 to toggle inside preview, press 2 to toggle outside preview; press O or Esc to exit." << std::endl;
    return true;
}

bool OrthogonalCropInteractionBridgeService::ExecuteDemo()
{
    return ActivateInteractiveCrop();
}

bool OrthogonalCropInteractionBridgeService::DeactivateInteractiveCrop()
{
    if (!m_cropInteractionEnabled) {
        return false;
    }

    m_widgetStateController.SetEnabled(false);
    m_cropInteractionEnabled = false;
    m_previewEnabled = false;
    m_lastInteractionPhase = CropInteractionPhase::Released;
    RestorePreviewRenderTargets();
    std::cout << "[Main] Orthogonal crop widget deactivated. 3D navigation restored." << std::endl;
    return true;
}

bool OrthogonalCropInteractionBridgeService::DeactivateDemo()
{
    return DeactivateInteractiveCrop();
}

bool OrthogonalCropInteractionBridgeService::EnsureInputReady()
{
    if (GetActiveDataSource() != OrthogonalCropDataSource::Auto) {
        return true;
    }

    // 只有在 router 仍找不到活跃输入时，才尝试从 data manager 兜底补 image。
    if (!GetInputImage() && m_dataMgr) {
        SetInputImage(m_dataMgr->GetVtkImage());
    }

    return GetActiveDataSource() != OrthogonalCropDataSource::Auto;
}

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetDefaultInteractiveBounds() const
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

void OrthogonalCropInteractionBridgeService::HandleWidgetBoundsChanged(
    const std::array<double, 6>& bounds,
    CropInteractionPhase phase)
{
    if (!m_cropInteractionEnabled) {
        return;
    }

    // 交互过程中始终记录最新 bounds，但只在 Released 时同步 preview，
    // 保持“拖拽只更新轻量 UI，预览结果在释放后统一刷新”的既有流程。
    m_currentBounds = bounds;
    m_boundsInitialized = true;
    m_lastInteractionPhase = phase;
    if (m_previewEnabled && phase == CropInteractionPhase::Released) {
        UpdatePreviewFromCurrentBounds(true);
    }
}

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetTransformedBounds(
    const std::array<double, 6>& sourceBounds,
    bool modelToWorld) const
{
    if (!m_referenceRenderService) {
        return sourceBounds;
    }

    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());
    if (!modelToWorld) {
        matrix->Invert();
    }

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(matrix);

    vtkBoundingBox transformedBounds;
    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                const double corner[3] = {
                    sourceBounds[ix == 0 ? 0 : 1],
                    sourceBounds[iy == 0 ? 2 : 3],
                    sourceBounds[iz == 0 ? 4 : 5]
                };
                double transformedPoint[3] = { 0.0, 0.0, 0.0 };
                transform->TransformPoint(corner, transformedPoint);
                transformedBounds.AddPoint(transformedPoint);
            }
        }
    }

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    transformedBounds.GetBounds(bounds);
    return { bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5] };
}

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetActiveWorldBounds() const
{
    const auto modelBounds = GetActiveInputBounds();
    if (!(modelBounds[0] < modelBounds[1]
        && modelBounds[2] < modelBounds[3]
        && modelBounds[4] < modelBounds[5])) {
        return modelBounds;
    }

    return GetTransformedBounds(modelBounds, true);
}

std::array<double, 16> OrthogonalCropInteractionBridgeService::GetWorldToModelMatrix() const
{
    if (!m_referenceRenderService) {
        return GetIdentityMatrixArray();
    }

    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());
    matrix->Invert();

    std::array<double, 16> matrixData = { 0.0 };
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            matrixData[row * 4 + col] = matrix->GetElement(row, col);
        }
    }
    return matrixData;
}

OrthogonalCropRequest OrthogonalCropInteractionBridgeService::BuildPreviewRequest() const
{
    auto previewRequest = GetDefaultRequest();

    // widget 持有的是世界坐标轴对齐盒；preview request 始终折叠成
    // LocalCenterAndDimensions + worldToModelMatrix，这样 image / polydata 两条后端都能复用同一套盒定义。
    previewRequest.SetLocalCenterAndDimensions(
        {
            (m_currentBounds[0] + m_currentBounds[1]) * 0.5,
            (m_currentBounds[2] + m_currentBounds[3]) * 0.5,
            (m_currentBounds[4] + m_currentBounds[5]) * 0.5
        },
        {
            m_currentBounds[1] - m_currentBounds[0],
            m_currentBounds[3] - m_currentBounds[2],
            m_currentBounds[5] - m_currentBounds[4]
        },
        GetWorldToModelMatrix());
    previewRequest.SetExecutionMode(CropExecutionMode::VirtualCrop);
    previewRequest.SetRemovalMode(m_currentRemovalMode);

    auto cropState = previewRequest.GetCropStateModel();
    cropState.SetCropEnabled(m_cropInteractionEnabled);
    cropState.SetInteractionPhase(m_lastInteractionPhase);
    previewRequest.SetCropStateModel(cropState);
    return previewRequest;
}

void OrthogonalCropInteractionBridgeService::UpdatePreviewFromCurrentBounds(bool logStats)
{
    if (!m_boundsInitialized) {
        return;
    }

    // 预览刷新主流程固定为：当前 bounds -> request -> backend result -> overlay/3D preview。
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

    const bool main3DPreviewApplied = SetPreviewServicesDirty(previewResult);

    if (logStats) {
        // 统计日志优先使用 result 中的 resolved 信息；若上层未回填，则退回 statistics 字段。
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
            << ", main3D = "
            << (main3DPreviewApplied ? "PolyDataClip" : "OverlayOnly")
            << ", bounds = ["
            << m_currentBounds[0] << ", " << m_currentBounds[1] << "; "
            << m_currentBounds[2] << ", " << m_currentBounds[3] << "; "
            << m_currentBounds[4] << ", " << m_currentBounds[5] << "]"
            << std::endl;
    }
}

void OrthogonalCropInteractionBridgeService::TogglePreview(CropRemovalMode removalMode, bool logStats)
{
    if (!m_cropInteractionEnabled) {
        return;
    }

    if (m_previewEnabled && m_currentRemovalMode == removalMode) {
        // 再次触发同一模式表示关闭 preview，并恢复原始显示内容。
        m_previewEnabled = false;
        RestorePreviewRenderTargets();
        if (logStats) {
            std::cout << "[Main] Orthogonal crop preview restored full model." << std::endl;
        }
        return;
    }

    m_previewEnabled = true;
    m_currentRemovalMode = removalMode;
    if (logStats) {
        std::cout << "[Main] Orthogonal crop preview mode: "
            << GetRemovalModeText(m_currentRemovalMode)
            << std::endl;
    }

    if (m_cropInteractionEnabled
        && m_lastInteractionPhase != CropInteractionPhase::Dragging) {
        // 非拖拽阶段才立即刷新；拖拽中依旧等待 EndInteractionEvent 统一触发。
        UpdatePreviewFromCurrentBounds(logStats);
    }
}

vtkSmartPointer<vtkPlanes> OrthogonalCropInteractionBridgeService::GetCurrentWidgetPlanesForModelInput() const
{
    auto worldPlanes = vtkSmartPointer<vtkPlanes>::New();
    if (!m_widgetStateController.GetPlanes(worldPlanes)) {
        return nullptr;
    }

    if (m_referenceRenderService) {
        // vtkPlanes 内部 transform 会在求值时把模型点带到 widget 世界空间，
        // 这样 3D 主模型 polydata clip 可以直接使用模型坐标输入。
        auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
        matrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());

        auto modelToWorldTransform = vtkSmartPointer<vtkTransform>::New();
        modelToWorldTransform->SetMatrix(matrix);
        worldPlanes->SetTransform(modelToWorldTransform);
    }

    return worldPlanes;
}

const char* OrthogonalCropInteractionBridgeService::GetFailureReasonText(OrthogonalCropFailureReason failureReason)
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

const char* OrthogonalCropInteractionBridgeService::GetRemovalModeText(CropRemovalMode removalMode)
{
    switch (removalMode) {
    case CropRemovalMode::KeepInside:
        return "KeepInside";
    case CropRemovalMode::RemoveInside:
        return "RemoveInside";
    }

    return "Unknown";
}

const char* OrthogonalCropInteractionBridgeService::GetDataSourceText(OrthogonalCropDataSource dataSource)
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

const char* OrthogonalCropInteractionBridgeService::GetResolvedBackendText(OrthogonalCropResolvedBackend backend)
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

std::shared_ptr<AbstractInteractiveService> OrthogonalCropInteractionBridgeService::GetFirstPreviewRenderService() const
{
    for (const auto& target : m_previewRenderTargets) {
        if (target.service) {
            return target.service;
        }
    }
    return nullptr;
}

void OrthogonalCropInteractionBridgeService::ClearPreviewRenderTargets()
{
    // 更新 preview 目标列表前，先把旧 overlay 从旧窗口解绑，避免残留 prop。
    for (const auto& target : m_previewRenderTargets) {
        if (target.service && target.overlayStrategy) {
            target.service->SetOverlayStrategyRemoved(target.overlayStrategy);
        }
    }
    m_previewRenderTargets.clear();
}

void OrthogonalCropInteractionBridgeService::RestorePreviewRenderTargets()
{
    // 退出 preview 时同时恢复 overlay 和 3D 主模型 mapper 输入。
    for (auto& target : m_previewRenderTargets) {
        if (target.overlayStrategy) {
            target.overlayStrategy->ClearPreview();
        }
        RestoreMainPolyDataPreview(target);
        if (target.service) {
            target.service->SetDirtyMarked();
        }
    }
}

void OrthogonalCropInteractionBridgeService::AddPreviewRenderService(const std::shared_ptr<AbstractInteractiveService>& service)
{
    if (!service) {
        return;
    }

    const auto sameTarget = std::find_if(
        m_previewRenderTargets.begin(),
        m_previewRenderTargets.end(),
        [service](const PreviewRenderTarget& target) {
            return target.service == service;
        });
    if (sameTarget != m_previewRenderTargets.end()) {
        return;
    }

    auto overlayStrategy = std::make_shared<OrthogonalCropPreviewOverlayStrategy>();
    service->SetOverlayStrategyAdded(overlayStrategy);
    m_previewRenderTargets.push_back({ service, overlayStrategy });
}

bool OrthogonalCropInteractionBridgeService::SetPreviewServicesDirty(const OrthogonalCropResult& previewResult)
{
    bool main3DPreviewApplied = false;

    // 同一份 previewResult 同时喂给所有目标窗口，保证多窗口显示的一致性。
    for (auto& target : m_previewRenderTargets) {
        if (target.service && target.overlayStrategy) {
            target.overlayStrategy->SetSliceAxis(target.service->GetNavigationAxis());
            target.overlayStrategy->SetRemovalMode(m_currentRemovalMode);
            target.overlayStrategy->SetCropResult(previewResult);
            main3DPreviewApplied = SetMainPolyDataPreviewApplied(target, previewResult) || main3DPreviewApplied;
            target.service->SetDirtyMarked();
        }
    }
    return main3DPreviewApplied;
}

void OrthogonalCropInteractionBridgeService::RestoreMainPolyDataPreview(PreviewRenderTarget& target)
{
    if (!target.mainPreviewMapper || !target.mainPreviewSourcePolyData) {
        return;
    }

    target.mainPreviewMapper->SetInputData(target.mainPreviewSourcePolyData);
    target.mainPreviewMapper->Update();

    if (target.service) {
        auto actor = vtkActor::SafeDownCast(target.service->GetMainProp());
        if (actor) {
            actor->Modified();
        }
    }
}

bool OrthogonalCropInteractionBridgeService::SetMainPolyDataPreviewApplied(
    PreviewRenderTarget& target,
    const OrthogonalCropResult& previewResult)
{
    if (!target.service || target.service->GetNavigationAxis() >= 0 || !previewResult.GetSucceeded()) {
        return false;
    }

    auto actor = vtkActor::SafeDownCast(target.service->GetMainProp());
    if (!actor) {
        return false;
    }

    auto mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
    if (!mapper) {
        return false;
    }

    if (target.mainPreviewMapper != mapper || !target.mainPreviewSourcePolyData) {
        // 第一次进入该 3D 窗口 preview 时缓存原始主模型输入，后续退出时直接恢复。
        mapper->Update();
        auto source = mapper->GetInput();
        if (!source || source->GetNumberOfPoints() == 0) {
            return false;
        }

        target.mainPreviewMapper = mapper;
        target.mainPreviewSourcePolyData = vtkSmartPointer<vtkPolyData>::New();
        target.mainPreviewSourcePolyData->ShallowCopy(source);
    }

    auto clipPlanes = GetCurrentWidgetPlanesForModelInput();
    if (!clipPlanes) {
        return false;
    }

    // 3D 主模型预览始终基于原始 polydata + 当前 widget planes 重新 clip，
    // 避免在连续切换 preview 时叠加裁切误差。
    auto clipped = OrthogonalCropBackendRouterService::GetClippedPolyData(
        target.mainPreviewSourcePolyData,
        clipPlanes,
        m_currentRemovalMode);
    if (!clipped) {
        return false;
    }

    mapper->SetInputData(clipped);
    mapper->Update();
    actor->Modified();
    return true;
}
