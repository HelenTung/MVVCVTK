// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/src/Interaction/OrthogonalCropInteractionBridgeService.cpp
// 分类: Service / Interaction Bridge Implementation
// 说明: 维护交互态，构造 preview/submit request，把预览结果分发给 overlay 与 3D 主模型，
//       并把 submit 结果交给主数据 reload 通道。
// =====================================================================

#include "Interaction/OrthogonalCropInteractionBridgeService.h"

#include <vtkBoundingBox.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkTransform.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

static constexpr double kBoxInitialBoundsScale = 0.60;
static constexpr double kPlanarInitialRectangleExtentScale = 0.35;
static constexpr double kPlaneVectorEpsilon = 1e-12;

namespace {
bool GetImageReady(vtkImageData* image)
{
    if (!image || !image->GetScalarPointer()) {
        return false;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    return dims[0] > 0 && dims[1] > 0 && dims[2] > 0;
}
}

OrthogonalCropInteractionBridgeService::OrthogonalCropInteractionBridgeService()
{
    // widget controller 只上报 bounds 和交互阶段；
    // bridge 接管预览请求构建与结果分发，保持 VTK widget 层不触碰后端业务。
    m_widgetStateController.SetBoundsCallback(
        [this](const std::array<double, 6>& worldBounds, CropInteractionPhase phase) {
            OnBoxWidget(worldBounds, phase);
        });

    m_planarWidgetStateController.SetPlaneCallback(
        [this](
            const CropVectorDouble3Array& worldOrigin,
            const CropVectorDouble3Array& worldNormal,
            CropInteractionPhase phase) {
            OnPlaneWidget(worldOrigin, worldNormal, phase);
        });
}

// 输入由 bridge 透传给 backend router；
// bridge 自身只管理交互状态，不拥有 image/polydata 后端选择逻辑。
void OrthogonalCropInteractionBridgeService::SetInputImage(
    vtkSmartPointer<vtkImageData> image,
    DataVersion version)
{
    if (!GetImageReady(image)) {
        ClearInputImage();
        return;
    }
    m_inputVersion = version;
    m_hasInputVersion = true;
    m_backend.SetInputImage(std::move(image));
}

void OrthogonalCropInteractionBridgeService::ClearInputImage()
{
    m_backend.SetInputImage(vtkSmartPointer<vtkImageData>());
    m_inputVersion = 0;
    m_hasInputVersion = false;
}

vtkSmartPointer<vtkImageData> OrthogonalCropInteractionBridgeService::GetInputImage() const
{
    return m_backend.GetInputImage();
}

void OrthogonalCropInteractionBridgeService::SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    m_boundInputPolyData = std::move(polyData);
    m_backend.SetInputPolyData(m_boundInputPolyData);
}

void OrthogonalCropInteractionBridgeService::SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    m_backend.SetPreferredDataSource(dataSource);
}

void OrthogonalCropInteractionBridgeService::SetPrimaryInteractor(vtkRenderWindowInteractor* interactor)
{
    m_primaryInteractor = interactor;
    m_widgetStateController.SetInteractor(interactor);
    m_planarWidgetStateController.SetInteractor(interactor);
}

void OrthogonalCropInteractionBridgeService::SetReferenceRenderer(vtkRenderer* renderer)
{
    m_referenceRenderer = renderer;
}

void OrthogonalCropInteractionBridgeService::SetReferenceRenderService(std::shared_ptr<AbstractInteractiveService> referenceService)
{
    m_referenceRenderService = std::move(referenceService);

    // reference service 是几何参照线框的唯一显示窗口；
    // preview target 已经存在时也要同步一次，支持上位机重新绑定 reference 窗口。
    for (auto& target : m_previewRenderTargets) {
        if (target.overlayStrategy) {
            target.overlayStrategy->SetRefVisible(target.service == m_referenceRenderService);
        }
    }
}

void OrthogonalCropInteractionBridgeService::SetPreviewRenderServices(std::vector<std::shared_ptr<AbstractInteractiveService>> previewRenderServices)
{
    ClearPreviewRenderTargets();
    m_previewRenderTargets.reserve(previewRenderServices.size());

    for (const auto& service : previewRenderServices) {
        AttachPreviewView(service);
    }

}

void OrthogonalCropInteractionBridgeService::SetSubmitReloadHandler(ReloadSubmitter reloadSubmitter)
{
    m_submitReloadHandler = std::move(reloadSubmitter);
}

void OrthogonalCropInteractionBridgeService::SendSubmit()
{
    if (!m_submitReloadHandler) {
        std::cerr << "[OrthogonalCrop] Submit failed: submit reload handler is not ready." << std::endl;
        return;
    }

    if (!GetSubmitReady()) {
        return;
    }

    m_cameraStateController.SetCameraState(m_referenceRenderer);
    ClearPreviewPolyDataInput();

    const auto submitRequest = m_currentGeometryType == OrthogonalCropGeometryType::Plane
        ? BuildPlaneRequest(OrthogonalCropOperation::Submit, OrthogonalCropDataSource::ImageData)
        : BuildBoxRequest(OrthogonalCropOperation::Submit, OrthogonalCropDataSource::ImageData);
    auto submitResult = m_backend.GetResult(submitRequest);
    if (submitResult.GetFailureReason() != OrthogonalCropFailureReason::None || !submitResult.GetSucceeded()) {
        m_cameraStateController.Clear();
        std::cerr << "[OrthogonalCrop] Submit failed: "
            << GetFailureReasonText(submitResult.GetFailureReason());
        if (!submitResult.GetMessage().empty()) {
            std::cerr << " - " << submitResult.GetMessage();
        }
        std::cerr << std::endl;
        return;
    }

    auto submitImage = submitResult.GetSubmitImage();
    if (!submitImage) {
        std::cerr << "[OrthogonalCrop] Image submit failed: output image is null." << std::endl;
        m_cameraStateController.Clear();
        m_pendingSubmitOverlayResult = OrthogonalCropResult();
        return;
    }

    submitResult.SetSubmitImage(vtkSmartPointer<vtkImageData>());
    m_pendingSubmitOverlayResult = submitResult;
    m_submitReloadPending = true;
    m_widgetStateController.SetEnabled(false);
    m_planarWidgetStateController.SetEnabled(false);

    if (!m_submitReloadHandler(
            std::move(submitImage),
            [this](bool success) {
                OnSubmitReload(success);
            })) {
        std::cerr << "[OrthogonalCrop] Submit failed: reload request was rejected." << std::endl;
        ClearPreviewPolyDataInput();
        m_pendingSubmitOverlayResult = OrthogonalCropResult();
        m_submitReloadPending = false;
        m_cameraStateController.Clear();
        if (m_cropInteractionEnabled) {
            if (m_currentGeometryType == OrthogonalCropGeometryType::Plane) {
                m_planarWidgetStateController.SetEnabled(true);
            }
            else {
                m_widgetStateController.SetEnabled(true);
            }
        }
    }
}

bool OrthogonalCropInteractionBridgeService::GetSubmitReady() const
{
    if (!m_cropInteractionEnabled || !m_worldBoundsInitialized) {
        std::cerr << "[OrthogonalCrop] Submit failed: crop widget is not active." << std::endl;
        return false;
    }

    if (m_lastInteractionPhase == CropInteractionPhase::Dragging) {
        std::cerr << "[OrthogonalCrop] Submit failed: wait until widget dragging finishes." << std::endl;
        return false;
    }

    if (!GetInputImage()) {
        std::cerr << "[OrthogonalCrop] Submit failed: image crop input is missing." << std::endl;
        return false;
    }

    if (!m_hasInputVersion) {
        std::cerr << "[OrthogonalCrop] Submit failed: image crop input version is missing." << std::endl;
        return false;
    }

    if (m_submitReloadPending) {
        std::cerr << "[OrthogonalCrop] Submit ignored: reload is already pending." << std::endl;
        return false;
    }

    return true;
}

void OrthogonalCropInteractionBridgeService::OnSubmitReload(bool success)
{
    if (!success) {
        std::cerr << "[OrthogonalCrop] Submit reload failed." << std::endl;
        ClearPreviewPolyDataInput();
        m_pendingSubmitOverlayResult = OrthogonalCropResult();
        m_submitReloadPending = false;
        m_cameraStateController.Clear();
        if (m_cropInteractionEnabled) {
            if (m_currentGeometryType == OrthogonalCropGeometryType::Plane) {
                m_planarWidgetStateController.SetEnabled(true);
            }
            else {
                m_widgetStateController.SetEnabled(true);
            }
        }
        return;
    }

    ClearPreviewPolyDataInput();

    const auto submitOverlayResult = m_pendingSubmitOverlayResult;
    m_pendingSubmitOverlayResult = OrthogonalCropResult();

    std::cout << "[OrthogonalCrop] Submit applied to host image data." << std::endl;
    m_cameraStateController.ResetCamera(m_referenceRenderer);
    if (m_submitReloadPending) {
        m_submitReloadPending = false;
        m_worldBoundsInitialized = false;
        ExitCrop();
    }

    SendResult(submitOverlayResult);
}

bool OrthogonalCropInteractionBridgeService::SwitchCropBox()
{
    if (!GetInputReady()) {
        std::cerr << "[OrthogonalCrop] Box crop trigger failed: no active image/polydata input is available yet." << std::endl;
        return false;
    }

    if (m_cropInteractionEnabled) {
        const auto previousGeometryType = m_currentGeometryType;
        if (!ExitCrop()) {
            return false;
        }
        if (previousGeometryType == OrthogonalCropGeometryType::Box) {
            return true;
        }
    }

    if (!m_primaryInteractor) {
        std::cerr << "[OrthogonalCrop] Box crop widget init failed: primary interactor missing." << std::endl;
        return false;
    }

    if (!m_worldBoundsInitialized) {
        m_currentWorldBounds = GetStartBounds();
        m_worldBoundsInitialized = true;
    }

    // 进入交互模式时，widget 使用 world bounds；真正执行时再通过 worldToInputModel 折回 active input model。
    m_widgetStateController.SetInteractor(m_primaryInteractor);
    m_widgetStateController.SetReferenceWorldBounds(GetActiveWorldBounds()); // 初始化widget交互范围
    m_widgetStateController.SetWidgetWorldBounds(m_currentWorldBounds); // 设置实际交互盒子范围有多大
    if (!m_widgetStateController.SetEnabled(true)) {
        std::cerr << "[OrthogonalCrop] Box crop widget init failed: vtkBoxWidget2 could not be enabled." << std::endl;
        return false;
    }

    m_planarWidgetStateController.SetEnabled(false);
    m_currentGeometryType = OrthogonalCropGeometryType::Box;
    m_cropInteractionEnabled = true;
    m_lastInteractionPhase = CropInteractionPhase::Released;
    ResetPreviewTargets();
    std::cout << "[OrthogonalCrop] Box crop widget active. UI uses vtkBoxWidget2, dataSource = "
        << GetDataSourceText(m_backend.GetActiveDataSource())
        << ". Use host crop commands to preview, submit, or exit." << std::endl;
    return true;
}

bool OrthogonalCropInteractionBridgeService::SwitchCropPlane()
{
    if (!GetInputReady()) {
        std::cerr << "[OrthogonalCrop] Planar crop trigger failed: no active image/polydata input is available yet." << std::endl;
        return false;
    }

    if (m_cropInteractionEnabled) {
        const auto previousGeometryType = m_currentGeometryType;
        if (!ExitCrop()) {
            return false;
        }
        if (previousGeometryType == OrthogonalCropGeometryType::Plane) {
            return true;
        }
    }

    if (!m_primaryInteractor) {
        std::cerr << "[OrthogonalCrop] Planar crop widget init failed: primary interactor missing." << std::endl;
        return false;
    }

    const auto activeWorldBounds = GetActiveWorldBounds();
    const std::array<double, 3> activeWorldDimensions = {
        activeWorldBounds[1] - activeWorldBounds[0],
        activeWorldBounds[3] - activeWorldBounds[2],
        activeWorldBounds[5] - activeWorldBounds[4]
    };
    m_currentWorldPlaneOrigin = {
        (activeWorldBounds[0] + activeWorldBounds[1]) * 0.5,
        (activeWorldBounds[2] + activeWorldBounds[3]) * 0.5,
        (activeWorldBounds[4] + activeWorldBounds[5]) * 0.5
    };
    m_currentWorldPlaneNormal = { 0.0, 0.0, 1.0 };
    m_currentWorldPlaneHalfExtents = {
        std::max(activeWorldDimensions[0] * kPlanarInitialRectangleExtentScale, kPlaneVectorEpsilon),
        std::max(activeWorldDimensions[1] * kPlanarInitialRectangleExtentScale, kPlaneVectorEpsilon)
    };
    // Plane 模式下 reference bounds 只约束 widget 的可视/交互范围；
    // halfExtents 只保留交互平面的尺度快照，不再驱动算法层生成有限矩形 outline。
    m_currentWorldBounds = activeWorldBounds;
    m_worldBoundsInitialized = true;

    m_widgetStateController.SetEnabled(false);
    m_planarWidgetStateController.SetInteractor(m_primaryInteractor);
    m_planarWidgetStateController.SetReferenceWorldBounds(activeWorldBounds);
    m_planarWidgetStateController.SetWidgetWorldPlane(
        m_currentWorldPlaneOrigin,
        m_currentWorldPlaneNormal,
        m_currentWorldPlaneHalfExtents);
    if (!m_planarWidgetStateController.SetEnabled(true)) {
        std::cerr << "[OrthogonalCrop] Planar crop widget init failed: vtkImplicitPlaneWidget2 could not be enabled." << std::endl;
        return false;
    }

    m_currentGeometryType = OrthogonalCropGeometryType::Plane;
    m_cropInteractionEnabled = true;
    m_lastInteractionPhase = CropInteractionPhase::Released;
    ResetPreviewTargets();
    std::cout << "[OrthogonalCrop] Planar crop widget active. UI uses vtkImplicitPlaneWidget2, dataSource = "
        << GetDataSourceText(m_backend.GetActiveDataSource())
        << ". Use host crop commands to preview, submit, or exit." << std::endl;
    return true;
}

bool OrthogonalCropInteractionBridgeService::ExitCrop()
{
    if (!m_cropInteractionEnabled) {
        return false;
    }

    if (m_submitReloadPending) {
        std::cout << "[OrthogonalCrop] Crop widget deactivation deferred: image submit reload is pending." << std::endl;
        return false;
    }

    m_widgetStateController.SetEnabled(false);
    m_planarWidgetStateController.SetEnabled(false);
    m_submitReloadPending = false;
    m_cropInteractionEnabled = false;
    m_previewEnabled = false;
    m_lastInteractionPhase = CropInteractionPhase::Released;
    ResetPreviewTargets();
    std::cout << "[OrthogonalCrop] Crop widget deactivated. 3D navigation restored." << std::endl;
    return true;
}

bool OrthogonalCropInteractionBridgeService::GetCropActive() const
{
    return m_cropInteractionEnabled;
}

bool OrthogonalCropInteractionBridgeService::GetInputReady()
{
    // bridge 只保证“当前至少有一个可用输入”。
    // 它不在这里做任何坐标折叠；缺 image 输入时不再反向读取 DataManager。
    // PolyData 可能是上一轮等值面 preview 临时写入 router 的输入，不能因此跳过 image 恢复。
    if (GetInputImage() || m_backend.GetInputPolyData()) {
        return true;
    }

    return false;
}

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetStartBounds() const
{
    // 默认交互盒在 world 坐标下构造：
    // 1. 先取当前活跃输入提升到 world 后的完整 bounds
    // 2. 再按中心等比缩小，得到首次拖拽更容易观察的起始盒
    // 3. 这只盒子只服务 widget；真正执行时还会折回 active input model
    std::array<double, 6> worldBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    const auto activeWorldBounds = GetActiveWorldBounds();
    if (!(activeWorldBounds[0] < activeWorldBounds[1]
        && activeWorldBounds[2] < activeWorldBounds[3]
        && activeWorldBounds[4] < activeWorldBounds[5])) {
        return worldBounds;
    }

    // 60% 不是算法约束，而是交互层的默认起始尺寸：
    // 它保留四周约 20% 的缩放/拖拽余量，同时避免初始盒过小导致用户难以选中。
    const std::array<double, 3> center = {
        (activeWorldBounds[0] + activeWorldBounds[1]) * 0.5,
        (activeWorldBounds[2] + activeWorldBounds[3]) * 0.5,
        (activeWorldBounds[4] + activeWorldBounds[5]) * 0.5
    };
    const std::array<double, 3> halfExtents = {
        (activeWorldBounds[1] - activeWorldBounds[0]) * kBoxInitialBoundsScale * 0.5,
        (activeWorldBounds[3] - activeWorldBounds[2]) * kBoxInitialBoundsScale * 0.5,
        (activeWorldBounds[5] - activeWorldBounds[4]) * kBoxInitialBoundsScale * 0.5
    };

    worldBounds[0] = center[0] - halfExtents[0];
    worldBounds[1] = center[0] + halfExtents[0];
    worldBounds[2] = center[1] - halfExtents[1];
    worldBounds[3] = center[1] + halfExtents[1];
    worldBounds[4] = center[2] - halfExtents[2];
    worldBounds[5] = center[2] + halfExtents[2];
    return worldBounds;
}

void OrthogonalCropInteractionBridgeService::OnBoxWidget(
    const std::array<double, 6>& worldBounds,
    CropInteractionPhase phase)
{
    if (!m_cropInteractionEnabled || m_currentGeometryType != OrthogonalCropGeometryType::Box) {
        return;
    }

    // 交互过程中始终记录最新 world AABB 和 phase，
    // 但只在 Released 时触发 preview，保持“拖拽只更新显示状态，释放后再统一跑后端”的既有流程。
    m_currentWorldBounds = worldBounds;
    m_worldBoundsInitialized = true;
    m_lastInteractionPhase = phase;
    if (m_previewEnabled && phase == CropInteractionPhase::Released) {
        SetBoxPreview();
    }
}

void OrthogonalCropInteractionBridgeService::OnPlaneWidget(
    const CropVectorDouble3Array& worldOrigin,
    const CropVectorDouble3Array& worldNormal,
    CropInteractionPhase phase)
{
    if (!m_cropInteractionEnabled || m_currentGeometryType != OrthogonalCropGeometryType::Plane) {
        return;
    }

    m_currentWorldPlaneOrigin = worldOrigin;
    m_currentWorldPlaneNormal = worldNormal;
    m_worldBoundsInitialized = true;
    m_lastInteractionPhase = phase;
    if (m_previewEnabled && phase == CropInteractionPhase::Released) {
        SetPlanePreview();
    }
}

std::array<double, 6> OrthogonalCropInteractionBridgeService::GetWorldBounds(
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

    return GetWorldBounds(activeInputModelBounds);
}

std::array<double, 16> OrthogonalCropInteractionBridgeService::GetWorldToInput() const
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

OrthogonalCropRequest OrthogonalCropInteractionBridgeService::BuildBoxRequest(
    OrthogonalCropOperation operation,
    OrthogonalCropDataSource dataSource) const
{
    // 先获取当前数据源的默认 request；
    // image 与 polydata 的 bounds 归一化由后端入口收口，bridge 只补交互盒姿态。
    auto boxRequest = m_backend.GetDefaultRequest();
    boxRequest.SetDataSource(dataSource);
    boxRequest.SetOperation(operation);
    boxRequest.SetGeometryType(OrthogonalCropGeometryType::Box);
    boxRequest.SetRemovalMode(m_currentRemovalMode);

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
        return boxRequest;
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
    worldToActiveInputModelMatrix->DeepCopy(GetWorldToInput().data());

    // boxToInputModelMatrix 是 request 下发给算法层的唯一几何真源；
    // 算法只消费标准盒到 active input model 的矩阵，不再依赖 widget 或 world 状态。
    auto boxToInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(worldToActiveInputModelMatrix, boxToWorldMatrix, boxToInputModelMatrix);
    vtkMatrix4x4::DeepCopy(boxToInputModelMatrixData.data(), boxToInputModelMatrix);

    boxRequest.SetBoxMatrix(boxToInputModelMatrixData);
    return boxRequest;
}

OrthogonalCropRequest OrthogonalCropInteractionBridgeService::BuildPlaneRequest(
    OrthogonalCropOperation operation,
    OrthogonalCropDataSource dataSource) const
{
    OrthogonalCropRequest planeRequest;
    planeRequest.SetDataSource(dataSource);
    planeRequest.SetOperation(operation);
    planeRequest.SetGeometryType(OrthogonalCropGeometryType::Plane);
    planeRequest.SetRemovalMode(m_currentRemovalMode);

    CropVectorDouble3Array worldOrigin = m_currentWorldPlaneOrigin;
    CropVectorDouble3Array worldNormal = m_currentWorldPlaneNormal;
    if (m_planarWidgetStateController.GetCurrentWorldPlane(worldOrigin, worldNormal)) {
        // 以 widget controller 的即时状态为准；缓存值只作为 widget 不可读时的兜底。
    }

    auto worldToActiveInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToActiveInputModelMatrix->DeepCopy(GetWorldToInput().data());

    const double worldOriginPoint[4] = { worldOrigin[0], worldOrigin[1], worldOrigin[2], 1.0 };
    double inputModelOriginPoint[4] = { 0.0, 0.0, 0.0, 1.0 };
    worldToActiveInputModelMatrix->MultiplyPoint(worldOriginPoint, inputModelOriginPoint);
    const double invW = std::abs(inputModelOriginPoint[3]) > kPlaneVectorEpsilon
        ? 1.0 / inputModelOriginPoint[3]
        : 1.0;

    CropVectorDouble3Array planeCenterInInputModel = {
        inputModelOriginPoint[0] * invW,
        inputModelOriginPoint[1] * invW,
        inputModelOriginPoint[2] * invW
    };

    auto activeInputModelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(worldToActiveInputModelMatrix, activeInputModelToWorldMatrix);
    activeInputModelToWorldMatrix->Transpose();

    const double worldNormalVector[4] = { worldNormal[0], worldNormal[1], worldNormal[2], 0.0 };
    double inputModelNormalVector[4] = { 0.0, 0.0, 1.0, 0.0 };
    activeInputModelToWorldMatrix->MultiplyPoint(worldNormalVector, inputModelNormalVector);

    CropVectorDouble3Array planeNormalInInputModel = {
        inputModelNormalVector[0],
        inputModelNormalVector[1],
        inputModelNormalVector[2]
    };
    if (vtkMath::Normalize(planeNormalInInputModel.data()) <= kPlaneVectorEpsilon) {
        planeNormalInInputModel = { 0.0, 0.0, 1.0 };
    }

    double worldNormalData[3] = { worldNormal[0], worldNormal[1], worldNormal[2] };
    if (vtkMath::Normalize(worldNormalData) <= kPlaneVectorEpsilon) {
        worldNormalData[0] = 0.0;
        worldNormalData[1] = 0.0;
        worldNormalData[2] = 1.0;
    }

    double referenceUp[3] = { 0.0, 1.0, 0.0 };
    if (std::abs(vtkMath::Dot(worldNormalData, referenceUp)) > 0.99) {
        referenceUp[0] = 0.0;
        referenceUp[1] = 0.0;
        referenceUp[2] = 1.0;
    }

    double worldPlaneAxisWidth[3] = { 0.0, 0.0, 0.0 };
    double worldPlaneAxisHeight[3] = { 0.0, 0.0, 0.0 };
    vtkMath::Cross(worldNormalData, referenceUp, worldPlaneAxisWidth);
    if (vtkMath::Normalize(worldPlaneAxisWidth) <= kPlaneVectorEpsilon) {
        worldPlaneAxisWidth[0] = 1.0;
        worldPlaneAxisWidth[1] = 0.0;
        worldPlaneAxisWidth[2] = 0.0;
    }
    vtkMath::Cross(worldNormalData, worldPlaneAxisWidth, worldPlaneAxisHeight);
    if (vtkMath::Normalize(worldPlaneAxisHeight) <= kPlaneVectorEpsilon) {
        worldPlaneAxisHeight[0] = 0.0;
        worldPlaneAxisHeight[1] = 1.0;
        worldPlaneAxisHeight[2] = 0.0;
    }

    double worldAxisWidthPoint[4] = {
        worldOrigin[0] + worldPlaneAxisWidth[0] * m_currentWorldPlaneHalfExtents[0],
        worldOrigin[1] + worldPlaneAxisWidth[1] * m_currentWorldPlaneHalfExtents[0],
        worldOrigin[2] + worldPlaneAxisWidth[2] * m_currentWorldPlaneHalfExtents[0],
        1.0
    };
    double worldAxisHeightPoint[4] = {
        worldOrigin[0] + worldPlaneAxisHeight[0] * m_currentWorldPlaneHalfExtents[1],
        worldOrigin[1] + worldPlaneAxisHeight[1] * m_currentWorldPlaneHalfExtents[1],
        worldOrigin[2] + worldPlaneAxisHeight[2] * m_currentWorldPlaneHalfExtents[1],
        1.0
    };
    double inputModelAxisWidthPoint[4] = { 0.0, 0.0, 0.0, 1.0 };
    double inputModelAxisHeightPoint[4] = { 0.0, 0.0, 0.0, 1.0 };
    worldToActiveInputModelMatrix->MultiplyPoint(worldAxisWidthPoint, inputModelAxisWidthPoint);
    worldToActiveInputModelMatrix->MultiplyPoint(worldAxisHeightPoint, inputModelAxisHeightPoint);

    const double widthInvW = std::abs(inputModelAxisWidthPoint[3]) > kPlaneVectorEpsilon
        ? 1.0 / inputModelAxisWidthPoint[3]
        : 1.0;
    const double heightInvW = std::abs(inputModelAxisHeightPoint[3]) > kPlaneVectorEpsilon
        ? 1.0 / inputModelAxisHeightPoint[3]
        : 1.0;
    const double inputModelAxisWidthVector[3] = {
        inputModelAxisWidthPoint[0] * widthInvW - planeCenterInInputModel[0],
        inputModelAxisWidthPoint[1] * widthInvW - planeCenterInInputModel[1],
        inputModelAxisWidthPoint[2] * widthInvW - planeCenterInInputModel[2]
    };
    const double inputModelAxisHeightVector[3] = {
        inputModelAxisHeightPoint[0] * heightInvW - planeCenterInInputModel[0],
        inputModelAxisHeightPoint[1] * heightInvW - planeCenterInInputModel[1],
        inputModelAxisHeightPoint[2] * heightInvW - planeCenterInInputModel[2]
    };
    const std::array<double, 2> planeHalfExtentsInInputModel = {
        std::max(vtkMath::Norm(inputModelAxisWidthVector), kPlaneVectorEpsilon),
        std::max(vtkMath::Norm(inputModelAxisHeightVector), kPlaneVectorEpsilon)
    };

    planeRequest.SetPlaneCenter(planeCenterInInputModel);
    planeRequest.SetPlaneNormal(planeNormalInInputModel);
    // halfExtents 继续随 request 下发，是为了保留 widget 尺度这一层级信息；
    // 裁切数学只使用 center + normal 的无限半空间，避免再次出现误导性的平面方框。
    planeRequest.SetPlaneHalf(planeHalfExtentsInInputModel);
    return planeRequest;
}

void OrthogonalCropInteractionBridgeService::SetBoxPreview()
{
    if (!m_worldBoundsInitialized) {
        return;
    }

    const auto volumeRequest = BuildBoxRequest(
        OrthogonalCropOperation::Preview,
        OrthogonalCropDataSource::VolumeData);
    auto polyDataRequest = volumeRequest;
    polyDataRequest.SetDataSource(OrthogonalCropDataSource::PolyData);

    SetPreviewReq(volumeRequest, polyDataRequest);
}

void OrthogonalCropInteractionBridgeService::SetPlanePreview()
{
    if (!m_worldBoundsInitialized) {
        return;
    }

    const auto volumeRequest = BuildPlaneRequest(
        OrthogonalCropOperation::Preview,
        OrthogonalCropDataSource::VolumeData);
    auto polyDataRequest = volumeRequest;
    polyDataRequest.SetDataSource(OrthogonalCropDataSource::PolyData);

    SetPreviewReq(volumeRequest, polyDataRequest);
}

void OrthogonalCropInteractionBridgeService::SetPreviewReq(
    const OrthogonalCropRequest& volumeRequest,
    const OrthogonalCropRequest& polyDataRequest)
{
    ClearPreviewPolyDataInput();

    bool hasMain3DPreviewTarget = false;
    for (const auto& target : m_previewRenderTargets) {
        if (!target.service) {
            continue;
        }

        if (target.service->GetNavigationAxis() < 0) {
            hasMain3DPreviewTarget = true;
        }
    }

    OrthogonalCropResult volumeResult;
    bool hasVolumeResult = false;
    if (GetInputImage() && hasMain3DPreviewTarget) {
        volumeResult = m_backend.GetResult(volumeRequest);
        if (volumeResult.GetFailureReason() == OrthogonalCropFailureReason::None
            && volumeResult.GetSucceeded()) {
            hasVolumeResult = true;
        }
    }

    bool anyPreviewApplied = false;
    for (const auto& target : m_previewRenderTargets) {
        if (!target.service || !target.overlayStrategy) {
            continue;
        }

        OrthogonalCropResult polyResult;
        bool hasPolyDataResult = false;
        m_previewPlug.ResetPreview(target.service, target.overlayStrategy);

        const bool isMain3DTarget = target.service->GetNavigationAxis() < 0;
        const OrthogonalCropResult* volumePreviewResult = isMain3DTarget && hasVolumeResult
            ? &volumeResult
            : nullptr;

        if (auto polyData = m_previewPlug.GetPreviewData(target.service)) {
            m_backend.SetInputPolyData(polyData);

            polyResult = m_backend.GetResult(polyDataRequest);
            ClearPreviewPolyDataInput();
            // PolyData preview 是 render-only 主显示路径；
            // 有效性由 result 成功与否决定，不再强制要求 clipPolyData artifact。
            if (polyResult.GetFailureReason() == OrthogonalCropFailureReason::None
                && polyResult.GetSucceeded()) {
                hasPolyDataResult = true;
            }
        }

        if (!volumePreviewResult && !hasPolyDataResult) {
            continue;
        }

        SendPreview(
            target,
            m_currentRemovalMode,
            volumePreviewResult,
            hasPolyDataResult ? &polyResult : nullptr);
        anyPreviewApplied = true;
    }

    if (!anyPreviewApplied) {
        ClearPreviewPolyDataInput();
        return;
    }

    ClearPreviewPolyDataInput();
}

void OrthogonalCropInteractionBridgeService::SwitchPreview(CropRemovalMode removalMode)
{
    if (m_currentGeometryType == OrthogonalCropGeometryType::Plane) {
        SetPlaneView(removalMode);
        return;
    }

    SwitchBoxView(removalMode);
}

void OrthogonalCropInteractionBridgeService::SwitchBoxView(CropRemovalMode removalMode)
{
    // Box 继续保留旧 toggle 语义：
    // A. 同模式再次触发：关闭 preview，并恢复主模型全量显示
    // B. 切到新模式：更新 removal mode；若当前不在 dragging，则立即刷新一次 preview
    if (!m_cropInteractionEnabled) {
        return;
    }

    if (m_previewEnabled && m_currentRemovalMode == removalMode) {
        // 再次触发同一模式表示关闭 preview，并恢复原始显示内容。
        m_previewEnabled = false;
        ResetPreviewTargets();
        return;
    }

    m_previewEnabled = true;
    m_currentRemovalMode = removalMode;

    if (m_cropInteractionEnabled
        && m_lastInteractionPhase != CropInteractionPhase::Dragging) {
        // 非拖拽阶段才立即刷新；拖拽中依旧等待 EndInteractionEvent 统一触发。
        SetBoxPreview();
    }
}

void OrthogonalCropInteractionBridgeService::SetPlaneView(CropRemovalMode removalMode)
{
    // Plane 模式下 1/2 是明确的半空间选择；
    // 再次按下当前激活的模式键表示关闭预览并恢复主模型，给用户明确的复原入口。
    if (!m_cropInteractionEnabled || m_currentGeometryType != OrthogonalCropGeometryType::Plane) {
        return;
    }

    if (m_previewEnabled && m_currentRemovalMode == removalMode) {
        m_previewEnabled = false;
        ResetPreviewTargets();
        return;
    }

    m_previewEnabled = true;
    m_currentRemovalMode = removalMode;
    if (m_lastInteractionPhase != CropInteractionPhase::Dragging) {
        SetPlanePreview();
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
    case OrthogonalCropFailureReason::SubmitMaskCreationFailed:
        return "SubmitMaskCreationFailed";
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

void OrthogonalCropInteractionBridgeService::ClearPreviewRenderTargets()
{
    // 这里只清理 overlay 生命周期；
    // 主模型 preview 状态由 ResetPreviewTargets 收口，避免解绑窗口时意外改动主显示。
    for (const auto& target : m_previewRenderTargets) {
        if (target.service && target.overlayStrategy) {
            target.service->RemoveOverlayStrategy(target.overlayStrategy);
        }
    }
    m_previewPlug.Clear();
    m_previewRenderTargets.clear();
    ClearPreviewPolyDataInput();
}

void OrthogonalCropInteractionBridgeService::ResetPreviewTargets()
{
    // 恢复 preview 涉及 overlay、volume shader 和 polydata clip 三条显示路径；
    // 统一在这里清空临时状态并标脏，确保关闭 preview 后各窗口回到全模型显示。
    for (auto& target : m_previewRenderTargets) {
        m_previewPlug.ResetPreview(target.service, target.overlayStrategy);
        if (target.service) {
            target.service->MarkDirty();
        }
    }
    ClearPreviewPolyDataInput();
}

void OrthogonalCropInteractionBridgeService::ClearPreviewPolyDataInput()
{
    if (m_boundInputPolyData) {
        m_backend.SetInputPolyData(m_boundInputPolyData);
        return;
    }

    m_backend.ClearInputPolyData();
}

void OrthogonalCropInteractionBridgeService::AttachPreviewView(const std::shared_ptr<AbstractInteractiveService>& service)
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
    // preview 目标可以有多个，但裁切 box/outline 只属于 reference 窗口。
    // 这样其它 3D 窗口仍能显示裁切后的主模型预览，不会再出现第二个可误读为交互控件的 box。
    overlayStrategy->SetRefVisible(service == m_referenceRenderService);
    service->AddOverlayStrategy(overlayStrategy);
    m_previewRenderTargets.push_back({ service, overlayStrategy });
}

bool OrthogonalCropInteractionBridgeService::SendPreview(
    const PreviewRenderTarget& target,
    CropRemovalMode removalMode,
    const OrthogonalCropResult* volumePreviewResult,
    const OrthogonalCropResult* polyDataPreviewResult)
{
    if (!target.service || !target.overlayStrategy) {
        return false;
    }

    const bool mainPreviewApplied = m_previewPlug.SetPreview(
        target.service,
        target.overlayStrategy,
        m_referenceRenderService,
        volumePreviewResult,
        polyDataPreviewResult,
        removalMode);

    target.service->MarkDirty();
    return mainPreviewApplied;
}

void OrthogonalCropInteractionBridgeService::SendResult(const OrthogonalCropResult& submitResult)
{
    if (!submitResult.GetSucceeded()) {
        return;
    }

    for (const auto& target : m_previewRenderTargets) {
        if (!target.service || !target.overlayStrategy) {
            continue;
        }

        target.overlayStrategy->SetSliceAxis(target.service->GetNavigationAxis());
        target.overlayStrategy->SetRemovalMode(submitResult.GetResolvedRemovalMode());
        target.overlayStrategy->SetCropResult(submitResult);
        target.service->MarkDirty();
    }
}
