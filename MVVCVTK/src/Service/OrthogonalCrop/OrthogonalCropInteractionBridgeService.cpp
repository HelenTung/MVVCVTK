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
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>
#include <vtkShaderProperty.h>
#include <vtkTableBasedClipDataSet.h>
#include <vtkTransform.h>
#include <vtkUniforms.h>
#include <vtkVolume.h>
#include <vtkVolumeMapper.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

namespace {
constexpr const char* kVolumeRemoveInsideEnabledUniform = "mvvcvtk_volumeRemoveInsideEnabled";
constexpr const char* kVolumeRemoveInsideInputToBoxUniform = "mvvcvtk_volumeRemoveInsideInputToBox";
constexpr const char* kVolumeRemoveInsideBaseImplTag = "//VTK::Base::Impl";

constexpr const char* kVolumeRemoveInsideBaseImplReplacement =
    "//VTK::Base::Impl\n"
    "    if (!g_skip && mvvcvtk_volumeRemoveInsideEnabled != 0)\n"
    "      {\n"
    "      vec4 mvvcvtk_inputPoint = in_textureDatasetMatrix[0] * vec4(g_dataPos, 1.0);\n"
    "      float mvvcvtk_inputInvW = abs(mvvcvtk_inputPoint.w) > 1e-6 ? 1.0 / mvvcvtk_inputPoint.w : 1.0;\n"
    "      mvvcvtk_inputPoint = vec4(mvvcvtk_inputPoint.xyz * mvvcvtk_inputInvW, 1.0);\n"
    "      vec4 mvvcvtk_boxPoint4 = mvvcvtk_volumeRemoveInsideInputToBox * mvvcvtk_inputPoint;\n"
    "      float mvvcvtk_boxInvW = abs(mvvcvtk_boxPoint4.w) > 1e-6 ? 1.0 / mvvcvtk_boxPoint4.w : 1.0;\n"
    "      vec3 mvvcvtk_boxPoint = mvvcvtk_boxPoint4.xyz * mvvcvtk_boxInvW;\n"
    "      if (all(lessThanEqual(abs(mvvcvtk_boxPoint), vec3(1.0))))\n"
    "        {\n"
    "        g_skip = true;\n"
    "        }\n"
    "      }\n";
}

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
    if (!logStats) {
        if (!m_cropInteractionEnabled || !m_boundsInitialized) {
            std::cerr << "[Main] Orthogonal crop physical crop failed: crop widget is not active." << std::endl;
            return;
        }

        if (m_lastInteractionPhase == CropInteractionPhase::Dragging) {
            std::cerr << "[Main] Orthogonal crop physical crop failed: wait until widget dragging finishes." << std::endl;
            return;
        }

        if (m_currentRemovalMode != CropRemovalMode::KeepInside) {
            std::cerr << "[Main] Orthogonal crop physical crop failed: RemoveInside preview does not support physical submit." << std::endl;
            return;
        }

        auto referenceRenderService = std::dynamic_pointer_cast<MedicalVizService>(m_referenceRenderService);
        if (!referenceRenderService) {
            std::cerr << "[Main] Orthogonal crop physical crop failed: reference render service does not support reload." << std::endl;
            return;
        }

        auto submitRequest = BuildPreviewRequest();
        submitRequest.SetExecutionMode(CropExecutionMode::PhysicalCrop);

        const auto submitResult = GetResult(submitRequest);
        if (submitResult.GetFailureReason() != OrthogonalCropFailureReason::None || !submitResult.GetSucceeded()) {
            std::cerr << "[Main] Orthogonal crop physical crop failed: "
                << GetFailureReasonText(submitResult.GetFailureReason())
                << " - " << submitResult.GetMessage() << std::endl;
            return;
        }

        auto derivedImage = submitResult.GetDerivedImage();
        if (!derivedImage) {
            std::cerr << "[Main] Orthogonal crop physical crop failed: derived image is null." << std::endl;
            return;
        }

        int dims[3] = { 0, 0, 0 };
        double spacingData[3] = { 1.0, 1.0, 1.0 };
        double rasOriginData[3] = { 0.0, 0.0, 0.0 };
        derivedImage->GetDimensions(dims);
        derivedImage->GetSpacing(spacingData);
        derivedImage->GetOrigin(rasOriginData);
        auto sourceData = static_cast<const float*>(derivedImage->GetScalarPointer());
        if (!sourceData || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
            std::cerr << "[Main] Orthogonal crop physical crop failed: derived image buffer is invalid." << std::endl;
            return;
        }

        const size_t nx = static_cast<size_t>(dims[0]);
        const size_t ny = static_cast<size_t>(dims[1]);
        const size_t nz = static_cast<size_t>(dims[2]);
        const size_t sliceSize = nx * ny;
        const size_t totalSize = sliceSize * nz;
        m_reloadBuffer = std::make_shared<std::vector<float>>(totalSize, 0.0f);

        auto* reloadData = m_reloadBuffer->data();
        for (size_t z = 0; z < nz; ++z) {
            const size_t sourceSliceOffset = z * sliceSize;
            const size_t targetSliceOffset = z * sliceSize;
            for (size_t y = 0; y < ny; ++y) {
                const float* sourceRow = sourceData + sourceSliceOffset + y * nx;
                float* targetRow = reloadData + targetSliceOffset + (ny - 1 - y) * nx;
                for (size_t x = 0; x < nx; ++x) {
                    targetRow[nx - 1 - x] = sourceRow[x];
                }
            }
        }

        const std::array<int, 3> reloadDims = { dims[0], dims[1], dims[2] };
        const std::array<float, 3> reloadSpacing = {
            static_cast<float>(spacingData[0]),
            static_cast<float>(spacingData[1]),
            static_cast<float>(spacingData[2])
        };
        const std::array<float, 3> reloadOrigin = {
            static_cast<float>(-rasOriginData[0] - (static_cast<double>(dims[0] - 1) * spacingData[0])),
            static_cast<float>(-rasOriginData[1] - (static_cast<double>(dims[1] - 1) * spacingData[1])),
            static_cast<float>(rasOriginData[2])
        };

        if (!referenceRenderService->SetReloadFromBufferAsync(
                m_reloadBuffer->data(),
                reloadDims,
                reloadSpacing,
                reloadOrigin,
                [this, referenceRenderService](bool success) {
                    if (!success) {
                        std::cerr << "[Main] Orthogonal crop physical crop reload failed." << std::endl;
                        m_reloadBuffer.reset();
                        return;
                    }

                    if (m_dataMgr) {
                        SetInputImage(m_dataMgr->GetVtkImage());
                    }

                    if (m_dataMgr) {
                        const auto range = m_dataMgr->GetScalarRange();
                        referenceRenderService->SetIsoThreshold(range[0] + (range[1] - range[0]) * 0.55);
                    }

                    std::cout << "[Main] Orthogonal crop physical crop applied to main image data." << std::endl;
                    m_reloadBuffer.reset();
                })) {
            std::cerr << "[Main] Orthogonal crop physical crop failed: reload request was rejected." << std::endl;
            m_reloadBuffer.reset();
            return;
        }

        SetInputImage(nullptr);
        DeactivateInteractiveCrop();
        return;
    }

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
        << ". Press 1 to toggle inside preview, press 2 to toggle outside preview, press 3 to toggle lightweight/full preview, press Ctrl+3 to apply physical crop; press O or Esc to exit." << std::endl;
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

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetModelBoundsAsWorldBounds(
    const std::array<double, 6>& modelBounds) const
{
    // 这是纯 bounds 级别的 8 角点变换 helper：
    // 输入是后端模型/输入坐标系轴对齐盒，输出是 widget 使用的世界坐标轴对齐盒。
    // 显式变换 8 个角点后再回收包围盒，可以避免旋转场景下的逐轴换算失真。
    if (!m_referenceRenderService) {
        return modelBounds;
    }

    auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    modelToWorldMatrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());

    auto modelToWorldTransform = vtkSmartPointer<vtkTransform>::New();
    modelToWorldTransform->SetMatrix(modelToWorldMatrix);

    vtkBoundingBox worldBounds;
    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                const double modelCorner[3] = {
                    modelBounds[ix == 0 ? 0 : 1],
                    modelBounds[iy == 0 ? 2 : 3],
                    modelBounds[iz == 0 ? 4 : 5]
                };
                double worldCorner[3] = { 0.0, 0.0, 0.0 };
                modelToWorldTransform->TransformPoint(modelCorner, worldCorner);
                worldBounds.AddPoint(worldCorner);
            }
        }
    }

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    worldBounds.GetBounds(bounds);
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

    return GetModelBoundsAsWorldBounds(modelBounds);
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

    std::array<double, 16> worldToModelMatrixData = { 0.0 };
    vtkMatrix4x4::DeepCopy(worldToModelMatrixData.data(), worldToModelMatrix);
    return worldToModelMatrixData;
}

OrthogonalCropRequest OrthogonalCropInteractionBridgeService::BuildPreviewRequest() const
{
    // ── 分支 ①：获取默认 request 模板 ──
    // Image 路径 → PluginService 根据 inputImage bounds 构造
    // PolyData 路径 → Router 根据 activeInput bounds 构造
    auto previewRequest = GetDefaultRequest();

    // ── 分支 ②：Widget 有向盒 → boxToInputMatrix 编码 ──
    // 标准盒 [-1,1]^3 先映射到 widget local 盒，再经 localToWorld 和 worldToModel 进入后端输入空间。
    CropVectorDouble3Array localCenter = {
        (m_currentBounds[0] + m_currentBounds[1]) * 0.5,
        (m_currentBounds[2] + m_currentBounds[3]) * 0.5,
        (m_currentBounds[4] + m_currentBounds[5]) * 0.5
    };
    CropVectorDouble3Array localDimensions = {
        m_currentBounds[1] - m_currentBounds[0],
        m_currentBounds[3] - m_currentBounds[2],
        m_currentBounds[5] - m_currentBounds[4]
    };
    CropMatrixDouble16Array boxToInputMatrixData = GetIdentityMatrixArray();
    CropMatrixDouble16Array localToWorldMatrixData = GetIdentityMatrixArray();
    m_widgetStateController.GetCurrentLocalBox(
        localCenter,
        localDimensions,
        localToWorldMatrixData);

    auto boxToLocalMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToLocalMatrix->Identity();
    for (int axis = 0; axis < 3; ++axis) {
        boxToLocalMatrix->SetElement(axis, axis, localDimensions[axis] * 0.5);
        boxToLocalMatrix->SetElement(axis, 3, localCenter[axis]);
    }

    auto localToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    localToWorldMatrix->DeepCopy(localToWorldMatrixData.data());

    auto boxToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(localToWorldMatrix, boxToLocalMatrix, boxToWorldMatrix);

    auto worldToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToModelMatrix->DeepCopy(GetWorldToModelMatrix().data());

    auto boxToInputMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(worldToModelMatrix, boxToWorldMatrix, boxToInputMatrix);
    vtkMatrix4x4::DeepCopy(boxToInputMatrixData.data(), boxToInputMatrix);

    previewRequest.SetBoxToInputMatrix(boxToInputMatrixData);
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
    // widget 有向盒 → boxToInputMatrix 编码
    // 固定 VirtualCrop 模式 + 当前 removalMode
    const auto previewRequest = BuildPreviewRequest();
    // ── 分支 A：轻量预览（不触发完整 mask/clip/统计管道） ──
    // Bridge 仍然只提交 request；source 分发与 request->cropData 归一化统一收口在 Router。
    if (!m_previewRequiresFullArtifacts) {
        const auto previewResult = m_backend.GetLightweightPreviewResult(previewRequest);
        if (previewResult.GetFailureReason() != OrthogonalCropFailureReason::None || !previewResult.GetSucceeded()) {
            if (logStats) {
                std::cerr << "[Main] Orthogonal crop preview failed: "
                    << GetFailureReasonText(previewResult.GetFailureReason())
                    << " - " << previewResult.GetMessage() << std::endl;
            }
            return;
        }

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
                << (main3DPreviewApplied ? "MainPreview" : "OverlayOnly")
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
    // ① SetMainVolumePreviewApplied / SetMainPolyDataPreviewApplied → 3D 主窗口常驻预览管道更新
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
            << (main3DPreviewApplied ? "MainPreview" : "OverlayOnly")
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
    // 2. 3D volume 主窗口清掉 shader discard / mask / clipping 状态
    // 3. 3D actor 主窗口把 clip function 切回全量直通盒
    // 4. 最后统一标脏，交给渲染阶段按需拉起
    for (auto& target : m_previewRenderTargets) {
        if (target.overlayStrategy) {
            target.overlayStrategy->ClearPreview();
        }

        if (target.service) {
            auto volume = vtkVolume::SafeDownCast(target.service->GetMainProp());
            auto volumeMapper = volume ? vtkVolumeMapper::SafeDownCast(volume->GetMapper()) : nullptr;
            if (volumeMapper) {
                volume->GetShaderProperty()->ClearFragmentShaderReplacement(kVolumeRemoveInsideBaseImplTag, true);
                volume->GetShaderProperty()->GetFragmentCustomUniforms()->SetUniformi(kVolumeRemoveInsideEnabledUniform, 0);
                if (auto gpuVolumeMapper = vtkGPUVolumeRayCastMapper::SafeDownCast(volumeMapper)) {
                    gpuVolumeMapper->SetMaskInput(nullptr);
                }
                volumeMapper->RemoveAllClippingPlanes();
                volumeMapper->CroppingOff();
                volumeMapper->SetCroppingRegionFlagsToSubVolume();
                volume->Modified();
            }
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
            bool mainPreviewAppliedForTarget = SetMainVolumePreviewApplied(target, previewResult);
            if (!mainPreviewAppliedForTarget) {
                mainPreviewAppliedForTarget = SetMainPolyDataPreviewApplied(target, previewResult);
            }

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

bool OrthogonalCropInteractionBridgeService::SetMainVolumePreviewApplied(
    PreviewRenderTarget& target,
    const OrthogonalCropResult& previewResult)
{
    if (!target.service || !previewResult.GetSucceeded()) {
        return false;
    }

    auto volume = vtkVolume::SafeDownCast(target.service->GetMainProp());
    auto volumeMapper = volume ? vtkVolumeMapper::SafeDownCast(volume->GetMapper()) : nullptr;
    if (!volumeMapper) {
        return false;
    }

    // 每次接管前先清空上一种 volume 后端表达，避免 KeepInside clipping
    // 与 RemoveInside shader discard / mask 在反复切换时互相残留。
    volume->GetShaderProperty()->ClearFragmentShaderReplacement(kVolumeRemoveInsideBaseImplTag, true);
    volume->GetShaderProperty()->GetFragmentCustomUniforms()->SetUniformi(kVolumeRemoveInsideEnabledUniform, 0);
    if (auto gpuVolumeMapper = vtkGPUVolumeRayCastMapper::SafeDownCast(volumeMapper)) {
        gpuVolumeMapper->SetMaskInput(nullptr);
    }
    volumeMapper->RemoveAllClippingPlanes();
    volumeMapper->CroppingOff();

    if (m_currentRemovalMode != CropRemovalMode::KeepInside) {
        auto gpuVolumeMapper = vtkGPUVolumeRayCastMapper::SafeDownCast(volumeMapper);
        if (!SetVolumeRemoveInsidePreviewApplied(volume, gpuVolumeMapper, previewResult)) {
            volumeMapper->SetCroppingRegionFlagsToSubVolume();
            volume->Modified();
            return false;
        }

        volume->Modified();
        return true;
    }

    SetVolumeKeepInsidePreviewApplied(volumeMapper, previewResult);
    volume->Modified();
    return true;
}

void OrthogonalCropInteractionBridgeService::SetVolumeKeepInsidePreviewApplied(
    vtkVolumeMapper* volumeMapper,
    const OrthogonalCropResult& previewResult) const
{
    if (!volumeMapper) {
        return;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    modelToWorldMatrix->Identity();
    if (m_referenceRenderService) {
        modelToWorldMatrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());
    }

    auto boxToInputMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInputMatrix->DeepCopy(cropData.GetBoxToInputMatrix().data());

    auto boxToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(modelToWorldMatrix, boxToInputMatrix, boxToWorldMatrix);

    auto boxToWorldTransform = vtkSmartPointer<vtkTransform>::New();
    boxToWorldTransform->SetMatrix(boxToWorldMatrix);

    auto clippingPlanes = vtkSmartPointer<vtkPlaneCollection>::New();
    for (int axis = 0; axis < 3; ++axis) {
        for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
            const double side = sideIndex == 0 ? -1.0 : 1.0;
            double boxOrigin[3] = { 0.0, 0.0, 0.0 };
            boxOrigin[axis] = side;

            double boxNormal[3] = { 0.0, 0.0, 0.0 };
            boxNormal[axis] = -side;

            double worldOrigin[3] = { 0.0, 0.0, 0.0 };
            double worldNormal[3] = { 0.0, 0.0, 0.0 };
            boxToWorldTransform->TransformPoint(boxOrigin, worldOrigin);
            boxToWorldTransform->TransformNormal(boxNormal, worldNormal);

            const double normalLength = std::sqrt(
                worldNormal[0] * worldNormal[0]
                + worldNormal[1] * worldNormal[1]
                + worldNormal[2] * worldNormal[2]);
            if (normalLength <= 1e-12) {
                continue;
            }

            auto plane = vtkSmartPointer<vtkPlane>::New();
            plane->SetOrigin(worldOrigin);
            plane->SetNormal(
                worldNormal[0] / normalLength,
                worldNormal[1] / normalLength,
                worldNormal[2] / normalLength);
            clippingPlanes->AddItem(plane);
        }
    }

    // KeepInside 和 RemoveInside 都只消费 previewResult.cropData；
    // 这里仅在 VTK volume clipping 需要的后端表达点，把 box/input 盒还原成 world planes。
    volumeMapper->SetClippingPlanes(clippingPlanes);
}

bool OrthogonalCropInteractionBridgeService::SetVolumeRemoveInsidePreviewApplied(
    vtkVolume* volume,
    vtkGPUVolumeRayCastMapper* volumeMapper,
    const OrthogonalCropResult& previewResult) const
{
    if (!volume || !volumeMapper) {
        return false;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto boxToInputMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInputMatrix->DeepCopy(cropData.GetBoxToInputMatrix().data());

    auto inputToBoxMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(boxToInputMatrix, inputToBoxMatrix);

    auto shaderProperty = volume->GetShaderProperty();
    shaderProperty->AddFragmentShaderReplacement(
        kVolumeRemoveInsideBaseImplTag,
        true,
        kVolumeRemoveInsideBaseImplReplacement,
        false);

    // 体渲染只消费 previewResult 中的 cropData；业务来源统一由 request -> cropData 决定。
    auto inputToBoxShaderMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    inputToBoxShaderMatrix->DeepCopy(inputToBoxMatrix);
    inputToBoxShaderMatrix->Transpose();

    // vtk volume shader 中 g_dataPos 是纹理坐标；in_textureDatasetMatrix[0] 才会把它还原到后端输入空间。
    // vtkUniforms 上传 vtkMatrix4x4 时按 OpenGL 列主序解释，所以自定义 inputToBox 需要先转置再交给 shader。
    // uniforms 的声明由 VTK 的 CustomUniforms::Dec 自动生成，不能再手写到 Base::Dec，否则会重复声明。
    auto fragmentUniforms = shaderProperty->GetFragmentCustomUniforms();
    fragmentUniforms->SetUniformMatrix(kVolumeRemoveInsideInputToBoxUniform, inputToBoxShaderMatrix);
    fragmentUniforms->SetUniformi(kVolumeRemoveInsideEnabledUniform, 1);

    volumeMapper->SetMaskInput(nullptr);
    shaderProperty->Modified();
    volumeMapper->Modified();
    volume->Modified();
    return true;
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
    // 3. 后续每次刷新只更新 cropData 派生的 clip function 和 InsideOut 语义，不再反复切换 mapper 输入

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

    if (!target.mainPreviewClipFilter || !target.mainPreviewGeometryFilter) {
        return false;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto clipFunction = vtkSmartPointer<vtkBox>::New();
    const auto canonicalBounds = GetCanonicalCropBoxBounds();
    clipFunction->SetBounds(
        canonicalBounds[0], canonicalBounds[1],
        canonicalBounds[2], canonicalBounds[3],
        canonicalBounds[4], canonicalBounds[5]);

    auto inputToBoxTransform = vtkSmartPointer<vtkTransform>::New();
    inputToBoxTransform->SetMatrix(cropData.GetBoxToInputMatrix().data());
    inputToBoxTransform->Inverse();
    clipFunction->SetTransform(inputToBoxTransform);

    // 这里每次只更新统一 cropData 派生出的 clip function 和 removal mode，
    // 持久管道本身不重建、不换输入，从而把卡顿点压缩到真正的 clip 执行上。
    // 结果说明：主窗口看到的是主模型本体被 clip 后的结果，而不是额外叠加的一份 overlay 网格。
    target.mainPreviewClipFilter->SetClipFunction(clipFunction);
    if (m_currentRemovalMode == CropRemovalMode::KeepInside) {
        target.mainPreviewClipFilter->InsideOutOn();
    }
    else {
        target.mainPreviewClipFilter->InsideOutOff();
    }

    actor->Modified();
    return true;
}
