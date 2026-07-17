// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/src/Interaction/CropBridge.cpp
// 分类: Service / Interaction Bridge Implementation
// 说明: 维护交互态，构造 preview/submit request，把预览结果分发给 overlay 与 3D 主模型，
//       并把 submit 结果交给主数据 reload 通道。
// =====================================================================

#include "Interaction/CropBridge.h"

#include "Algorithms/OrthogonalCropAlgorithm.h"
#include "AppInterfaces.h"
#include "Interaction/CropBoxWidget.h"
#include "Interaction/CropPlaneWidget.h"
#include "Preview/CropPreviewPlug.h"
#include "Render/Strategies/CropOverlay.h"
#include "Routing/CropRouter.h"

#include <vtkBoundingBox.h>
#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkTransform.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <utility>

static constexpr double kBoxInitialBoundsScale = 0.60;
static constexpr double kPlaneScale = 0.35;
static constexpr double kPlaneVectorEpsilon = 1e-12;

class CropBridge::Impl final {
public:
    using ReloadSubmitter = CropBridge::ReloadSubmitter;

    Impl();
    ~Impl();

    void SetInputImage(vtkSmartPointer<vtkImageData> image);
    void ClearInputImage();
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);
    void SetPrimaryInteractor(vtkRenderWindowInteractor* interactor);
    void SetReferenceRenderer(vtkRenderer* renderer);
    void SetReferenceRenderService(std::shared_ptr<InteractiveService> referenceService);
    void SetPreviewRenderServices(std::vector<std::shared_ptr<InteractiveService>> previewRenderServices);
    void SetSubmitReloadHandler(ReloadSubmitter reloadSubmitter);
    bool StartView(CropViewRequest request);
    void ClearBindings();
    bool SwitchCropBox();
    bool SwitchCropPlane();
    bool ExitCrop();
    bool GetCropActive() const;
    void SwitchPreview(CropRemovalMode removalMode);
    bool SendSubmit(std::function<void(bool isSuccess)> onComplete);

private:
    enum class CropSessionPhase {
        Idle,
        Editing,
        Reloading
    };

    struct ReloadGate {
        std::mutex mutex;
        Impl* owner = nullptr;
        std::size_t generation = 0;
    };

    bool GetInputReady();
    void ClearPreviewInput();
    void AttachPreview(const std::shared_ptr<InteractiveService>& service);
    std::array<double, 6> GetStartBounds() const;
    std::array<double, 6> GetWorldBounds(const std::array<double, 6>& activeInputModelBounds) const;
    std::array<double, 6> GetActiveWorldBounds() const;
    std::array<double, 16> GetWorldToInput() const;
    void OnBoxWidget(const std::array<double, 6>& worldBounds, CropInteractionPhase phase);
    void OnPlaneWidget(
        const CropVectorDouble3Array& worldOrigin,
        const CropVectorDouble3Array& worldNormal,
        CropInteractionPhase phase);
    OrthogonalCropRequest BuildBoxRequest(
        OrthogonalCropOperation operation,
        OrthogonalCropDataSource dataSource) const;
    OrthogonalCropRequest BuildPlaneRequest(
        OrthogonalCropOperation operation,
        OrthogonalCropDataSource dataSource) const;
    bool SendPreview();
    void ClearPreviewViews();
    void ResetPreview();
    bool GetSubmitReady() const;
    void OnSubmitReload(bool isSuccess);
    bool BuildCameraState();
    bool ResetCamera();
    bool ClearCamera();
    static const char* GetFailureReasonText(CropFailure failureReason);
    static const char* GetDataSourceText(OrthogonalCropDataSource dataSource);

    // bridge 值拥有的业务 router；长期 image/polydata 输入及 preview/submit 执行都收口到该实例。
    CropRouter m_backend;
    // 无持久成员的显示适配器；裁切临时状态写入目标 mapper/shader，并由 ResetPreview 对称清理。
    CropPreviewPlug m_previewPlug;
    // m_boundInputPolyData 是 host 绑定的长期输入；临时 preview mesh 写入 router 后由 ClearPreviewInput 恢复它。
    vtkSmartPointer<vtkPolyData> m_boundInputPolyData;
    // [风险] 非拥有主交互器；宿主必须保证任一 widget 启用期间有效，并在窗口销毁前解除绑定，
    // 否则后续 Switch* 重新注入 widget 时会使用悬垂地址。
    vtkRenderWindowInteractor* m_primaryInteractor = nullptr;
    // 交互会话轴：编辑与 reload 是互斥阶段，不再允许 active/reload 布尔值组合出非法状态。
    CropSessionPhase m_sessionPhase = CropSessionPhase::Idle;
    // 当前激活 widget 和 request 的几何轴；crop 关闭后保留最后选择，下次 Switch* 会显式覆盖。
    CropShape m_currentGeometryType = CropShape::Box;
    // 几何可用轴：widget 回调或模式初始化后置位；成功 reload 后清零，迫使下一轮重新取输入 bounds。
    bool m_hasWorldState = false;
    // 预览意图轴：同模式命令可关闭；它不表示任一 target 已成功应用预览。
    bool m_isPreviewOn = false;
    // 最近交互阶段由 widget 回调更新；Dragging 阻止重型 preview 和 submit，Released 允许消费当前几何。
    CropInteractionPhase m_lastInteractionPhase = CropInteractionPhase::Idle;
    // preview 与随后 submit 共用的 Inside 保留轴；切换 preview 模式时更新，默认保留几何内部。
    CropRemovalMode m_currentRemovalMode = CropRemovalMode::KeepInside;
    // Box 的 world AABB 缓存，布局 [minX,maxX,minY,maxY,minZ,maxZ]；request 构造时折回 active input model。
    std::array<double, 6> m_currentWorldBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // Plane 的 world 几何真源；origin 为平面上一点，normal 由 widget 路径保持单位化并定义 Inside 正侧。
    CropVectorDouble3Array m_currentWorldPlaneOrigin = { 0.0, 0.0, 0.0 };
    CropVectorDouble3Array m_currentWorldPlaneNormal = { 0.0, 0.0, 1.0 };
    // 平面 widget 的 world 可视半尺寸 [halfWidth,halfHeight]；不限制后端无限半空间方程。
    std::array<double, 2> m_worldHalf = { 1.0, 1.0 };
    // 宿主注入并由 std::function 持有的 reload 能力；替换/清空时释放 closure，接受请求后负责回传完成结果。
    ReloadSubmitter m_submitReloadHandler;
    // reload pending 期间只保留不含 submit image 的结果元数据，成功后用于分发各 target 的 overlay。
    OrthogonalCropResult m_submitOverlay;
    std::function<void(bool)> m_submitCallback;
    // submit 前缓存 reference renderer 相机值快照；不持有 renderer/camera 所有权。
    CameraStateSnapshot m_submitCameraState;
    // completion 通过弱引用进入 gate；析构在同一 mutex 下清 owner，避免延迟回调访问已销毁 Impl。
    std::shared_ptr<ReloadGate> m_reloadGate;
    // bridge 值拥有两种 widget 控制器及其 VTK callback/representation 生命周期；任一时刻只启用当前几何对应项。
    CropBoxWidget m_boxWidget;
    CropPlaneWidget m_planeWidget;
    // 几何参照窗口 service 的共享 owner；提供 active input model -> world，且决定 reference overlay 可见性。
    std::shared_ptr<InteractiveService> m_referenceRenderService;
    // [风险] 非拥有 renderer，仅用于相机快照/恢复；宿主必须让它与 reference service 同步换绑，
    // 并保证 submit 完成回调前仍有效，否则相机收尾会访问失效视图。
    vtkRenderer* m_referenceRenderer = nullptr;
    // 每个 preview 目标的 [service, overlay] 共享 owner；ClearPreviewViews 先 Remove 再清空集合。
    std::vector<std::pair<std::shared_ptr<InteractiveService>, std::shared_ptr<CropOverlay>>> m_previewRenderTargets;
};

CropBridge::Impl::Impl()
    : m_reloadGate(std::make_shared<ReloadGate>())
{
    m_reloadGate->owner = this;
    // widget controller 只上报 bounds 和交互阶段；
    // bridge 接管预览请求构建与结果分发，保持 VTK widget 层不触碰后端业务。
    m_boxWidget.SetBoundsCallback(
        [this](const std::array<double, 6>& worldBounds, CropInteractionPhase phase) {
            OnBoxWidget(worldBounds, phase);
        });

    m_planeWidget.SetPlaneCallback(
        [this](
            const CropVectorDouble3Array& worldOrigin,
            const CropVectorDouble3Array& worldNormal,
            CropInteractionPhase phase) {
            OnPlaneWidget(worldOrigin, worldNormal, phase);
        });
}

CropBridge::Impl::~Impl()
{
    std::lock_guard<std::mutex> lock(m_reloadGate->mutex);
    m_reloadGate->owner = nullptr;
    ++m_reloadGate->generation;
}

// 输入由 bridge 透传给 backend router；
// bridge 自身只管理交互状态，不拥有 image/polydata 后端选择逻辑。
void CropBridge::Impl::SetInputImage(
    vtkSmartPointer<vtkImageData> image)
{
    if (!image || !image->GetScalarPointer()) {
        ClearInputImage();
        return;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        ClearInputImage();
        return;
    }

    m_backend.SetInputImage(std::move(image));
}

void CropBridge::Impl::ClearInputImage()
{
    m_backend.SetInputImage(vtkSmartPointer<vtkImageData>());
}

void CropBridge::Impl::SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    m_boundInputPolyData = std::move(polyData);
    m_backend.SetInputPolyData(m_boundInputPolyData);
}

void CropBridge::Impl::SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    m_backend.SetPreferredDataSource(dataSource);
}

bool CropBridge::Impl::GetInputReady()
{
    // bridge 只保证“当前至少有一个可用输入”。
    // 它不在这里做任何坐标折叠；缺 image 输入时不再反向读取 DataManager。
    // PolyData 可能是上一轮等值面 preview 临时写入 router 的输入，不能因此跳过 image 恢复。
    if (m_backend.GetImageReady() || m_backend.GetPolyDataReady()) {
        return true;
    }

    return false;
}

void CropBridge::Impl::ClearPreviewInput()
{
    if (m_boundInputPolyData) {
        m_backend.SetInputPolyData(m_boundInputPolyData);
        return;
    }

    m_backend.ClearInputPolyData();
}

void CropBridge::Impl::SetPrimaryInteractor(vtkRenderWindowInteractor* interactor)
{
    m_primaryInteractor = interactor;
    m_boxWidget.SetInteractor(interactor);
    m_planeWidget.SetInteractor(interactor);
}

void CropBridge::Impl::SetReferenceRenderer(vtkRenderer* renderer)
{
    m_referenceRenderer = renderer;
}

void CropBridge::Impl::SetReferenceRenderService(std::shared_ptr<InteractiveService> referenceService)
{
    m_referenceRenderService = std::move(referenceService);

    // reference service 是几何参照线框的唯一显示窗口；
    // preview target 已经存在时也要同步一次，支持上位机重新绑定 reference 窗口。
    for (auto& target : m_previewRenderTargets) {
        if (target.second) {
            target.second->SetRefVisible(target.first == m_referenceRenderService);
        }
    }
}

void CropBridge::Impl::SetPreviewRenderServices(std::vector<std::shared_ptr<InteractiveService>> previewRenderServices)
{
    ClearPreviewViews();
    m_previewRenderTargets.reserve(previewRenderServices.size());

    for (const auto& service : previewRenderServices) {
        AttachPreview(service);
    }

}

void CropBridge::Impl::SetSubmitReloadHandler(ReloadSubmitter reloadSubmitter)
{
    m_submitReloadHandler = std::move(reloadSubmitter);
}

bool CropBridge::Impl::StartView(CropViewRequest request)
{
    if (!request.inputImage || !request.interactor || !request.renderer
        || !request.referenceService) {
        return false;
    }
    SetInputImage(std::move(request.inputImage));
    SetInputPolyData(std::move(request.inputPolyData));
    SetPreferredDataSource(request.dataSource);
    SetPrimaryInteractor(request.interactor);
    SetReferenceRenderer(request.renderer);
    SetReferenceRenderService(std::move(request.referenceService));
    SetPreviewRenderServices(std::move(request.previewServices));
    return GetInputReady();
}

void CropBridge::Impl::ClearBindings()
{
    {
        std::lock_guard<std::mutex> lock(m_reloadGate->mutex);
        ++m_reloadGate->generation;
    }
    m_submitCallback = nullptr;
    m_submitReloadHandler = nullptr;
    m_boxWidget.SetEnabled(false);
    m_planeWidget.SetEnabled(false);
    m_sessionPhase = CropSessionPhase::Idle;
    m_isPreviewOn = false;
    ResetPreview();
    ClearPreviewViews();
    ClearInputImage();
    SetInputPolyData(nullptr);
    SetPrimaryInteractor(nullptr);
    SetReferenceRenderer(nullptr);
    SetReferenceRenderService(nullptr);
}

void CropBridge::Impl::AttachPreview(const std::shared_ptr<InteractiveService>& service)
{
    if (!service) {
        return;
    }

    // 每个窗口只允许挂一份 overlay strategy；重复 service 直接跳过，避免重复 prop 和重复设脏。
    const auto sameTarget = std::find_if(
        m_previewRenderTargets.begin(),
        m_previewRenderTargets.end(),
        [service](const std::pair<std::shared_ptr<InteractiveService>, std::shared_ptr<CropOverlay>>& target) {
            return target.first == service;
        });
    if (sameTarget != m_previewRenderTargets.end()) {
        return;
    }

    auto overlayStrategy = std::make_shared<CropOverlay>();
    // preview 目标可以有多个，但裁切 box/outline 只属于 reference 窗口。
    // 这样其它 3D 窗口仍能显示裁切后的主模型预览，不会再出现第二个可误读为交互控件的 box。
    overlayStrategy->SetRefVisible(service == m_referenceRenderService);
    service->AttachOverlayStrategy(overlayStrategy);
    m_previewRenderTargets.push_back({ service, overlayStrategy });
}

std::array<double, 6> CropBridge::Impl::GetStartBounds() const
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

std::array<double, 6> CropBridge::Impl::GetWorldBounds(
    const std::array<double, 6>& activeInputModelBounds) const
{
    // 这是纯 bounds 级别的 8 角点变换 helper：
    // 输入是 active input model 轴对齐盒，输出是 widget 使用的 world AABB。
    // 对 image 路径，input model 底层由 VTK physical-point API 表达。
    // 显式变换 8 个角点后再回收包围盒，可以避免旋转场景下的逐轴换算失真。
    if (!m_referenceRenderService) {
        return activeInputModelBounds;
    }

    auto inputToWorldMat = vtkSmartPointer<vtkMatrix4x4>::New();
    inputToWorldMat->DeepCopy(m_referenceRenderService->GetModelMatrix().data());

    auto inputToWorld = vtkSmartPointer<vtkTransform>::New();
    inputToWorld->SetMatrix(inputToWorldMat);

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
                inputToWorld->TransformPoint(activeInputModelCorner, worldCorner);
                worldBounds.AddPoint(worldCorner);
            }
        }
    }

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    worldBounds.GetBounds(bounds);
    return { bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5] };
}

std::array<double, 6> CropBridge::Impl::GetActiveWorldBounds() const
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

std::array<double, 16> CropBridge::Impl::GetWorldToInput() const
{
    if (!m_referenceRenderService) {
        return CropGeometryAlgorithm::GetIdentityMatrix();
    }

    // 参考窗口维护的是 active input model -> world 矩阵；裁切 request 需要反向矩阵，
    // 否则 widget 盒和后端 active input model 会落在两个不同坐标系里。
    // 对 image 路径，active input model 底层由 VTK physical-point API 表达；
    // 对 polydata 路径，则是主网格自己的 input model 坐标。
    auto worldToInputMat = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToInputMat->DeepCopy(m_referenceRenderService->GetModelMatrix().data());
    worldToInputMat->Invert();

    std::array<double, 16> worldToInputData = { 0.0 };
    vtkMatrix4x4::DeepCopy(worldToInputData.data(), worldToInputMat);
    return worldToInputData;
}

void CropBridge::Impl::OnBoxWidget(
    const std::array<double, 6>& worldBounds,
    CropInteractionPhase phase)
{
    if (m_sessionPhase != CropSessionPhase::Editing
        || m_currentGeometryType != CropShape::Box) {
        return;
    }

    // 交互过程中始终记录最新 world AABB 和 phase，
    // 但只在 Released 时触发 preview，保持“拖拽只更新显示状态，释放后再统一跑后端”的既有流程。
    m_currentWorldBounds = worldBounds;
    m_hasWorldState = true;
    m_lastInteractionPhase = phase;
    if (m_isPreviewOn && phase == CropInteractionPhase::Released) {
        (void)SendPreview();
    }
}

void CropBridge::Impl::OnPlaneWidget(
    const CropVectorDouble3Array& worldOrigin,
    const CropVectorDouble3Array& worldNormal,
    CropInteractionPhase phase)
{
    if (m_sessionPhase != CropSessionPhase::Editing
        || m_currentGeometryType != CropShape::Plane) {
        return;
    }

    // 与 Box 共用“持续记录、仅 Released 消费”的交互契约；缓存值仍保持 world 坐标。
    m_currentWorldPlaneOrigin = worldOrigin;
    m_currentWorldPlaneNormal = worldNormal;
    m_hasWorldState = true;
    m_lastInteractionPhase = phase;
    if (m_isPreviewOn && phase == CropInteractionPhase::Released) {
        (void)SendPreview();
    }
}

OrthogonalCropRequest CropBridge::Impl::BuildBoxRequest(
    OrthogonalCropOperation operation,
    OrthogonalCropDataSource dataSource) const
{
    // 先获取当前数据源的默认 request；
    // image 与 polydata 的 bounds 归一化由后端入口收口，bridge 只补交互盒姿态。
    auto boxRequest = m_backend.GetDefaultRequest();
    boxRequest.dataSource = dataSource;
    boxRequest.operation = operation;
    boxRequest.geometryType = CropShape::Box;
    boxRequest.removalMode = m_currentRemovalMode;

    // 准备 widget 有向盒的 world 基准信息；
    // 默认值来自当前 world AABB，GetCurrentWorldBox 成功时会替换成最近一次 PlaceWidget 的基准盒。
    CropVectorDouble3Array baseCenter = {
        (m_currentWorldBounds[0] + m_currentWorldBounds[1]) * 0.5,
        (m_currentWorldBounds[2] + m_currentWorldBounds[3]) * 0.5,
        (m_currentWorldBounds[4] + m_currentWorldBounds[5]) * 0.5
    };
    CropVectorDouble3Array baseSize = {
        m_currentWorldBounds[1] - m_currentWorldBounds[0],
        m_currentWorldBounds[3] - m_currentWorldBounds[2],
        m_currentWorldBounds[5] - m_currentWorldBounds[4]
    };
    CropMatrixDouble16Array boxToInputModelMatrixData = CropGeometryAlgorithm::GetIdentityMatrix();
    CropMatrixDouble16Array baseToNowData = CropGeometryAlgorithm::GetIdentityMatrix();

    // 读取 widget 当前姿态并反解 initialWorld -> currentWorld；
    // 失败时保留默认 request，避免把不完整 widget 状态发给后端。
    if (!m_boxWidget.GetCurrentWorldBox(
        baseCenter,
        baseSize,
        baseToNowData)) {
        return boxRequest;
    }

    // boxToInitialWorldMatrix 把固定标准盒 [-1,1]^3 放回 PlaceWidget 时的 world AABB；
    // 标准盒半径为 1，所以轴向缩放必须使用 baseSize * 0.5。
    auto boxToInitialWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInitialWorldMatrix->Identity();
    for (int axis = 0; axis < 3; ++axis) {
        boxToInitialWorldMatrix->SetElement(axis, axis, baseSize[axis] * 0.5);
        boxToInitialWorldMatrix->SetElement(axis, 3, baseCenter[axis]);
    }

    // baseToNow 表达用户交互造成的姿态变化；
    // 它接在 boxToInitialWorldMatrix 后面，负责把初始 world AABB 变成当前 world 有向盒。
    auto baseToNow = vtkSmartPointer<vtkMatrix4x4>::New();
    baseToNow->DeepCopy(baseToNowData.data());

    // boxToWorldMatrix 是当前裁切盒在 world 中的完整几何；
    // 组合顺序从右到左读：标准盒先进入初始 world，再应用交互后的 current world 姿态。
    auto boxToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(baseToNow, boxToInitialWorldMatrix, boxToWorldMatrix);

    // worldToInputMat 把显示层 world 几何折回当前数据输入坐标；
    // image 使用 physical-point 语义，polydata 使用主网格 input model 语义。
    auto worldToInputMat = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToInputMat->DeepCopy(GetWorldToInput().data());

    // boxToInputModelMatrix 是 request 下发给算法层的唯一几何真源；
    // 算法只消费标准盒到 active input model 的矩阵，不再依赖 widget 或 world 状态。
    auto boxToInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(worldToInputMat, boxToWorldMatrix, boxToInputModelMatrix);
    vtkMatrix4x4::DeepCopy(boxToInputModelMatrixData.data(), boxToInputModelMatrix);

    boxRequest.boxToInputModelMatrix = boxToInputModelMatrixData;
    return boxRequest;
}

OrthogonalCropRequest CropBridge::Impl::BuildPlaneRequest(
    OrthogonalCropOperation operation,
    OrthogonalCropDataSource dataSource) const
{
    OrthogonalCropRequest planeRequest;
    planeRequest.dataSource = dataSource;
    planeRequest.operation = operation;
    planeRequest.geometryType = CropShape::Plane;
    planeRequest.removalMode = m_currentRemovalMode;

    CropVectorDouble3Array worldOrigin = m_currentWorldPlaneOrigin;
    CropVectorDouble3Array worldNormal = m_currentWorldPlaneNormal;
    if (m_planeWidget.GetCurrentWorldPlane(worldOrigin, worldNormal)) {
        // 以 widget controller 的即时状态为准；缓存值只作为 widget 不可读时的兜底。
    }

    auto worldToInputMat = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToInputMat->DeepCopy(GetWorldToInput().data());

    const double worldOriginPoint[4] = { worldOrigin[0], worldOrigin[1], worldOrigin[2], 1.0 };
    double inputModelOriginPoint[4] = { 0.0, 0.0, 0.0, 1.0 };
    // 平面中心是点，直接使用 world -> active input model 仿射变换。
    worldToInputMat->MultiplyPoint(worldOriginPoint, inputModelOriginPoint);
    const double invW = std::abs(inputModelOriginPoint[3]) > kPlaneVectorEpsilon
        ? 1.0 / inputModelOriginPoint[3]
        : 1.0;

    CropVectorDouble3Array planeCenterInInputModel = {
        inputModelOriginPoint[0] * invW,
        inputModelOriginPoint[1] * invW,
        inputModelOriginPoint[2] * invW
    };

    auto inputToWorldMat = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(worldToInputMat, inputToWorldMat);
    // 法线不能按点或方向向量直接变换；world 法线折回 input model 需使用 inputToWorld 的转置。
    inputToWorldMat->Transpose();

    const double worldNormalVector[4] = { worldNormal[0], worldNormal[1], worldNormal[2], 0.0 };
    double inputModelNormalVector[4] = { 0.0, 0.0, 1.0, 0.0 };
    inputToWorldMat->MultiplyPoint(worldNormalVector, inputModelNormalVector);

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
        worldOrigin[0] + worldPlaneAxisWidth[0] * m_worldHalf[0],
        worldOrigin[1] + worldPlaneAxisWidth[1] * m_worldHalf[0],
        worldOrigin[2] + worldPlaneAxisWidth[2] * m_worldHalf[0],
        1.0
    };
    double worldAxisHeightPoint[4] = {
        worldOrigin[0] + worldPlaneAxisHeight[0] * m_worldHalf[1],
        worldOrigin[1] + worldPlaneAxisHeight[1] * m_worldHalf[1],
        worldOrigin[2] + worldPlaneAxisHeight[2] * m_worldHalf[1],
        1.0
    };
    double inputModelAxisWidthPoint[4] = { 0.0, 0.0, 0.0, 1.0 };
    double inputModelAxisHeightPoint[4] = { 0.0, 0.0, 0.0, 1.0 };
    worldToInputMat->MultiplyPoint(worldAxisWidthPoint, inputModelAxisWidthPoint);
    worldToInputMat->MultiplyPoint(worldAxisHeightPoint, inputModelAxisHeightPoint);

    const double widthInvW = std::abs(inputModelAxisWidthPoint[3]) > kPlaneVectorEpsilon
        ? 1.0 / inputModelAxisWidthPoint[3]
        : 1.0;
    const double heightInvW = std::abs(inputModelAxisHeightPoint[3]) > kPlaneVectorEpsilon
        ? 1.0 / inputModelAxisHeightPoint[3]
        : 1.0;
    const double inputWidthVec[3] = {
        inputModelAxisWidthPoint[0] * widthInvW - planeCenterInInputModel[0],
        inputModelAxisWidthPoint[1] * widthInvW - planeCenterInInputModel[1],
        inputModelAxisWidthPoint[2] * widthInvW - planeCenterInInputModel[2]
    };
    const double inputHeightVec[3] = {
        inputModelAxisHeightPoint[0] * heightInvW - planeCenterInInputModel[0],
        inputModelAxisHeightPoint[1] * heightInvW - planeCenterInInputModel[1],
        inputModelAxisHeightPoint[2] * heightInvW - planeCenterInInputModel[2]
    };
    const std::array<double, 2> planeHalf = {
        std::max(vtkMath::Norm(inputWidthVec), kPlaneVectorEpsilon),
        std::max(vtkMath::Norm(inputHeightVec), kPlaneVectorEpsilon)
    };

    planeRequest.planeCenterInInputModel = planeCenterInInputModel;
    planeRequest.planeNormalInInputModel = planeNormalInInputModel;
    // halfExtents 继续随 request 下发，是为了保留 widget 尺度这一层级信息；
    // 裁切数学只使用 center + normal 的无限半空间，避免再次出现误导性的平面方框。
    planeRequest.planeHalf = planeHalf;
    return planeRequest;
}

bool CropBridge::Impl::SwitchCropBox()
{
    if (!GetInputReady()) {
        std::cerr << "[OrthogonalCrop] Box crop trigger failed: no active image/polydata input is available yet." << std::endl;
        return false;
    }

    if (m_sessionPhase != CropSessionPhase::Idle) {
        const auto previousGeometryType = m_currentGeometryType;
        if (!ExitCrop()) {
            return false;
        }
        if (previousGeometryType == CropShape::Box) {
            return true;
        }
    }

    if (!m_primaryInteractor) {
        std::cerr << "[OrthogonalCrop] Box crop widget init failed: primary interactor missing." << std::endl;
        return false;
    }

    if (!m_hasWorldState) {
        m_currentWorldBounds = GetStartBounds();
        m_hasWorldState = true;
    }

    // 进入交互模式时，widget 使用 world bounds；真正执行时再通过 worldToInputModel 折回 active input model。
    m_boxWidget.SetInteractor(m_primaryInteractor);
    m_boxWidget.SetReferenceWorldBounds(GetActiveWorldBounds()); // 初始化widget交互范围
    m_boxWidget.SetWidgetWorldBounds(m_currentWorldBounds); // 设置实际交互盒子范围有多大
    if (!m_boxWidget.SetEnabled(true)) {
        std::cerr << "[OrthogonalCrop] Box crop widget init failed: vtkBoxWidget2 could not be enabled." << std::endl;
        return false;
    }

    m_planeWidget.SetEnabled(false);
    // 只有 widget 已成功启用后才发布交互态，避免初始化失败留下“已激活”假状态。
    m_currentGeometryType = CropShape::Box;
    m_sessionPhase = CropSessionPhase::Editing;
    m_lastInteractionPhase = CropInteractionPhase::Released;
    ResetPreview();
    std::cout << "[OrthogonalCrop] Box crop widget active. UI uses vtkBoxWidget2, dataSource = "
        << GetDataSourceText(m_backend.GetActiveDataSource())
        << ". Use host crop commands to preview, submit, or exit." << std::endl;
    return true;
}

bool CropBridge::Impl::SwitchCropPlane()
{
    if (!GetInputReady()) {
        std::cerr << "[OrthogonalCrop] Planar crop trigger failed: no active image/polydata input is available yet." << std::endl;
        return false;
    }

    if (m_sessionPhase != CropSessionPhase::Idle) {
        const auto previousGeometryType = m_currentGeometryType;
        if (!ExitCrop()) {
            return false;
        }
        if (previousGeometryType == CropShape::Plane) {
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
    m_worldHalf = {
        std::max(activeWorldDimensions[0] * kPlaneScale, kPlaneVectorEpsilon),
        std::max(activeWorldDimensions[1] * kPlaneScale, kPlaneVectorEpsilon)
    };
    // reference bounds 提供初始中心和启用合法性门槛；widget 可视范围由 origin 与 halfExtents 构造。
    // halfExtents 只保留交互平面的尺度快照，不驱动算法层生成有限矩形 outline。
    m_currentWorldBounds = activeWorldBounds;
    m_hasWorldState = true;

    m_boxWidget.SetEnabled(false);
    m_planeWidget.SetInteractor(m_primaryInteractor);
    m_planeWidget.SetReferenceWorldBounds(activeWorldBounds);
    m_planeWidget.SetWidgetWorldPlane(
        m_currentWorldPlaneOrigin,
        m_currentWorldPlaneNormal,
        m_worldHalf);
    if (!m_planeWidget.SetEnabled(true)) {
        std::cerr << "[OrthogonalCrop] Planar crop widget init failed: vtkImplicitPlaneWidget2 could not be enabled." << std::endl;
        return false;
    }

    // 只有 widget 已成功启用后才发布交互态，避免初始化失败留下“已激活”假状态。
    m_currentGeometryType = CropShape::Plane;
    m_sessionPhase = CropSessionPhase::Editing;
    m_lastInteractionPhase = CropInteractionPhase::Released;
    ResetPreview();
    std::cout << "[OrthogonalCrop] Planar crop widget active. UI uses vtkImplicitPlaneWidget2, dataSource = "
        << GetDataSourceText(m_backend.GetActiveDataSource())
        << ". Use host crop commands to preview, submit, or exit." << std::endl;
    return true;
}

bool CropBridge::Impl::ExitCrop()
{
    if (m_sessionPhase == CropSessionPhase::Idle) {
        return false;
    }

    // reload pending 时 widget 已被禁用，但 submit 元数据仍等待完成回调，不能提前清场。
    if (m_sessionPhase == CropSessionPhase::Reloading) {
        std::cout << "[OrthogonalCrop] Crop widget deactivation deferred: image submit reload is pending." << std::endl;
        return false;
    }

    // 正常退出同时清空交互与预览意图；world 几何缓存保留给后续再次激活复用。
    m_boxWidget.SetEnabled(false);
    m_planeWidget.SetEnabled(false);
    m_sessionPhase = CropSessionPhase::Idle;
    m_isPreviewOn = false;
    m_lastInteractionPhase = CropInteractionPhase::Released;
    ResetPreview();
    std::cout << "[OrthogonalCrop] Crop widget deactivated. 3D navigation restored." << std::endl;
    return true;
}

bool CropBridge::Impl::GetCropActive() const
{
    return m_sessionPhase != CropSessionPhase::Idle;
}

bool CropBridge::Impl::SendPreview()
{
    if (!m_hasWorldState) {
        return false;
    }

    // 1. 从当前 widget world 几何构造 request；Box 与 Plane 只在几何真源构造上不同。
    const auto volumeRequest = m_currentGeometryType == CropShape::Plane
        ? BuildPlaneRequest(
            OrthogonalCropOperation::Preview,
            OrthogonalCropDataSource::VolumeData)
        : BuildBoxRequest(
            OrthogonalCropOperation::Preview,
            OrthogonalCropDataSource::VolumeData);
    auto polyDataRequest = volumeRequest;
    polyDataRequest.dataSource = OrthogonalCropDataSource::PolyData;
    ClearPreviewInput();

    // 2. 先确认是否存在 3D 主 target；VolumeData 结果只计算一次并在这些 target 间复用。
    bool hasMainTarget = false;
    bool isPreviewSent = false;
    for (const auto& target : m_previewRenderTargets) {
        if (!target.first) {
            continue;
        }

        if (target.first->GetNavigationAxis() < 0) {
            hasMainTarget = true;
        }
    }

    OrthogonalCropResult volumeResult;
    bool hasVolumeResult = false;
    if (m_backend.GetImageReady() && hasMainTarget) {
        volumeResult = m_backend.GetResult(volumeRequest);
        if (volumeResult.failureReason == CropFailure::None
            && volumeResult.isSucceeded) {
            hasVolumeResult = true;
        }
    }

    // 3. 每个 target 单独读取当前 actor mesh，路由 PolyData 结果，再由 plug 应用主显示和 overlay。
    for (const auto& target : m_previewRenderTargets) {
        if (!target.first || !target.second) {
            continue;
        }

        OrthogonalCropResult polyResult;
        bool hasPolyResult = false;
        m_previewPlug.ResetPreview(target.first, target.second);

        const bool isMain3DTarget = target.first->GetNavigationAxis() < 0;
        const OrthogonalCropResult* volumePreviewResult = isMain3DTarget && hasVolumeResult
            ? &volumeResult
            : nullptr;

        if (auto polyData = m_previewPlug.GetPreviewData(target.first)) {
            m_backend.SetInputPolyData(polyData);

            polyResult = m_backend.GetResult(polyDataRequest);
            ClearPreviewInput();
            // PolyData preview 是 render-only 主显示路径；
            // 有效性由 result 成功与否决定，不再强制要求 clipPolyData artifact。
            if (polyResult.failureReason == CropFailure::None
                && polyResult.isSucceeded) {
                hasPolyResult = true;
            }
        }

        if (!volumePreviewResult && !hasPolyResult) {
            continue;
        }

        isPreviewSent = m_previewPlug.SetPreview(
            target.first,
            target.second,
            m_referenceRenderService,
            volumePreviewResult,
            hasPolyResult ? &polyResult : nullptr,
            m_currentRemovalMode)
            || isPreviewSent;
        target.first->SetDirty();
    }

    // 4. 无论有多少 target，结束时都把 router 的临时 mesh 输入恢复为 host 绑定输入。
    ClearPreviewInput();
    return isPreviewSent;
}

void CropBridge::Impl::SwitchPreview(CropRemovalMode removalMode)
{
    if (m_sessionPhase == CropSessionPhase::Idle) {
        return;
    }

    // Box 与 Plane 使用相同的 preview 切换状态机：
    // A. 同模式再次触发时关闭 preview 并恢复主模型。
    // B. 切到新模式时更新 removal mode，非拖拽阶段立即刷新。
    if (m_isPreviewOn && m_currentRemovalMode == removalMode) {
        m_isPreviewOn = false;
        ResetPreview();
        return;
    }

    m_isPreviewOn = true;
    m_currentRemovalMode = removalMode;
    if (m_lastInteractionPhase != CropInteractionPhase::Dragging) {
        // 拖拽中等待 EndInteractionEvent 统一刷新，避免重复执行裁切预览。
        (void)SendPreview();
    }
}

void CropBridge::Impl::ClearPreviewViews()
{
    // 这里只清理 overlay 生命周期；
    // 主模型 preview 状态由 ResetPreview 收口，避免解绑窗口时意外改动主显示。
    for (const auto& target : m_previewRenderTargets) {
        if (target.first && target.second) {
            target.first->RemoveOverlayStrategy(target.second);
        }
    }
    m_previewRenderTargets.clear();
    ClearPreviewInput();
}

void CropBridge::Impl::ResetPreview()
{
    // 恢复 preview 涉及 overlay、volume shader 和 polydata clip 三条显示路径；
    // 统一在这里清空临时状态并标脏，确保关闭 preview 后各窗口回到全模型显示。
    for (auto& target : m_previewRenderTargets) {
        m_previewPlug.ResetPreview(target.first, target.second);
        if (target.first) {
            target.first->SetDirty();
        }
    }
    ClearPreviewInput();
}

bool CropBridge::Impl::BuildCameraState()
{
    ClearCamera();
    if (!m_referenceRenderer || !m_referenceRenderer->GetActiveCamera()) {
        return false;
    }

    auto* camera = m_referenceRenderer->GetActiveCamera();
    camera->GetPosition(m_submitCameraState.position.data());
    camera->GetFocalPoint(m_submitCameraState.focalPoint.data());
    camera->GetViewUp(m_submitCameraState.viewUp.data());
    camera->GetClippingRange(m_submitCameraState.clippingRange.data());
    m_submitCameraState.parallelScale = camera->GetParallelScale();
    m_submitCameraState.viewAngle = camera->GetViewAngle();
    m_submitCameraState.isParallelProjection = camera->GetParallelProjection() != 0;
    m_submitCameraState.isValid = true;
    return true;
}

bool CropBridge::Impl::ResetCamera()
{
    if (!m_submitCameraState.isValid
        || !m_referenceRenderer
        || !m_referenceRenderer->GetActiveCamera()) {
        return false;
    }

    auto* camera = m_referenceRenderer->GetActiveCamera();
    camera->SetPosition(m_submitCameraState.position.data());
    camera->SetFocalPoint(m_submitCameraState.focalPoint.data());
    camera->SetViewUp(m_submitCameraState.viewUp.data());
    camera->SetClippingRange(m_submitCameraState.clippingRange.data());
    camera->SetParallelScale(m_submitCameraState.parallelScale);
    camera->SetViewAngle(m_submitCameraState.viewAngle);
    camera->SetParallelProjection(m_submitCameraState.isParallelProjection ? 1 : 0);
    ClearCamera();
    return true;
}

bool CropBridge::Impl::ClearCamera()
{
    const bool hasCamera = m_submitCameraState.isValid;
    m_submitCameraState = {};
    return hasCamera;
}

bool CropBridge::Impl::SendSubmit(std::function<void(bool)> onComplete)
{
    if (!m_submitReloadHandler) {
        std::cerr << "[OrthogonalCrop] Submit failed: submit reload handler is not ready." << std::endl;
        return false;
    }

    // 1. submit 只消费已激活、非拖拽且具有 image 输入的当前状态；pending reload 会拒绝重入。
    if (!GetSubmitReady()) {
        return false;
    }

    BuildCameraState();
    ClearPreviewInput();

    // 2. request 直接从当前 widget 几何和 backend image 输入构造，不复用任何 preview result。
    const auto submitRequest = m_currentGeometryType == CropShape::Plane
        ? BuildPlaneRequest(OrthogonalCropOperation::Submit, OrthogonalCropDataSource::ImageData)
        : BuildBoxRequest(OrthogonalCropOperation::Submit, OrthogonalCropDataSource::ImageData);
    auto submitResult = m_backend.GetResult(submitRequest);
    if (submitResult.failureReason != CropFailure::None || !submitResult.isSucceeded) {
        ClearCamera();
        std::cerr << "[OrthogonalCrop] Submit failed: "
            << GetFailureReasonText(submitResult.failureReason);
        if (!submitResult.message.empty()) {
            std::cerr << " - " << submitResult.message;
        }
        std::cerr << std::endl;
        return false;
    }

    auto submitImage = submitResult.submitImage;
    if (!submitImage) {
        std::cerr << "[OrthogonalCrop] Image submit failed: output image is null." << std::endl;
        ClearCamera();
        m_submitOverlay = OrthogonalCropResult();
        return false;
    }

    // 3. submit image 移交 reload handler；bridge 只保留 overlay 元数据，并在外部调用前关闭 widget、置 pending。
    submitResult.submitImage = vtkSmartPointer<vtkImageData>();
    m_submitOverlay = submitResult;
    m_submitCallback = std::move(onComplete);
    m_sessionPhase = CropSessionPhase::Reloading;
    m_boxWidget.SetEnabled(false);
    m_planeWidget.SetEnabled(false);

    // A. handler 拒绝表示外部事务从未建立；本地立即清 pending、清元数据并恢复当前 widget。
    std::size_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_reloadGate->mutex);
        generation = ++m_reloadGate->generation;
    }
    const std::weak_ptr<ReloadGate> weakGate = m_reloadGate;
    bool isAccepted = false;
    try {
        isAccepted = m_submitReloadHandler(
            std::move(submitImage),
            [weakGate, generation](bool isSuccess) {
                const auto gate = weakGate.lock();
                if (!gate) return;
                Impl* owner = nullptr;
                {
                    std::lock_guard<std::mutex> lock(gate->mutex);
                    if (!gate->owner || gate->generation != generation) return;
                    owner = gate->owner;
                    ++gate->generation;
                }
                owner->OnSubmitReload(isSuccess);
            });
    }
    catch (const std::exception& error) {
        std::cerr << "[OrthogonalCrop] Reload handler failed: " << error.what() << std::endl;
    }
    catch (...) {
        std::cerr << "[OrthogonalCrop] Reload handler failed with an unknown exception." << std::endl;
    }
    if (!isAccepted) {
        std::cerr << "[OrthogonalCrop] Submit failed: reload request was rejected." << std::endl;
        ClearPreviewInput();
        m_submitOverlay = OrthogonalCropResult();
        m_submitCallback = nullptr;
        m_sessionPhase = CropSessionPhase::Editing;
        ClearCamera();
        if (m_sessionPhase == CropSessionPhase::Editing) {
            if (m_currentGeometryType == CropShape::Plane) {
                m_planeWidget.SetEnabled(true);
            }
            else {
                m_boxWidget.SetEnabled(true);
            }
        }
        return false;
    }

    // B. true 只表示 reload handler 接受了请求；成功或失败恢复均由 OnSubmitReload 完成。
    return true;
}

bool CropBridge::Impl::GetSubmitReady() const
{
    if (m_sessionPhase != CropSessionPhase::Editing || !m_hasWorldState) {
        std::cerr << "[OrthogonalCrop] Submit failed: crop widget is not active." << std::endl;
        return false;
    }

    if (m_lastInteractionPhase == CropInteractionPhase::Dragging) {
        std::cerr << "[OrthogonalCrop] Submit failed: wait until widget dragging finishes." << std::endl;
        return false;
    }

    if (!m_backend.GetImageReady()) {
        std::cerr << "[OrthogonalCrop] Submit failed: image crop input is missing." << std::endl;
        return false;
    }

    return true;
}

void CropBridge::Impl::OnSubmitReload(bool isSuccess)
{
    auto callback = std::move(m_submitCallback);
    if (!isSuccess) {
        // A. reload 失败不提交几何状态：清除 pending 事务并恢复当前模式 widget，允许用户调整后重试。
        std::cerr << "[OrthogonalCrop] Submit reload failed." << std::endl;
        ClearPreviewInput();
        m_submitOverlay = OrthogonalCropResult();
        m_sessionPhase = CropSessionPhase::Editing;
        ClearCamera();
        if (m_sessionPhase == CropSessionPhase::Editing) {
            if (m_currentGeometryType == CropShape::Plane) {
                m_planeWidget.SetEnabled(true);
            }
            else {
                m_boxWidget.SetEnabled(true);
            }
        }
        if (callback) {
            try { callback(false); } catch (...) {}
        }
        return;
    }

    // B. reload 成功后新 image 已成为 host 真源；旧 world 几何失效，退出交互并要求下一轮重建 bounds。
    ClearPreviewInput();

    const auto submitOverlayResult = m_submitOverlay;
    m_submitOverlay = OrthogonalCropResult();

    std::cout << "[OrthogonalCrop] Submit applied to host image data." << std::endl;
    ResetCamera();
    m_sessionPhase = CropSessionPhase::Editing;
    m_hasWorldState = false;
    ExitCrop();

    if (!submitOverlayResult.isSucceeded) {
        return;
    }

    // submit result 已移除 image payload，只把与新主数据对应的裁切元数据分发给各 overlay target。
    for (const auto& target : m_previewRenderTargets) {
        if (!target.first || !target.second) {
            continue;
        }

        target.second->SetSliceAxis(target.first->GetNavigationAxis());
        target.second->SetCropResult(submitOverlayResult);
        target.first->SetDirty();
    }
    if (callback) {
        try { callback(true); } catch (...) {}
    }
}

const char* CropBridge::Impl::GetFailureReasonText(CropFailure failureReason)
{
    switch (failureReason) {
    case CropFailure::None:
        return "None";
    case CropFailure::NoImage:
        return "NoImage";
    case CropFailure::NoPolyData:
        return "NoPolyData";
    case CropFailure::BadBounds:
        return "BadBounds";
    case CropFailure::OutOfBounds:
        return "OutOfBounds";
    case CropFailure::NoBackend:
        return "NoBackend";
    case CropFailure::BadSubmitMode:
        return "BadSubmitMode";
    case CropFailure::LowRam:
        return "LowRam";
    case CropFailure::MaskFailed:
        return "MaskFailed";
    case CropFailure::ImageFailed:
        return "ImageFailed";
    case CropFailure::ClipFailed:
        return "ClipFailed";
    }

    return "Unknown";
}

const char* CropBridge::Impl::GetDataSourceText(OrthogonalCropDataSource dataSource)
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

CropBridge::CropBridge()
    : m_impl(std::make_unique<CropBridge::Impl>())
{
}

CropBridge::~CropBridge() = default;

CropBridge::CropBridge(CropBridge&&) noexcept = default;

CropBridge& CropBridge::operator=(CropBridge&&) noexcept = default;

void CropBridge::SetInputSnapshot(
    vtkSmartPointer<vtkImageData> image)
{
    m_impl->SetInputImage(std::move(image));
}

void CropBridge::SetSubmitReloadHandler(ReloadSubmitter reloadSubmitter)
{
    m_impl->SetSubmitReloadHandler(std::move(reloadSubmitter));
}

bool CropBridge::SendSubmit(std::function<void(bool)> onComplete)
{
    return m_impl->SendSubmit(std::move(onComplete));
}

bool CropBridge::StartView(CropViewRequest request)
{
    return m_impl->StartView(std::move(request));
}

void CropBridge::ClearBindings()
{
    m_impl->ClearBindings();
}

bool CropBridge::SwitchCropBox()
{
    return m_impl->SwitchCropBox();
}

bool CropBridge::SwitchCropPlane()
{
    return m_impl->SwitchCropPlane();
}

bool CropBridge::ExitCrop()
{
    return m_impl->ExitCrop();
}

bool CropBridge::GetCropActive() const
{
    return m_impl->GetCropActive();
}

void CropBridge::SwitchPreview(CropRemovalMode removalMode)
{
    m_impl->SwitchPreview(removalMode);
}
