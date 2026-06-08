// =====================================================================
// Path: MVVCVTK/src/Service/OrthogonalCrop/OrthogonalCropInteractionBridgeService.cpp
// 分类: Service / Interaction Bridge Implementation
// 说明: 维护交互态、构造 preview request、把结果分发给 overlay 与 3D 主模型预览。
// =====================================================================

#include "OrthogonalCrop/OrthogonalCropInteractionBridgeService.h"
#include "OrthogonalCrop/OrthogonalCropAlgorithm.h"

#include <vtkActor.h>
#include <vtkBoundingBox.h>
#include <vtkBox.h>
#include <vtkGeometryFilter.h>
#include <vtkMatrix4x4.h>
#include <vtkPlanes.h>
#include <vtkTableBasedClipDataSet.h>
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

// ═══ 输入转发 ═══
// 以下接口把输入参数透传给 backend router，bridge 自身不做额外处理
void OrthogonalCropInteractionBridgeService::SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    m_backend.SetInputImage(std::move(image));
}

vtkSmartPointer<vtkImageData> OrthogonalCropInteractionBridgeService::GetInputImage() const
{
    return m_backend.GetInputImage();
}

void OrthogonalCropInteractionBridgeService::SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    m_backend.SetInputPolyData(std::move(polyData));
}

vtkSmartPointer<vtkPolyData> OrthogonalCropInteractionBridgeService::GetInputPolyData() const
{
    return m_backend.GetInputPolyData();
}

void OrthogonalCropInteractionBridgeService::SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    m_backend.SetPreferredDataSource(dataSource);
}

OrthogonalCropDataSource OrthogonalCropInteractionBridgeService::GetActiveDataSource() const
{
    return m_backend.GetActiveDataSource();
}

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetActiveInputBounds() const
{
    return m_backend.GetActiveInputBounds();
}

OrthogonalCropRequest OrthogonalCropInteractionBridgeService::GetDefaultRequest() const
{
    return m_backend.GetDefaultRequest();
}

OrthogonalCropStatistics OrthogonalCropInteractionBridgeService::GetStatistics(const OrthogonalCropRequest& request) const
{
    return m_backend.GetStatistics(request);
}

OrthogonalCropResult OrthogonalCropInteractionBridgeService::GetResult(const OrthogonalCropRequest& request) const
{
    return m_backend.GetResult(request);
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

void OrthogonalCropInteractionBridgeService::SetPreviewRequiresFullArtifacts(bool required)
{
    m_previewRequiresFullArtifacts = required;
}

void OrthogonalCropInteractionBridgeService::TogglePreviewArtifactMode(bool logStats)
{
    m_previewRequiresFullArtifacts = !m_previewRequiresFullArtifacts;
    if (logStats) {
        std::cout << "[Main] Orthogonal crop preview artifact mode: "
            << (m_previewRequiresFullArtifacts ? "Full" : "Lightweight")
            << std::endl;
    }

    if (m_previewEnabled
        && m_cropInteractionEnabled
        && m_lastInteractionPhase != CropInteractionPhase::Dragging) {
        UpdatePreviewFromCurrentBounds(logStats);
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
        << ". Press 1 to toggle inside preview, press 2 to toggle outside preview, press 3 to toggle lightweight/full preview; press O or Esc to exit." << std::endl;
    return true;
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

bool OrthogonalCropInteractionBridgeService::EnsureInputReady()
{
    // bridge 只保证“当前至少有一个可用后端输入”。
    // 它不在这里做任何坐标折叠；只有在 Auto 模式下还找不到活跃输入时，才从 data manager 兜底补 image。
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
    // 默认交互盒完全在世界坐标下构造：
    // 1. 先取当前活跃输入提升到世界坐标后的完整 bounds
    // 2. 再按经验比例缩小，得到首次拖拽更容易观察的起始盒
    // 3. 这只盒子只服务 widget；真正执行时还会折回后端输入坐标系
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

    // 这组比例不是算法约束，而是交互层的默认起始盒经验值：
    // 初始盒要足够小，便于用户第一下就看到明显裁切效果，同时保持三个轴都有可拖拽余量。

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

    // 交互过程中始终记录最新世界坐标 bounds 和 phase，
    // 但只在 Released 时触发 preview，保持“拖拽只更新轻量 UI，释放后再统一跑后端”的既有流程。
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
    // 这是纯 bounds 级别的 8 角点变换 helper：
    // 输入和输出都仍然是轴对齐盒，只是根据方向选择 model->world 或其逆矩阵。
    // 显式变换 8 个角点后再回收包围盒，可以避免旋转场景下的逐轴换算失真。
    if (!m_referenceRenderService) {
        return sourceBounds;
    }

    auto sourceToTargetMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    sourceToTargetMatrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());
    if (!modelToWorld) {
        sourceToTargetMatrix->Invert();
    }

    auto sourceToTargetTransform = vtkSmartPointer<vtkTransform>::New();
    sourceToTargetTransform->SetMatrix(sourceToTargetMatrix);

    vtkBoundingBox transformedBounds;
    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                const double corner[3] = {
                    sourceBounds[ix == 0 ? 0 : 1],
                    sourceBounds[iy == 0 ? 2 : 3],
                    sourceBounds[iz == 0 ? 4 : 5]
                };
                double sourceToTargetPoint[3] = { 0.0, 0.0, 0.0 };
                sourceToTargetTransform->TransformPoint(corner, sourceToTargetPoint);
                transformedBounds.AddPoint(sourceToTargetPoint);
            }
        }
    }

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    transformedBounds.GetBounds(bounds);
    return { bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5] };
}

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetActiveWorldBounds() const
{
    // backend/router 暴露的是后端输入坐标系 bounds；
    // widget 只工作在世界坐标里，所以这里负责做一次 model -> world 的包围盒提升。
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

    // 参考窗口维护的是 model->world 矩阵；裁切 request 需要反向的 world->model，
    // 否则 widget 盒和后端输入会落在两个不同坐标系里。
    // 对 image 路径，这里的 model 实际就是 vtkImageData 的 physical 空间；
    // 对 polydata 路径，则是主模型自己的输入坐标系。
    auto worldToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToModelMatrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());
    worldToModelMatrix->Invert();

    std::array<double, 16> matrixData = { 0.0 };
    vtkMatrix4x4::DeepCopy(matrixData.data(), worldToModelMatrix);
    return matrixData;
}

OrthogonalCropRequest OrthogonalCropInteractionBridgeService::BuildPreviewRequest() const
{
    // ── 分支 ①：获取默认 request 模板 ──
    // Image 路径 → PluginService 根据 inputImage bounds 构造
    // PolyData 路径 → Router 根据 activeInput bounds 构造
    auto previewRequest = GetDefaultRequest();

    // ── 分支 ②：Widget 世界 box → LocalCenterAndDimensions 编码 ──
    // center = widget 世界坐标盒中心
    // dimensions = widget 世界坐标盒尺寸
    // localToInputMatrix = GetWorldToModelMatrix()（参考渲染服务 modelMatrix 的逆）
    // 下游 Algorithm 收到后用 world→model 矩阵还原到后端输入坐标系
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

    // cropState 不参与几何计算，但会跟随结果回到 overlay / 上层日志，
    // 让调用方知道这次 preview 到底是拖拽释放触发，还是模式切换触发。
    // 结果说明：这个 request 仍然只是 preview 请求，不会直接要求 physical 导出。
    previewRequest.SetCropStateModel(cropState);
    return previewRequest;
}

void OrthogonalCropInteractionBridgeService::UpdatePreviewFromCurrentBounds(bool logStats)
{
    if (!m_boundsInitialized) {
        return;
    }

    // ── 步骤 1：构建 preview request ──
    // widget 世界坐标盒 → LocalCenterAndDimensions 编码
    // 固定 VirtualCrop 模式 + 当前 removalMode
    const auto previewRequest = BuildPreviewRequest();

    // ── 分支 A：轻量预览（不触发完整 mask 管道） ──
    // 只做 cropData 归一化 + outline 生成，适合频繁拖拽场景
    if (!m_previewRequiresFullArtifacts) {
        CropDataModel cropData;
        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
        std::string message;
        if (!OrthogonalCropAlgorithm::GetCropDataModel(
                GetActiveInputBounds(),
                previewRequest,
                cropData,
                failureReason,
                message,
                true)) {
            if (logStats) {
                std::cerr << "[Main] Orthogonal crop preview failed: "
                    << GetFailureReasonText(failureReason)
                    << " - " << message << std::endl;
            }
            return;
        }

        // ── 手动构造轻量结果 ──
        OrthogonalCropResult previewResult;
        previewResult.SetResolvedDataSource(GetActiveDataSource());
        previewResult.SetSucceeded(true);
        previewResult.SetFailureReason(OrthogonalCropFailureReason::None);
        previewResult.SetCropDataModel(cropData);
        previewResult.SetCropStateModel(previewRequest.GetCropStateModel());
        previewResult.SetOutlinePolyData(OrthogonalCropAlgorithm::GetOutlinePolyData(cropData));

        // ── 结果分发 ──
        const bool main3DPreviewApplied = SetPreviewServicesDirty(previewResult);
        if (logStats) {
            std::cout
                << "[Main] Orthogonal crop preview updated. source = "
                << GetDataSourceText(previewResult.GetResolvedDataSource())
                << ", backend = LightweightPreview"
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
        return;
    }

    // ── 分支 B：完整预览 ──
    // 经 Router → PluginService/Algorithm 完整执行 mask 或 polyData clip
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

    // ── 步骤 2：结果分发（两条分支共享） ──
    // 对每个 PreviewRenderTarget：
    // ① SetMainPolyDataPreviewApplied → 3D 主窗口常驻 clip 管道更新
    //    3D 主窗口（axis<0）若成功接管，剥离 derivedPolyData overlay 避免重复绘制
    // ② overlayStrategy->SetCropResult → outline / mask / polydata 三类可视内容
    // ③ target.service->SetDirtyMarked → 触发渲染刷新
    const bool main3DPreviewApplied = SetPreviewServicesDirty(previewResult);

    // 这里的 3D 主模型 clip 只是临时预览表现；真正的几何/统计结果仍以 previewResult 为准。
    // 因此关闭 preview 时只需要把管道切回 pass-through 状态，而不是重新向后端求一份“恢复结果”。

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
    // Toggle 语义固定为：
    // - 同模式再次触发：关闭 preview，并恢复 full model 显示
    // - 切到新模式：更新 removal mode；若当前不在 dragging，则立即刷新一次 preview
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
    // widget controller 提供的是世界坐标平面；
    // 这里给 vtkPlanes 挂上 modelToWorld transform，让 clip 在“评估模型点时”自动折叠到 widget 世界空间。
    // 结果语义是：调用方可以直接拿这组 planes 去裁模型坐标下的 polydata，而不用先手动变换点。
    auto worldPlanes = vtkSmartPointer<vtkPlanes>::New();
    if (!m_widgetStateController.GetPlanes(worldPlanes)) {
        return nullptr;
    }

    if (m_referenceRenderService) {
        // vtkPlanes 内部 transform 会在求值时把模型点带到 widget 世界空间，
        // 这样 3D 主模型 polydata clip 可以直接使用模型坐标输入。
        auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
        modelToWorldMatrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());

        auto modelToWorldTransform = vtkSmartPointer<vtkTransform>::New();
        modelToWorldTransform->SetMatrix(modelToWorldMatrix);
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
    // 清理流程只处理 overlay 生命周期：
    // 1. 先把旧 overlay 从旧窗口解绑，避免残留 prop
    // 2. 再清空目标数组
    // 主模型 clip 管道的恢复由 RestorePreviewRenderTargets 单独收口。
    for (const auto& target : m_previewRenderTargets) {
        if (target.service && target.overlayStrategy) {
            target.service->SetOverlayStrategyRemoved(target.overlayStrategy);
        }
    }
    m_previewRenderTargets.clear();
}

void OrthogonalCropInteractionBridgeService::RestorePreviewRenderTargets()
{
    // preview 恢复流程：
    // 1. 清掉每个窗口的 overlay 内容
    // 2. 3D 主窗口把 clip function 切回全量直通盒
    // 3. 最后统一标脏，交给渲染阶段按需拉起
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

    // 每个窗口只允许挂一份 overlay strategy；重复 service 直接跳过，避免重复 prop 和重复设脏。
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

    // 结果分发流程：
    // 1. 先尝试给 3D 主窗口更新常驻 clip 管道
    // 2. 再按目标窗口轴向准备 overlay 要消费的结果
    // 3. 如果 3D 主窗口已经接管主模型显示，就剥掉 derived polydata overlay，避免重复绘制
    for (auto& target : m_previewRenderTargets) {
        if (target.service && target.overlayStrategy) {
            const bool mainPreviewAppliedForTarget = SetMainPolyDataPreviewApplied(target, previewResult);

            auto overlayResult = previewResult;
            if (mainPreviewAppliedForTarget && target.service->GetNavigationAxis() < 0) {
                // 3D 主窗口已经由常驻 clip 管道直接表现裁切结果，
                // 不再额外叠一份 clipped polydata overlay，避免重复绘制同一套网格。
                overlayResult.SetDerivedPolyData(nullptr);
            }

            target.overlayStrategy->SetSliceAxis(target.service->GetNavigationAxis());
            target.overlayStrategy->SetRemovalMode(m_currentRemovalMode);
            target.overlayStrategy->SetCropResult(overlayResult);
            main3DPreviewApplied = mainPreviewAppliedForTarget || main3DPreviewApplied;
            target.service->SetDirtyMarked();
        }
    }
    return main3DPreviewApplied;
}

void OrthogonalCropInteractionBridgeService::RestoreMainPolyDataPreview(PreviewRenderTarget& target)
{
    // 3D 主窗口并不切回原始 mapper 输入，
    // 而是把 clip function 切回一只完全包住原模型的 pass-through box。
    // 这样 mapper 始终挂在同一条流式管道上，关闭 preview 只会让输出退化成原模型。
    if (!target.mainPreviewClipFilter || !target.mainPreviewPassThroughBox) {
        return;
    }

    target.mainPreviewClipFilter->SetClipFunction(target.mainPreviewPassThroughBox);
    target.mainPreviewClipFilter->InsideOutOn();

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

    // 这里只有 3D 主窗口会命中；2D slice 窗口只消费 overlay，不接管主 mapper。
    // 应用流程固定为：
    // 1. 首次进入时抓一份稳定 source polydata，作为常驻 preview 管道输入
    // 2. 建 clip -> geometry 持久管道，并准备 pass-through box 作为“全量直通”状态
    // 3. 后续每次刷新只更新 widget planes 和 InsideOut 语义，不再反复切换 mapper 输入

    auto actor = vtkActor::SafeDownCast(target.service->GetMainProp());
    if (!actor) {
        return false;
    }

    auto mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
    if (!mapper) {
        return false;
    }

    if (target.mainPreviewMapper != mapper || !target.mainPreviewSourcePolyData) {
        // 首次进入时先尝试直接抓 mapper 当前输入；
        // 只有当上游还没物化成稳定 polydata 时，才退回一次 Update() 取当前输出快照。
        auto source = mapper->GetInput();
        if (!source || source->GetNumberOfPoints() == 0) {
            mapper->Update();
            source = mapper->GetInput();
        }
        if (!source || source->GetNumberOfPoints() == 0) {
            return false;
        }

        target.mainPreviewMapper = mapper;
        target.mainPreviewSourcePolyData = vtkSmartPointer<vtkPolyData>::New();
        target.mainPreviewSourcePolyData->ShallowCopy(source);
        target.mainPreviewClipFilter = vtkSmartPointer<vtkTableBasedClipDataSet>::New();
        target.mainPreviewClipFilter->SetInputData(target.mainPreviewSourcePolyData);
        target.mainPreviewGeometryFilter = vtkSmartPointer<vtkGeometryFilter>::New();
        target.mainPreviewGeometryFilter->SetInputConnection(target.mainPreviewClipFilter->GetOutputPort());

        // pass-through box 需要完整包住原模型输入，
        // 这样关闭 preview 时只切 clip function 就能恢复“显示全量模型”的结果。
        double sourceBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        target.mainPreviewSourcePolyData->GetBounds(sourceBounds);
        const double xSpan = sourceBounds[1] - sourceBounds[0];
        const double ySpan = sourceBounds[3] - sourceBounds[2];
        const double zSpan = sourceBounds[5] - sourceBounds[4];
        const double maxSpan = std::max({ xSpan, ySpan, zSpan, 1.0 });
        const double padding = std::max(maxSpan * 1e-6, 1e-6);

        target.mainPreviewPassThroughBox = vtkSmartPointer<vtkBox>::New();
        target.mainPreviewPassThroughBox->SetBounds(
            sourceBounds[0] - padding, sourceBounds[1] + padding,
            sourceBounds[2] - padding, sourceBounds[3] + padding,
            sourceBounds[4] - padding, sourceBounds[5] + padding);

        mapper->SetInputConnection(target.mainPreviewGeometryFilter->GetOutputPort());
    }

    auto clipPlanes = GetCurrentWidgetPlanesForModelInput();
    if (!clipPlanes || !target.mainPreviewClipFilter || !target.mainPreviewGeometryFilter) {
        return false;
    }

    // 这里每次只更新当前 widget planes 和 removal mode，
    // 持久管道本身不重建、不换输入，从而把卡顿点压缩到真正的 clip 执行上。
    // 结果说明：主窗口看到的是主模型本体被 clip 后的结果，而不是额外叠加的一份 overlay 网格。
    target.mainPreviewClipFilter->SetClipFunction(clipPlanes);
    if (m_currentRemovalMode == CropRemovalMode::KeepInside) {
        target.mainPreviewClipFilter->InsideOutOn();
    }
    else {
        target.mainPreviewClipFilter->InsideOutOff();
    }

    actor->Modified();
    return true;
}
