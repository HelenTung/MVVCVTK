#pragma once

// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/include/Service/OrthogonalCropInteractionBridgeService.h
// 分类: Service / Interaction Bridge
// OrthogonalCropInteractionBridgeService - 正交裁切交互桥接服务
// 说明: 连接 widget、数据后端与 preview 窗口，管理交互态与预览刷新顺序。
// =====================================================================
// 交互主链路：
// 1. ToggleInteractiveCrop 进入交互态，内部生成默认 widget bounds 并挂接 vtkBoxWidget2
// 2. HandleWidgetWorldBoundsChanged 持续记录 widget world bounds 与交互 phase
// 3. Released 或显式 toggle preview 时，BuildPreviewRequest 把 widget world 有向盒折回 input model request
// 4. BuildResultContext 在 bridge 侧确定本次结果的数据源、后端和交互态
// 5. UpdatePreviewFromCurrentBounds 调用 backend 填充统一结果
// 6. DispatchPreviewResult 把结果分发给 2D/3D overlay，必要时再给 3D 主模型做主显示预览

#include "OrthogonalCropWidgetStateController.h"
#include "OrthogonalCropCameraStateController.h"
#include "OrthogonalCropBackendRouterService.h"
#include "OrthogonalCropPreviewOverlayStrategy.h"
#include "AppInterfaces.h"

#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <array>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class vtkGeometryFilter;
class vtkTableBasedClipDataSet;
class vtkBox;
class vtkVolume;
class vtkVolumeMapper;
class vtkGPUVolumeRayCastMapper;
class vtkRenderer;
struct OrthogonalCropSubmitReloadPayload {
    std::shared_ptr<std::vector<float>> buffer;
    std::array<int, 3> dims = { 0, 0, 0 };
    std::array<float, 3> spacing = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> origin = { 0.0f, 0.0f, 0.0f };
};

class OrthogonalCropInteractionBridgeService {
public:

    // Public boundary: initialization/setup and user hotkey actions.
    // Internal state transitions, backend routing details, and VTK preview plumbing stay private.

    // 构造时绑定 widget bounds 回调，把 VTK 交互事件转入本类状态机。
    OrthogonalCropInteractionBridgeService();

    // 以下一组接口把 image 输入转发给 backend router。
    void SetInputImage(vtkSmartPointer<vtkImageData> image);
    vtkSmartPointer<vtkImageData> GetInputImage() const;

    // 以下一组接口把 polydata 输入转发给 backend router。
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);

    // 设置 backend router 的首选数据源。
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);

    // DataManager 只作为 Auto 模式下 image 输入的兜底来源。
    void SetDataManager(std::shared_ptr<AbstractDataManager> dataMgr);

    // 主 interactor 由 3D 参考窗口提供，widget 只会挂到这个 interactor 上。
    void SetPrimaryInteractor(vtkRenderWindowInteractor* interactor);

    // 参考渲染器只供算法内部保存/恢复相机状态，不写入 SharedState。
    void SetReferenceRenderer(vtkRenderer* renderer);

    // 参考渲染服务负责 world / active input model 坐标互转。
    void SetReferenceRenderService(std::shared_ptr<AbstractInteractiveService> referenceService);

    // preview 服务列表决定哪些窗口会收到 overlay 与设脏刷新。
    void SetPreviewRenderServices(std::vector<std::shared_ptr<AbstractInteractiveService>> previewRenderServices);

    // 构建当前 KeepInside image submit 的 reload payload；真正提交由 workflow 协调。
    bool BuildSubmitReloadPayload(OrthogonalCropSubmitReloadPayload& payload);

    // workflow 提交 reload 成功发起后，bridge 锁住 widget 并保留 preview 状态。
    void SetSubmitReloadStarted();

    // workflow 收到 reload 失败或请求被拒绝时，bridge 恢复可交互状态。
    void SetSubmitReloadFailed();

    // workflow 收到 reload 完成回调后，bridge 统一恢复相机并关闭裁切状态。
    void SetSubmitReloadSynced();

    // 对应的裁切模式 toggle 入口。
    bool ToggleInteractiveCrop();

    // 对应的显式退出入口。
    bool ExitInteractiveCrop();

    // 对应的“保留盒内”预览动作。
    void ToggleInsidePreview();

    // 对应的“移除盒内”预览动作。
    void ToggleOutsidePreview();

private:
    // Private boundary: widget state machine, world/active input model conversion, backend query,
    // preview distribution, and VTK mapper/shader implementation details.

    // 一个 preview 目标窗口对应一份 overlay 策略与可能的主模型临时裁切缓存。
    struct PreviewRenderTarget {
        // 实际要刷新的窗口服务。
        std::shared_ptr<AbstractInteractiveService> service;

        // 负责显示 mask / outline / clipped polydata 的 overlay 策略。
        std::shared_ptr<OrthogonalCropPreviewOverlayStrategy> overlayStrategy;

        // 3D 主模型预览时缓存的 mapper 指针。
        vtkPolyDataMapper* mainPreviewMapper = nullptr;

        // 主模型原始 polydata 的浅拷贝，作为 preview 持久管道的稳定输入。
        vtkSmartPointer<vtkPolyData> mainPreviewSourcePolyData;

        // 3D 主窗口预览时复用的 clip 管道，避免每次刷新都重建 filter。
        vtkSmartPointer<vtkTableBasedClipDataSet> mainPreviewClipFilter;

        // 把 clip dataset 输出稳定收敛为 polydata 的持久 geometry filter。
        vtkSmartPointer<vtkGeometryFilter> mainPreviewGeometryFilter;

        // preview 关闭时使用的全量直通盒，避免再切回原始 mapper 输入。
        vtkSmartPointer<vtkBox> mainPreviewPassThroughBox;
    };

    // 确保当前至少有一个可用后端；Auto 模式下会尝试从 data manager 抓 image。
    bool EnsureInputReady();

    // 生成默认交互裁切盒，作为第一次进入模式时的初始 world bounds。
    std::array<double, 6> GetDefaultInteractiveWorldBounds() const;

    // 响应 widget 交互回调，记录 world bounds/phase，并在 Released 时触发 preview。
    void HandleWidgetWorldBoundsChanged(const std::array<double, 6>& worldBounds, CropInteractionPhase phase);

    // 把 active input model bounds 提升为 widget 所需的 world bounds。
    // image model 底层由 VTK physical-point API 表达。
    std::array<double, 6> GetActiveInputModelBoundsAsWorldBounds(const std::array<double, 6>& activeInputModelBounds) const;

    // 返回活跃数据在 world 下的 bounds，供 widget 摆放与默认盒生成。
    std::array<double, 6> GetActiveWorldBounds() const;

    // 返回 world -> active input model 矩阵。
    // 它把 widget 在显示层 world 中形成的 boxToWorld 继续折回 boxToInputModel，
    // 让后端在输入数据坐标里执行裁切，而不是依赖窗口或 widget 状态。
    std::array<double, 16> GetWorldToActiveInputModelMatrix() const;

    // 基于当前 widget 有向盒组装一次 preview request。
    // 这里把标准盒 [-1,1]^3 依次映射到初始 world、当前 world、active input model，
    // 最终只把 boxToInputModelMatrix 下发给后端，避免后端反向读取 UI 状态。
    const OrthogonalCropRequest BuildPreviewRequest() const;

    // 基于 request 构造 result 上下文；bridge 在这里固定数据源、后端和交互态。
    OrthogonalCropResult BuildResultContext(const OrthogonalCropRequest& request) const;

    // 统一执行一次 preview 刷新：构建 request、拿结果、投递 overlay、刷新窗口。
    void UpdatePreviewFromCurrentBounds(bool logStats);

    // 切换 preview 开关与 removal mode。
    void TogglePreview(CropRemovalMode removalMode, bool logStats);

    // 校验当前交互状态是否允许发起 image submit。
    bool CanApplySubmit() const;

    // 基于当前 widget 有向盒构建 image submit request。
    OrthogonalCropRequest BuildSubmitRequest() const;

    // 把 image submit image 适配成 workflow 可提交的 reload payload。
    bool BuildSubmitPayload(const OrthogonalCropResult& submitResult, OrthogonalCropSubmitReloadPayload& payload) const;

    // 激活交互裁切模式，初始化 widget 与默认 bounds。
    bool ActivateInteractiveCrop();

    // 关闭裁切模式并恢复 preview / 主模型状态。
    bool DeactivateInteractiveCrop();

    // 以下查询接口仅作为 bridge 内部 backend 边界，外部不直接调用 backend 细节。
    OrthogonalCropDataSource GetActiveDataSource() const;
    std::array<double, 6> GetActiveInputModelBounds() const;
    OrthogonalCropRequest GetDefaultRequest() const;
    OrthogonalCropResult GetResult(
        const OrthogonalCropRequest& request,
        const OrthogonalCropResult& resultContext) const;

    // 以下文本 helper 统一服务于日志输出。
    static const char* GetFailureReasonText(OrthogonalCropFailureReason failureReason);
    static const char* GetRemovalModeText(CropRemovalMode removalMode);
    static const char* GetDataSourceText(OrthogonalCropDataSource dataSource);
    static const char* GetBackendText(OrthogonalCropBackend backend);

    // preview 列表为空时，取第一个有效目标作为 reference service 的后备来源。
    std::shared_ptr<AbstractInteractiveService> GetFirstPreviewRenderService() const;

    // 替换 preview 目标列表前，先把旧 overlay 从旧窗口上摘掉。
    void ClearPreviewRenderTargets();

    // 退出 preview 或退出交互模式时恢复 overlay，并把主模型管道切回全量直通。
    void RestorePreviewRenderTargets();

    // 向 preview 目标列表新增一个窗口服务，并为其挂载 overlay。
    void AddPreviewRenderService(const std::shared_ptr<AbstractInteractiveService>& service);

    // 把一次 previewResult 按本次 request 的 removal mode 分发给所有 preview 窗口。
    bool DispatchPreviewResult(const OrthogonalCropResult& previewResult, CropRemovalMode removalMode);

    // 在 3D volume 主窗口上执行一次 volume preview；仅接管 VTK mapper 能正确表达的模式。
    bool ApplyVolumePreview(
        PreviewRenderTarget& target,
        const OrthogonalCropResult& previewResult,
        CropRemovalMode removalMode);

    // KeepInside 从统一 cropData 还原世界平面，再交给 VTK volume clipping 表达“只显示盒内”。
    void ApplyVolumeKeepInsidePreview(
        vtkVolumeMapper* volumeMapper,
        const OrthogonalCropResult& previewResult) const;

    // RemoveInside 用 GPU volume shader discard 表达旋转盒外补集。
    bool ApplyVolumeRemoveInsidePreview(
        vtkVolume* volume,
        vtkGPUVolumeRayCastMapper* volumeMapper,
        const OrthogonalCropResult& previewResult) const;

    // 把某个 3D 主窗口的持久 preview 管道恢复成全量直通状态。
    void RestorePolyDataPreview(PreviewRenderTarget& target);

    // 在满足条件的 3D 主窗口上执行一次临时 polydata clip 预览。
    bool ApplyPolyDataPreview(
        PreviewRenderTarget& target,
        const OrthogonalCropResult& previewResult,
        CropRemovalMode removalMode);

    // 后端分发器，负责 image / polydata 两条执行链。
    OrthogonalCropBackendRouterService m_backend;

    // Auto 模式下 image 输入的兜底来源。
    std::shared_ptr<AbstractDataManager> m_dataMgr;

    // widget 唯一挂载的主 interactor。
    vtkRenderWindowInteractor* m_primaryInteractor = nullptr;

    // 当前是否处于裁切交互模式。
    bool m_cropInteractionEnabled = false;

    // 当前 world bounds 是否已经初始化过。
    bool m_worldBoundsInitialized = false;

    // 当前是否有 preview 在显示。
    bool m_previewEnabled = false;

    // 最近一次 widget 交互阶段，用于区分 dragging / released。
    CropInteractionPhase m_lastInteractionPhase = CropInteractionPhase::Idle;

    // 当前 preview 真正使用的 removal mode。
    CropRemovalMode m_currentRemovalMode = CropRemovalMode::KeepInside;

    // 缓存 widget 当前 world AABB，用于初始化、日志和 bounds 变化回调；
    // 旋转后它只是外接范围，真实裁切姿态必须由 GetCurrentWorldBox() 重建。
    std::array<double, 6> m_currentWorldBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

    // image submit 已提交到 reload 通道但尚未完成；期间保留 preview，避免闪回原模型。
    bool m_submitReloadPending = false;

    // 裁切提交前保存相机，下一次主数据重建并完成策略同步后恢复；每次保存都会覆盖上一份。
    OrthogonalCropCameraStateController m_cameraStateController;

    // 只负责 vtkBoxWidget2 生命周期与状态同步，不承担业务计算。
    OrthogonalCropWidgetStateController m_widgetStateController;

    // world / active input model 坐标互转的参考窗口服务。
    std::shared_ptr<AbstractInteractiveService> m_referenceRenderService;

    // 相机快照保存/恢复的参考 renderer；算法内部状态，不进入 service/share。
    vtkRenderer* m_referenceRenderer = nullptr;

    // 真正需要被 overlay / preview 联动刷新的窗口目标列表。
    std::vector<PreviewRenderTarget> m_previewRenderTargets;
};
