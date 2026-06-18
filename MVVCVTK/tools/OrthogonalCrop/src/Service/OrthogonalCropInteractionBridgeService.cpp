// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/Service/OrthogonalCropInteractionBridgeService.cpp
// 分类: Service / Interaction Bridge Implementation
// 说明: 维护交互态、构造 preview request、把结果分发给 overlay 与 3D 主模型预览。
// =====================================================================

#include "OrthogonalCropInteractionBridgeService.h"
#include "OrthogonalCropAlgorithm.h"

#include <vtkActor.h>
#include <vtkBoundingBox.h>
#include <vtkBox.h>
#include <vtkGeometryFilter.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkMath.h>
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
#include <iostream>
#include <utility>

static constexpr const char* kVolumeRemoveInsideEnabledUniform = "mvvcvtk_volumeRemoveInsideEnabled";
static constexpr const char* kVolumeRemoveInsideActiveInputModelToBoxUniform = "mvvcvtk_volumeRemoveInsideActiveInputModelToBox";
static constexpr const char* kVolumeRemoveInsideBaseImplTag = "//VTK::Base::Impl";

static constexpr const char* kVolumeRemoveInsideBaseImplReplacement =
    "//VTK::Base::Impl\n"
    "    if (!g_skip && mvvcvtk_volumeRemoveInsideEnabled != 0)\n"
    "      {\n"
    "      vec4 mvvcvtk_activeInputModelPoint = in_textureDatasetMatrix[0] * vec4(g_dataPos, 1.0);\n"
    "      float mvvcvtk_activeInputModelInvW = abs(mvvcvtk_activeInputModelPoint.w) > 1e-6 ? 1.0 / mvvcvtk_activeInputModelPoint.w : 1.0;\n"
    "      mvvcvtk_activeInputModelPoint = vec4(mvvcvtk_activeInputModelPoint.xyz * mvvcvtk_activeInputModelInvW, 1.0);\n"
    "      vec4 mvvcvtk_boxPoint4 = mvvcvtk_volumeRemoveInsideActiveInputModelToBox * mvvcvtk_activeInputModelPoint;\n"
    "      float mvvcvtk_boxInvW = abs(mvvcvtk_boxPoint4.w) > 1e-6 ? 1.0 / mvvcvtk_boxPoint4.w : 1.0;\n"
    "      vec3 mvvcvtk_boxPoint = mvvcvtk_boxPoint4.xyz * mvvcvtk_boxInvW;\n"
    "      if (all(lessThanEqual(abs(mvvcvtk_boxPoint), vec3(1.0))))\n"
    "        {\n"
    "        g_skip = true;\n"
    "        }\n"
    "      }\n";

OrthogonalCropInteractionBridgeService::OrthogonalCropInteractionBridgeService()
{
    // widget controller 只上报 bounds 和交互阶段；
    // bridge 接管预览请求构建与结果分发，保持 VTK widget 层不触碰后端业务。
    m_widgetStateController.SetWorldBoundsChangedCallback(
        [this](const std::array<double, 6>& worldBounds, CropInteractionPhase phase) {
            HandleWidgetWorldBoundsChanged(worldBounds, phase);
        });
}

// 输入由 bridge 透传给 backend router；
// bridge 自身只管理交互状态，不拥有 image/polydata 后端选择逻辑。
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

void OrthogonalCropInteractionBridgeService::SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    m_backend.SetPreferredDataSource(dataSource);
}

OrthogonalCropDataSource OrthogonalCropInteractionBridgeService::GetActiveDataSource() const
{
    return m_backend.GetActiveDataSource();
}

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetActiveInputModelBounds() const
{
    return m_backend.GetActiveInputModelBounds();
}

OrthogonalCropRequest OrthogonalCropInteractionBridgeService::GetDefaultRequest() const
{
    return m_backend.GetDefaultRequest();
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

void OrthogonalCropInteractionBridgeService::SetReferenceRenderer(vtkRenderer* renderer)
{
    m_referenceRenderer = renderer;
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

void OrthogonalCropInteractionBridgeService::SetFullPreviewRequired(bool required)
{
    m_fullPreviewRequired = required;
}

void OrthogonalCropInteractionBridgeService::TogglePreviewMode()
{
    m_fullPreviewRequired = !m_fullPreviewRequired;
    std::cout << "[Main] Orthogonal crop preview artifact mode: "
        << (m_fullPreviewRequired ? "FullPreview" : "Lightweight3DOutlineGuide")
        << std::endl;

    if (m_previewEnabled
        && m_cropInteractionEnabled
        && m_lastInteractionPhase != CropInteractionPhase::Dragging) {
        UpdatePreviewFromCurrentBounds(true);
    }
}

bool OrthogonalCropInteractionBridgeService::BuildSubmitReloadPayload(OrthogonalCropSubmitReloadPayload& payload)
{
    if (!CanApplySubmit()) {
        return false;
    }

    m_cameraStateController.Save(m_referenceRenderer);

    const auto submitRequest = BuildSubmitRequest();
    const auto submitResult = GetResult(submitRequest);
    if (submitResult.GetFailureReason() != OrthogonalCropFailureReason::None || !submitResult.GetSucceeded()) {
        m_cameraStateController.Clear();
        std::cerr << "[Main] Orthogonal crop submit failed: "
            << GetFailureReasonText(submitResult.GetFailureReason())
            << " - " << submitResult.GetMessage() << std::endl;
        return false;
    }

    if (!BuildSubmitPayload(submitResult, payload)) {
        m_cameraStateController.Clear();
        return false;
    }

    return true;
}

void OrthogonalCropInteractionBridgeService::SetSubmitReloadStarted()
{
    m_submitReloadPending = true;
    m_widgetStateController.SetEnabled(false);
}

void OrthogonalCropInteractionBridgeService::SetSubmitReloadFailed()
{
    m_submitReloadPending = false;
    m_cameraStateController.Clear();
    if (m_cropInteractionEnabled) {
        m_widgetStateController.SetEnabled(true);
    }
}

void OrthogonalCropInteractionBridgeService::SetSubmitReloadSynced()
{
    m_cameraStateController.Restore(m_referenceRenderer);
    if (m_submitReloadPending) {
        m_submitReloadPending = false;
        SetInputImage(nullptr);
        m_worldBoundsInitialized = false;
        DeactivateInteractiveCrop();
    }
}

bool OrthogonalCropInteractionBridgeService::CanApplySubmit() const
{
    if (!m_cropInteractionEnabled || !m_worldBoundsInitialized) {
        std::cerr << "[Main] Orthogonal crop submit failed: crop widget is not active." << std::endl;
        return false;
    }

    if (m_lastInteractionPhase == CropInteractionPhase::Dragging) {
        std::cerr << "[Main] Orthogonal crop submit failed: wait until widget dragging finishes." << std::endl;
        return false;
    }

    if (m_currentRemovalMode != CropRemovalMode::KeepInside) {
        std::cerr << "[Main] Orthogonal crop submit failed: RemoveInside preview does not support submit." << std::endl;
        return false;
    }

    if (m_submitReloadPending) {
        std::cerr << "[Main] Orthogonal crop submit ignored: reload is already pending." << std::endl;
        return false;
    }

    return true;
}

OrthogonalCropRequest OrthogonalCropInteractionBridgeService::BuildSubmitRequest() const
{
    auto submitRequest = BuildPreviewRequest();
    submitRequest.SetExecutionMode(CropExecutionMode::Submit);
    return submitRequest;
}

bool OrthogonalCropInteractionBridgeService::BuildSubmitPayload(
    const OrthogonalCropResult& submitResult,
    OrthogonalCropSubmitReloadPayload& payload) const
{
    auto submitImage = submitResult.GetSubmitImage();
    if (!submitImage) {
        std::cerr << "[Main] Orthogonal crop image submit failed: output image is null." << std::endl;
        return false;
    }

    int dims[3] = { 0, 0, 0 };
    submitImage->GetDimensions(dims);
    const auto sourceData = static_cast<const float*>(submitImage->GetScalarPointer());
    if (!sourceData || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        std::cerr << "[Main] Orthogonal crop image submit failed: output image buffer is invalid." << std::endl;
        return false;
    }

    double spacingData[3] = { 1.0, 1.0, 1.0 };
    double imageModelOriginData[3] = { 0.0, 0.0, 0.0 };
    submitImage->GetSpacing(spacingData);
    submitImage->GetOrigin(imageModelOriginData);

    payload.dims = { dims[0], dims[1], dims[2] };
    payload.spacing = {
        static_cast<float>(spacingData[0]),
        static_cast<float>(spacingData[1]),
        static_cast<float>(spacingData[2])
    };
    payload.origin = {
        static_cast<float>(-imageModelOriginData[0] - (static_cast<double>(dims[0] - 1) * spacingData[0])),
        static_cast<float>(-imageModelOriginData[1] - (static_cast<double>(dims[1] - 1) * spacingData[1])),
        static_cast<float>(imageModelOriginData[2])
    };

    const size_t nx = static_cast<size_t>(dims[0]);
    const size_t ny = static_cast<size_t>(dims[1]);
    const size_t nz = static_cast<size_t>(dims[2]);
    const size_t sliceSize = nx * ny;
    const size_t totalSize = sliceSize * nz;
    payload.buffer = std::make_shared<std::vector<float>>(totalSize, 0.0f);
    auto* reloadData = payload.buffer->data();
    for (size_t z = 0; z < nz; ++z) {
        const size_t sliceOffset = z * sliceSize;
        for (size_t y = 0; y < ny; ++y) {
            const float* sourceRow = sourceData + sliceOffset + y * nx;
            float* targetRow = reloadData + sliceOffset + (ny - 1 - y) * nx;
            for (size_t x = 0; x < nx; ++x) {
                targetRow[nx - 1 - x] = sourceRow[x];
            }
        }
    }

    return true;
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

    if (!m_worldBoundsInitialized) {
        m_currentWorldBounds = GetDefaultInteractiveWorldBounds();
        m_worldBoundsInitialized = true;
    }

    // 进入交互模式时，widget 使用 world bounds；真正执行时再通过 worldToInputModel 折回 active input model。
    m_widgetStateController.SetInteractor(m_primaryInteractor);
    m_widgetStateController.SetReferenceWorldBounds(GetActiveWorldBounds()); // 初始化widget交互范围
    m_widgetStateController.SetWidgetWorldBounds(m_currentWorldBounds); // 设置实际交互盒子范围有多大
    if (!m_widgetStateController.SetEnabled(true)) {
        std::cerr << "[Main] Orthogonal crop widget init failed: vtkBoxWidget2 could not be enabled." << std::endl;
        return false;
    }

    m_cropInteractionEnabled = true;
    m_lastInteractionPhase = CropInteractionPhase::Released;
    RestorePreviewRenderTargets();
    std::cout << "[Main] Orthogonal crop widget active. UI uses vtkBoxWidget2, backend = "
        << GetDataSourceText(GetActiveDataSource())
        << ". Press 1 to toggle inside preview, press 2 to toggle outside preview, press 3 to toggle lightweight/full preview, press Ctrl+3 to apply submit; press O or Esc to exit." << std::endl;
    return true;
}

bool OrthogonalCropInteractionBridgeService::DeactivateInteractiveCrop()
{
    if (!m_cropInteractionEnabled) {
        return false;
    }

    if (m_submitReloadPending) {
        std::cout << "[Main] Orthogonal crop widget deactivation deferred: image submit reload is pending." << std::endl;
        return false;
    }

    m_widgetStateController.SetEnabled(false);
    m_submitReloadPending = false;
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

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetDefaultInteractiveWorldBounds() const
{
    // 默认交互盒在 world 坐标下构造：
    // 1. 先取当前活跃输入提升到 world 后的完整 bounds
    // 2. 再按经验比例缩小，得到首次拖拽更容易观察的起始盒
    // 3. 这只盒子只服务 widget；真正执行时还会折回 active input model
    std::array<double, 6> worldBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    const auto activeWorldBounds = GetActiveWorldBounds();
    if (!(activeWorldBounds[0] < activeWorldBounds[1]
        && activeWorldBounds[2] < activeWorldBounds[3]
        && activeWorldBounds[4] < activeWorldBounds[5])) {
        return worldBounds;
    }

    const std::array<double, 3> center = {
        (activeWorldBounds[0] + activeWorldBounds[1]) * 0.5,
        (activeWorldBounds[2] + activeWorldBounds[3]) * 0.5,
        (activeWorldBounds[4] + activeWorldBounds[5]) * 0.5
    };
    const std::array<double, 3> dimensions = {
        (activeWorldBounds[1] - activeWorldBounds[0]) * 0.30,
        (activeWorldBounds[3] - activeWorldBounds[2]) * 0.24,
        (activeWorldBounds[5] - activeWorldBounds[4]) * 0.36
    };

    // 这组比例不是算法约束，而是交互层的默认起始盒经验值：
    // 初始盒要足够小，便于用户第一下就看到明显裁切效果，同时保持三个轴都有可拖拽余量。

    worldBounds[0] = center[0] - dimensions[0] * 0.5;
    worldBounds[1] = center[0] + dimensions[0] * 0.5;
    worldBounds[2] = center[1] - dimensions[1] * 0.5;
    worldBounds[3] = center[1] + dimensions[1] * 0.5;
    worldBounds[4] = center[2] - dimensions[2] * 0.5;
    worldBounds[5] = center[2] + dimensions[2] * 0.5;
    return worldBounds;
}

void OrthogonalCropInteractionBridgeService::HandleWidgetWorldBoundsChanged(
    const std::array<double, 6>& worldBounds,
    CropInteractionPhase phase)
{
    if (!m_cropInteractionEnabled) {
        return;
    }

    // 交互过程中始终记录最新 world AABB 和 phase，
    // 但只在 Released 时触发 preview，保持“拖拽只更新轻量 UI，释放后再统一跑后端”的既有流程。
    m_currentWorldBounds = worldBounds;
    m_worldBoundsInitialized = true;
    m_lastInteractionPhase = phase;
    if (m_previewEnabled && phase == CropInteractionPhase::Released) {
        UpdatePreviewFromCurrentBounds(true);
    }
}

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetActiveInputModelBoundsAsWorldBounds(
    const std::array<double, 6>& activeInputModelBounds) const
{
    // 这是纯 bounds 级别的 8 角点变换 helper：
    // 输入是 active input model 轴对齐盒，输出是 widget 使用的 world AABB。
    // 对 image 路径，input model 底层由 VTK physical-point API 表达。
    // 显式变换 8 个角点后再回收包围盒，可以避免旋转场景下的逐轴换算失真。
    if (!m_referenceRenderService) {
        return activeInputModelBounds;
    }

    auto activeInputModelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    activeInputModelToWorldMatrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());

    auto activeInputModelToWorldTransform = vtkSmartPointer<vtkTransform>::New();
    activeInputModelToWorldTransform->SetMatrix(activeInputModelToWorldMatrix);

    vtkBoundingBox worldBounds;
    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                const double activeInputModelCorner[3] = {
                    activeInputModelBounds[ix == 0 ? 0 : 1],
                    activeInputModelBounds[iy == 0 ? 2 : 3],
                    activeInputModelBounds[iz == 0 ? 4 : 5]
                };
                double worldCorner[3] = { 0.0, 0.0, 0.0 };
                activeInputModelToWorldTransform->TransformPoint(activeInputModelCorner, worldCorner);
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
    // backend/router 暴露的是 active input model bounds；
    // widget 放在 world 里，所以这里负责做一次 active input model -> world 的包围盒提升。
    const auto activeInputModelBounds = GetActiveInputModelBounds();
    if (!(activeInputModelBounds[0] < activeInputModelBounds[1]
        && activeInputModelBounds[2] < activeInputModelBounds[3]
        && activeInputModelBounds[4] < activeInputModelBounds[5])) {
        return activeInputModelBounds;
    }

    return GetActiveInputModelBoundsAsWorldBounds(activeInputModelBounds);
}

std::array<double, 16> OrthogonalCropInteractionBridgeService::GetWorldToActiveInputModelMatrix() const
{
    if (!m_referenceRenderService) {
        return GetIdentityMatrixArray();
    }

    // 参考窗口维护的是 active input model -> world 矩阵；裁切 request 需要反向矩阵，
    // 否则 widget 盒和后端 active input model 会落在两个不同坐标系里。
    // 对 image 路径，active input model 底层由 VTK physical-point API 表达；
    // 对 polydata 路径，则是主网格自己的 input model 坐标。
    auto worldToActiveInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToActiveInputModelMatrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());
    worldToActiveInputModelMatrix->Invert();

    std::array<double, 16> worldToActiveInputModelMatrixData = { 0.0 };
    vtkMatrix4x4::DeepCopy(worldToActiveInputModelMatrixData.data(), worldToActiveInputModelMatrix);
    return worldToActiveInputModelMatrixData;
}

const OrthogonalCropRequest OrthogonalCropInteractionBridgeService::BuildPreviewRequest() const
{
    // 先获取当前数据源的默认 request；
    // image 与 polydata 的 bounds 归一化由后端入口收口，bridge 只补交互盒姿态。
    auto previewRequest = GetDefaultRequest();

    // 准备 widget 有向盒的 world 基准信息；
    // 默认值来自当前 world AABB，GetCurrentWorldBox 成功时会替换成最近一次 PlaceWidget 的基准盒。
    CropVectorDouble3Array initialWorldCenter = {
        (m_currentWorldBounds[0] + m_currentWorldBounds[1]) * 0.5,
        (m_currentWorldBounds[2] + m_currentWorldBounds[3]) * 0.5,
        (m_currentWorldBounds[4] + m_currentWorldBounds[5]) * 0.5
    };
    CropVectorDouble3Array initialWorldDimensions = {
        m_currentWorldBounds[1] - m_currentWorldBounds[0],
        m_currentWorldBounds[3] - m_currentWorldBounds[2],
        m_currentWorldBounds[5] - m_currentWorldBounds[4]
    };
    CropMatrixDouble16Array boxToInputModelMatrixData = GetIdentityMatrixArray();
    CropMatrixDouble16Array initialWorldToCurrentWorldMatrixData = GetIdentityMatrixArray();

    // 读取 widget 当前姿态并反解 initialWorld -> currentWorld；
    // 失败时保留默认 request，避免把不完整 widget 状态发给后端。
    if (!m_widgetStateController.GetCurrentWorldBox(
        initialWorldCenter,
        initialWorldDimensions,
        initialWorldToCurrentWorldMatrixData)) {
        return previewRequest;
    }

    // boxToInitialWorldMatrix 把固定标准盒 [-1,1]^3 放回 PlaceWidget 时的 world AABB；
    // 标准盒半径为 1，所以轴向缩放必须使用 initialWorldDimensions * 0.5。
    auto boxToInitialWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInitialWorldMatrix->Identity();
    for (int axis = 0; axis < 3; ++axis) {
        boxToInitialWorldMatrix->SetElement(axis, axis, initialWorldDimensions[axis] * 0.5);
        boxToInitialWorldMatrix->SetElement(axis, 3, initialWorldCenter[axis]);
    }

    // initialWorldToCurrentWorldMatrix 表达用户交互造成的姿态变化；
    // 它接在 boxToInitialWorldMatrix 后面，负责把初始 world AABB 变成当前 world 有向盒。
    auto initialWorldToCurrentWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    initialWorldToCurrentWorldMatrix->DeepCopy(initialWorldToCurrentWorldMatrixData.data());

    // boxToWorldMatrix 是当前裁切盒在 world 中的完整几何；
    // 组合顺序从右到左读：标准盒先进入初始 world，再应用交互后的 current world 姿态。
    auto boxToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(initialWorldToCurrentWorldMatrix, boxToInitialWorldMatrix, boxToWorldMatrix);

    // worldToActiveInputModelMatrix 把显示层 world 几何折回当前数据输入坐标；
    // image 使用 physical-point 语义，polydata 使用主网格 input model 语义。
    auto worldToActiveInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToActiveInputModelMatrix->DeepCopy(GetWorldToActiveInputModelMatrix().data());

    // boxToInputModelMatrix 是 request 下发给后端的唯一几何真源；
    // 后端只消费标准盒到 active input model 的矩阵，不再依赖 widget 或 world 状态。
    auto boxToInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(worldToActiveInputModelMatrix, boxToWorldMatrix, boxToInputModelMatrix);
    vtkMatrix4x4::DeepCopy(boxToInputModelMatrixData.data(), boxToInputModelMatrix);

    previewRequest.SetBoxToInputModelMatrix(boxToInputModelMatrixData);
    previewRequest.SetExecutionMode(CropExecutionMode::PreviewArtifact);
    previewRequest.SetPreviewArtifactMode(
        m_fullPreviewRequired
            ? CropPreviewArtifactMode::FullPreview
            : CropPreviewArtifactMode::Lightweight3DOutlineGuide);
    previewRequest.SetRemovalMode(m_currentRemovalMode);

    auto cropState = previewRequest.GetCropStateModel();
    cropState.SetCropEnabled(m_cropInteractionEnabled);
    cropState.SetInteractionPhase(m_lastInteractionPhase);

    // cropState 只描述交互语义，不参与几何计算；
    // 它随结果回到 overlay 和日志，让调用方区分拖拽释放与模式切换触发。
    previewRequest.SetCropStateModel(cropState);
    return previewRequest;
}

void OrthogonalCropInteractionBridgeService::UpdatePreviewFromCurrentBounds(bool logStats)
{
    if (!m_worldBoundsInitialized) {
        return;
    }

    // 将当前 widget world 有向盒固化为 active input model request；
    // 后端根据 request 内的 preview artifact mode 选择轻量 guide 或完整预览产物。
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

    // 分发预览结果到所有目标；
    // 轻量模式只携带 outline，完整模式再携带 image mask 或 polydata clip artifact。
    const bool main3DPreviewApplied = DispatchPreviewResult(previewResult);

    // 3D 主模型 clip 只是临时显示状态；
    // 关闭 preview 时切回 pass-through 管道即可恢复全模型，不需要重新向后端请求恢复结果。

    if (logStats) {
        // 日志优先使用 result 的 resolved 信息；
        // 若上层结果未回填，则退回 statistics，保证日志仍能说明实际后端来源。
        const auto dataSource = previewResult.GetResolvedDataSource() != OrthogonalCropDataSource::Auto
            ? previewResult.GetResolvedDataSource()
            : previewStats.GetResolvedDataSource();
        const auto backend = previewResult.GetResolvedBackend() != OrthogonalCropResolvedBackend::None
            ? previewResult.GetResolvedBackend()
            : previewStats.GetResolvedBackend();
        std::cout
            << "[Main] Orthogonal crop preview updated. source = "
            << GetDataSourceText(dataSource)
            << ", artifact = "
            << GetPreviewArtifactModeText(previewRequest.GetPreviewArtifactMode())
            << ", backend = "
            << GetResolvedBackendText(backend)
            << ", removal = "
            << GetRemovalModeText(previewResult.GetRemovalMode())
            << ", main3D = "
            << (main3DPreviewApplied ? "MainPreview" : "OverlayOnly")
            << ", bounds = ["
            << m_currentWorldBounds[0] << ", " << m_currentWorldBounds[1] << "; "
            << m_currentWorldBounds[2] << ", " << m_currentWorldBounds[3] << "; "
            << m_currentWorldBounds[4] << ", " << m_currentWorldBounds[5] << "]"
            << std::endl;
    }
}

void OrthogonalCropInteractionBridgeService::TogglePreview(CropRemovalMode removalMode, bool logStats)
{
    // Toggle 语义固定为：
    // - 同模式再次触发：关闭 preview，并恢复主模型全量显示
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
    case OrthogonalCropFailureReason::SubmitRemoveInsideUnsupported:
        return "SubmitRemoveInsideUnsupported";
    case OrthogonalCropFailureReason::InsufficientRam:
        return "InsufficientRam";
    case OrthogonalCropFailureReason::MaskPreviewCreationFailed:
        return "MaskPreviewCreationFailed";
    case OrthogonalCropFailureReason::SubmitImageCreationFailed:
        return "SubmitImageCreationFailed";
    case OrthogonalCropFailureReason::ClipPreviewPolyDataCreationFailed:
        return "ClipPreviewPolyDataCreationFailed";
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

const char* OrthogonalCropInteractionBridgeService::GetPreviewArtifactModeText(CropPreviewArtifactMode previewArtifactMode)
{
    switch (previewArtifactMode) {
    case CropPreviewArtifactMode::Lightweight3DOutlineGuide:
        return "Lightweight3DOutlineGuide";
    case CropPreviewArtifactMode::FullPreview:
        return "FullPreview";
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
    case OrthogonalCropResolvedBackend::MaskPreview:
        return "MaskPreview";
    case OrthogonalCropResolvedBackend::SubmitExtractVOI:
        return "SubmitExtractVOI";
    case OrthogonalCropResolvedBackend::ClipPreview:
        return "ClipPreview";
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
    // 这里只清理 overlay 生命周期；
    // 主模型 clip 管道由 RestorePreviewRenderTargets 收口，避免解绑窗口时意外改动主显示。
    for (const auto& target : m_previewRenderTargets) {
        if (target.service && target.overlayStrategy) {
            target.service->RemoveOverlayStrategy(target.overlayStrategy);
        }
    }
    m_previewRenderTargets.clear();
}

void OrthogonalCropInteractionBridgeService::RestorePreviewRenderTargets()
{
    // 恢复 preview 涉及 overlay、volume shader 和 polydata clip 三条显示路径；
    // 统一在这里清空临时状态并标脏，确保关闭 preview 后各窗口回到全模型显示。
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

        RestorePolyDataPreview(target);
        if (target.service) {
            target.service->MarkDirty();
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
    service->AddOverlayStrategy(overlayStrategy);
    m_previewRenderTargets.push_back({ service, overlayStrategy });
}

bool OrthogonalCropInteractionBridgeService::DispatchPreviewResult(const OrthogonalCropResult& previewResult)
{
    bool main3DPreviewApplied = false;

    // 分发时优先让 3D 主窗口接管主模型 clip；
    // overlay 再按窗口轴向消费剩余 artifact，避免同一份 polydata 被主模型和 overlay 重复绘制。
    for (auto& target : m_previewRenderTargets) {
        if (target.service && target.overlayStrategy) {
            bool mainPreviewAppliedForTarget = ApplyVolumePreview(target, previewResult);
            if (!mainPreviewAppliedForTarget) {
                mainPreviewAppliedForTarget = ApplyPolyDataPreview(target, previewResult);
            }

            auto overlayResult = previewResult;
            if (mainPreviewAppliedForTarget && target.service->GetNavigationAxis() < 0) {
                // 3D 主窗口已经由常驻 clip 管道直接表现裁切结果，
                // 不再额外叠一份 clipped polydata overlay，避免重复绘制同一套网格。
                overlayResult.SetClipPolyData(nullptr);
            }

            target.overlayStrategy->SetSliceAxis(target.service->GetNavigationAxis());
            target.overlayStrategy->SetRemovalMode(previewResult.GetRemovalMode());
            target.overlayStrategy->SetCropResult(overlayResult);
            main3DPreviewApplied = mainPreviewAppliedForTarget || main3DPreviewApplied;
            target.service->MarkDirty();
        }
    }
    return main3DPreviewApplied;
}

bool OrthogonalCropInteractionBridgeService::ApplyVolumePreview(
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

    if (previewResult.GetRemovalMode() != CropRemovalMode::KeepInside) {
        auto gpuVolumeMapper = vtkGPUVolumeRayCastMapper::SafeDownCast(volumeMapper);
        if (!ApplyVolumeRemoveInsidePreview(volume, gpuVolumeMapper, previewResult)) {
            volumeMapper->SetCroppingRegionFlagsToSubVolume();
            volume->Modified();
            return false;
        }

        volume->Modified();
        return true;
    }

    ApplyVolumeKeepInsidePreview(volumeMapper, previewResult);
    volume->Modified();
    return true;
}

void OrthogonalCropInteractionBridgeService::ApplyVolumeKeepInsidePreview(
    vtkVolumeMapper* volumeMapper,
    const OrthogonalCropResult& previewResult) const
{
    if (!volumeMapper) {
        return;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto activeInputModelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    activeInputModelToWorldMatrix->Identity();
    if (m_referenceRenderService) {
        activeInputModelToWorldMatrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());
    }

    auto boxToActiveInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToActiveInputModelMatrix->DeepCopy(cropData.GetBoxToInputModelMatrix().data());

    // VTK clipping planes 需要 world 坐标；cropData 保存的是 box -> active input model，
    // 所以先还原 box -> world，再把标准盒 6 个面转换成 world plane。
    auto boxToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(activeInputModelToWorldMatrix, boxToActiveInputModelMatrix, boxToWorldMatrix);

    auto boxToWorldTransform = vtkSmartPointer<vtkTransform>::New();
    boxToWorldTransform->SetMatrix(boxToWorldMatrix);

    auto clippingPlanes = vtkSmartPointer<vtkPlaneCollection>::New();
    for (int axis = 0; axis < 3; ++axis) {
        for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
            const double side = sideIndex == 0 ? -1.0 : 1.0;
            double boxOrigin[3] = { 0.0, 0.0, 0.0 };
            boxOrigin[axis] = side;

            double boxNormal[3] = { 0.0, 0.0, 0.0 };
            // KeepInside 需要保留 6 个面共同围住的内部区域；
            // 法线朝内时，VTK clipping planes 的交集语义正好表达盒内保留。
            boxNormal[axis] = -side;

            double worldOrigin[3] = { 0.0, 0.0, 0.0 };
            double worldNormal[3] = { 0.0, 0.0, 0.0 };
            boxToWorldTransform->TransformPoint(boxOrigin, worldOrigin);
            boxToWorldTransform->TransformNormal(boxNormal, worldNormal);

            if (vtkMath::Normalize(worldNormal) <= 1e-12) {
                continue;
            }

            auto plane = vtkSmartPointer<vtkPlane>::New();
            plane->SetOrigin(worldOrigin);
            plane->SetNormal(worldNormal);
            clippingPlanes->AddItem(plane);
        }
    }

    // KeepInside 和 RemoveInside 都只消费 previewResult.cropData；
    // 这里仅在 VTK volume clipping 需要的后端表达点，把 box/active input model 盒还原成 world planes。
    volumeMapper->SetClippingPlanes(clippingPlanes);
}

bool OrthogonalCropInteractionBridgeService::ApplyVolumeRemoveInsidePreview(
    vtkVolume* volume,
    vtkGPUVolumeRayCastMapper* volumeMapper,
    const OrthogonalCropResult& previewResult) const
{
    if (!volume || !volumeMapper) {
        return false;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto boxToActiveInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToActiveInputModelMatrix->DeepCopy(cropData.GetBoxToInputModelMatrix().data());

    // RemoveInside shader 在采样点上判断是否落入标准盒 [-1,1]^3。
    // 采样点先由 VTK shader 内置矩阵还原到 active input model，再用 activeInputModelToBox 送入标准盒空间。
    auto activeInputModelToBoxMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(boxToActiveInputModelMatrix, activeInputModelToBoxMatrix);

    auto shaderProperty = volume->GetShaderProperty();
    shaderProperty->AddFragmentShaderReplacement(
        kVolumeRemoveInsideBaseImplTag,
        true,
        kVolumeRemoveInsideBaseImplReplacement,
        false);

    // 体渲染只消费 previewResult 中的 cropData；业务来源统一由 request -> cropData 决定。
    auto activeInputModelToBoxShaderMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    activeInputModelToBoxShaderMatrix->DeepCopy(activeInputModelToBoxMatrix);
    // 上传前转置矩阵；
    // VTK uniforms 按 OpenGL 列主序解释，转置后 shader 乘法才与 C++ activeInputModelToBox 结果一致。
    activeInputModelToBoxShaderMatrix->Transpose();

    // vtk volume shader 中 g_dataPos 是纹理坐标；
    // in_textureDatasetMatrix[0] 会把它还原到 active input model。
    // vtkUniforms 上传 vtkMatrix4x4 时按 OpenGL 列主序解释，所以自定义 activeInputModelToBox 需要先转置再交给 shader。
    // uniforms 的声明由 VTK 的 CustomUniforms::Dec 自动生成，不能再手写到 Base::Dec，否则会重复声明。
    auto fragmentUniforms = shaderProperty->GetFragmentCustomUniforms();
    fragmentUniforms->SetUniformMatrix(kVolumeRemoveInsideActiveInputModelToBoxUniform, activeInputModelToBoxShaderMatrix);
    fragmentUniforms->SetUniformi(kVolumeRemoveInsideEnabledUniform, 1);

    volumeMapper->SetMaskInput(nullptr);
    shaderProperty->Modified();
    volumeMapper->Modified();
    volume->Modified();
    return true;
}

void OrthogonalCropInteractionBridgeService::RestorePolyDataPreview(PreviewRenderTarget& target)
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

bool OrthogonalCropInteractionBridgeService::ApplyPolyDataPreview(
    PreviewRenderTarget& target,
    const OrthogonalCropResult& previewResult)
{
    if (!target.service || target.service->GetNavigationAxis() >= 0 || !previewResult.GetSucceeded()) {
        return false;
    }

    // 只有 3D 主窗口接管主 mapper；
    // 2D 窗口继续走 overlay，避免切片视图被 3D polydata 管道影响。
    // 首次命中时建立常驻 clip 管道，后续只更新 clip function 和 InsideOut 语义。

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

    auto activeInputModelToBoxTransform = vtkSmartPointer<vtkTransform>::New();
    // vtkBox 的 implicit function 定义在标准盒空间；把 active input model 点先变到 box 空间，
    // 就能复用同一个 [-1,1]^3 盒子表达任意旋转/缩放后的裁切区域。
    activeInputModelToBoxTransform->SetMatrix(cropData.GetBoxToInputModelMatrix().data());
    activeInputModelToBoxTransform->Inverse();
    clipFunction->SetTransform(activeInputModelToBoxTransform);

    // 每次刷新只更新 cropData 派生出的 clip function 和 removal mode；
    // 持久管道不重建、不换输入，让主窗口显示主模型本体的 clip 结果而不是叠加网格。
    target.mainPreviewClipFilter->SetClipFunction(clipFunction);
    if (previewResult.GetRemovalMode() == CropRemovalMode::KeepInside) {
        target.mainPreviewClipFilter->InsideOutOn();
    }
    else {
        target.mainPreviewClipFilter->InsideOutOff();
    }

    actor->Modified();
    return true;
}
