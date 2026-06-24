// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/Service/OrthogonalCropInteractionBridgeService.cpp
// 分类: Service / Interaction Bridge Implementation
// 说明: 维护交互态、构造 preview request、把结果分发给 overlay 与 3D 主模型预览。
// =====================================================================

#include "OrthogonalCropInteractionBridgeService.h"

#include <vtkActor.h>
#include <vtkBoundingBox.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyDataMapper.h>
#include <vtkTransform.h>

#include <algorithm>
#include <iostream>
#include <utility>

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

bool OrthogonalCropInteractionBridgeService::BuildSubmitReloadPayload(OrthogonalCropSubmitReloadPayload& payload)
{
    if (!CanApplySubmit()) {
        return false;
    }

    m_cameraStateController.Save(m_referenceRenderer);

    const auto submitRequest = BuildSubmitRequest();
    const auto submitResult = m_backend.GetResult(submitRequest, BuildResultContext(submitRequest));
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
        ExitInteractiveCrop();
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

    if (!GetInputImage()) {
        std::cerr << "[Main] Orthogonal crop submit failed: image crop input is missing." << std::endl;
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
    auto submitRequest = BuildPreviewRequest(OrthogonalCropDataSource::ImageData);
    submitRequest.SetOperation(OrthogonalCropOperation::Submit);
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
    if (!EnsureInputReady()) {
        std::cerr << "[Main] Orthogonal crop trigger failed: no active image/polydata input is available yet." << std::endl;
        return false;
    }

    if (m_cropInteractionEnabled) {
        ExitInteractiveCrop();
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
    std::cout << "[Main] Orthogonal crop widget active. UI uses vtkBoxWidget2, dataSource = "
        << GetDataSourceText(m_backend.GetActiveDataSource())
        << ". Press 1 to toggle inside preview, press 2 to toggle outside preview, press Ctrl+3 to apply submit; press O or Esc to exit." << std::endl;
    return true;
}

bool OrthogonalCropInteractionBridgeService::ExitInteractiveCrop()
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
    // bridge 只保证“当前至少有一个可用输入”。
    // 它不在这里做任何坐标折叠；缺 image 输入时才从 data manager 兜底补 image。
    if (GetInputImage() || m_backend.GetInputPolyData()) {
        return true;
    }

    if (m_dataMgr) {
        SetInputImage(m_dataMgr->GetVtkImage());
    }

    return GetInputImage() || m_backend.GetInputPolyData();
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
    // 但只在 Released 时触发 preview，保持“拖拽只更新显示状态，释放后再统一跑后端”的既有流程。
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
    const auto activeInputModelBounds = m_backend.GetActiveInputModelBounds();
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

OrthogonalCropRequest OrthogonalCropInteractionBridgeService::BuildPreviewRequest(
    OrthogonalCropDataSource dataSource) const
{
    // 先获取当前数据源的默认 request；
    // image 与 polydata 的 bounds 归一化由后端入口收口，bridge 只补交互盒姿态。
    auto previewRequest = m_backend.GetDefaultRequest();
    previewRequest.SetDataSource(dataSource);

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

    // boxToInputModelMatrix 是 request 下发给算法层的唯一几何真源；
    // 算法只消费标准盒到 active input model 的矩阵，不再依赖 widget 或 world 状态。
    auto boxToInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(worldToActiveInputModelMatrix, boxToWorldMatrix, boxToInputModelMatrix);
    vtkMatrix4x4::DeepCopy(boxToInputModelMatrixData.data(), boxToInputModelMatrix);

    previewRequest.SetBoxToInputModelMatrix(boxToInputModelMatrixData);
    previewRequest.SetOperation(OrthogonalCropOperation::Preview);
    previewRequest.SetRemovalMode(m_currentRemovalMode);

    auto cropState = previewRequest.GetCropStateModel();
    cropState.SetCropEnabled(m_cropInteractionEnabled);
    cropState.SetInteractionPhase(m_lastInteractionPhase);

    // cropState 只描述交互语义，不参与几何计算；
    // 它随结果回到 overlay 和日志，让调用方区分拖拽释放与模式切换触发。
    previewRequest.SetCropStateModel(cropState);
    return previewRequest;
}

OrthogonalCropResult OrthogonalCropInteractionBridgeService::BuildResultContext(const OrthogonalCropRequest& request) const
{
    // bridge 在调用 backend 前固定结果身份；
    // router 和算法只填充 artifact、cropData 和 diagnostics，避免下层重新决定业务动作。
    OrthogonalCropResult resultContext;
    resultContext.SetResolvedDataSource(request.GetDataSource());
    resultContext.SetResolvedOperation(request.GetOperation());
    resultContext.SetCropStateModel(request.GetCropStateModel());
    return resultContext;
}

void OrthogonalCropInteractionBridgeService::UpdatePreviewFromCurrentBounds(bool logStats)
{
    if (!m_worldBoundsInitialized) {
        return;
    }

    // 体渲染预览使用 router 已显式持有的图像输入；
    // 网格预览直接读取目标窗口通过 GetMainProp 暴露的主三维输入，bridge 不识别具体策略类。
    OrthogonalCropResult volumeResult;
    bool hasVolumeResult = false;
    if (GetInputImage()) {
        const auto volumeRequest = BuildPreviewRequest(OrthogonalCropDataSource::VolumeData);
        volumeResult = m_backend.GetResult(volumeRequest, BuildResultContext(volumeRequest));
        if (volumeResult.GetFailureReason() != OrthogonalCropFailureReason::None) {
            if (logStats) {
                std::cerr << "[Main] Orthogonal crop volume preview failed: "
                    << GetFailureReasonText(volumeResult.GetFailureReason())
                    << " - " << volumeResult.GetMessage() << std::endl;
            }
            return;
        }

        if (!volumeResult.GetSucceeded()) {
            if (logStats) {
                std::cerr << "[Main] Orthogonal crop volume preview result warning: "
                    << GetFailureReasonText(volumeResult.GetFailureReason())
                    << " - " << volumeResult.GetMessage() << std::endl;
            }
            return;
        }

        hasVolumeResult = true;
    }

    bool main3DPreviewApplied = false;
    bool polyDataPreviewApplied = false;
    bool anyPreviewApplied = false;
    for (const auto& target : m_previewRenderTargets) {
        if (!target.service || !target.overlayStrategy) {
            continue;
        }

        OrthogonalCropResult polyResult;
        bool hasPolyDataResult = false;
        m_previewPlug.RestorePreview(target.service, target.overlayStrategy);

        auto actor = vtkActor::SafeDownCast(target.service->GetMainProp());
        auto mapper = actor ? vtkPolyDataMapper::SafeDownCast(actor->GetMapper()) : nullptr;
        if (mapper) {
            mapper->Update();
            if (auto polyData = vtkPolyData::SafeDownCast(mapper->GetInput())) {
                m_backend.SetInputPolyData(polyData);

                const auto polyRequest = BuildPreviewRequest(OrthogonalCropDataSource::PolyData);
                polyResult = m_backend.GetResult(polyRequest, BuildResultContext(polyRequest));
                // clipPolyData 是 polydata 后端的成功产物；主 actor 的 shader discard
                // 可能只消费 cropData，但这里仍要避免把只有诊断信息的结果当作有效预览。
                if (polyResult.GetFailureReason() == OrthogonalCropFailureReason::None
                    && polyResult.GetSucceeded()
                    && polyResult.GetClipPolyData()) {
                    hasPolyDataResult = true;
                    polyDataPreviewApplied = true;
                }
                else if (logStats && polyResult.GetFailureReason() != OrthogonalCropFailureReason::InputPolyDataMissing) {
                    std::cerr << "[Main] Orthogonal crop polydata preview skipped: "
                        << GetFailureReasonText(polyResult.GetFailureReason())
                        << " - " << polyResult.GetMessage() << std::endl;
                }
            }
        }

        if (!hasVolumeResult && !hasPolyDataResult) {
            continue;
        }

        main3DPreviewApplied = DispatchPreviewResult(
            target,
            m_currentRemovalMode,
            hasVolumeResult ? &volumeResult : nullptr,
            hasPolyDataResult ? &polyResult : nullptr) || main3DPreviewApplied;
        anyPreviewApplied = true;
    }

    if (!anyPreviewApplied) {
        if (logStats) {
            std::cerr << "[Main] Orthogonal crop preview skipped: no volume or polydata preview input is available." << std::endl;
        }
        return;
    }

    if (logStats) {
        // 日志只报告本次显式跑过的数据源；结果与统计只是执行回执，不能反过来作为流程判断来源。
        std::cout
            << "[Main] Orthogonal crop preview updated. volume = "
            << (hasVolumeResult ? "Used" : "Skipped")
            << ", polydata = "
            << (polyDataPreviewApplied ? "Used" : "Skipped")
            << ", operation = "
            << GetOperationText(OrthogonalCropOperation::Preview)
            << ", removal = "
            << GetRemovalModeText(m_currentRemovalMode)
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
    case OrthogonalCropFailureReason::UnsupportedBackend:
        return "UnsupportedBackend";
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

const char* OrthogonalCropInteractionBridgeService::GetDataSourceText(OrthogonalCropDataSource dataSource)
{
    switch (dataSource) {
    case OrthogonalCropDataSource::ImageData:
        return "ImageData";
    case OrthogonalCropDataSource::VolumeData:
        return "VolumeData";
    case OrthogonalCropDataSource::PolyData:
        return "PolyData";
    default:
        return "Unknown";
    }
}

const char* OrthogonalCropInteractionBridgeService::GetOperationText(OrthogonalCropOperation operation)
{
    switch (operation) {
    case OrthogonalCropOperation::Preview:
        return "Preview";
    case OrthogonalCropOperation::Submit:
        return "Submit";
    case OrthogonalCropOperation::None:
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
    // 主模型 preview 状态由 RestorePreviewRenderTargets 收口，避免解绑窗口时意外改动主显示。
    for (const auto& target : m_previewRenderTargets) {
        if (target.service && target.overlayStrategy) {
            target.service->RemoveOverlayStrategy(target.overlayStrategy);
        }
    }
    m_previewPlug.Clear();
    m_previewRenderTargets.clear();
}

void OrthogonalCropInteractionBridgeService::RestorePreviewRenderTargets()
{
    // 恢复 preview 涉及 overlay、volume shader 和 polydata clip 三条显示路径；
    // 统一在这里清空临时状态并标脏，确保关闭 preview 后各窗口回到全模型显示。
    for (auto& target : m_previewRenderTargets) {
        m_previewPlug.RestorePreview(target.service, target.overlayStrategy);
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

bool OrthogonalCropInteractionBridgeService::DispatchPreviewResult(
    const PreviewRenderTarget& target,
    CropRemovalMode removalMode,
    const OrthogonalCropResult* volumePreviewResult,
    const OrthogonalCropResult* polyDataPreviewResult)
{
    if (!target.service || !target.overlayStrategy) {
        return false;
    }

    const bool mainPreviewApplied = m_previewPlug.ApplyPreview(
        target.service,
        target.overlayStrategy,
        m_referenceRenderService,
        volumePreviewResult,
        polyDataPreviewResult,
        removalMode);

    target.service->MarkDirty();
    return mainPreviewApplied;
}
