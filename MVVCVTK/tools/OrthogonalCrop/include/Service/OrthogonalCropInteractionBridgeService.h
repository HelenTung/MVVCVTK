#pragma once

// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/include/Service/OrthogonalCropInteractionBridgeService.h
// 分类: Service / Interaction Bridge
// OrthogonalCropInteractionBridgeService - 正交裁切交互桥接服务
// 说明: 连接 widget、数据后端与 preview 窗口，管理交互态与预览刷新顺序。
// =====================================================================
// 交互主链路：
// 1. ActivateInteractiveCrop 生成默认 widget bounds 并挂接 vtkBoxWidget2
// 2. HandleWidgetBoundsChanged 持续记录 reference render 交互坐标 bounds 与交互 phase
// 3. Released 或显式 toggle preview 时，BuildPreviewRequest 把 widget 有向盒折回 model request
// 4. UpdatePreviewFromCurrentBounds 调用 backend 获取统一结果
// 5. SetPreviewServicesDirty 把结果分发给 2D/3D overlay，必要时再给 3D 主模型做主显示预览

#include "OrthogonalCropWidgetStateController.h"
#include "OrthogonalCropBackendRouterService.h"
#include "OrthogonalCropPreviewOverlayStrategy.h"
#include "AppService.h"

#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <array>
#include <memory>
#include <utility>
#include <vector>

class vtkGeometryFilter;
class vtkTableBasedClipDataSet;
class vtkBox;
class vtkVolume;
class vtkVolumeMapper;
class vtkGPUVolumeRayCastMapper;
class OrthogonalCropInteractionBridgeService {
public:
    // 构造时绑定 widget bounds 回调，把 VTK 交互事件转入本类状态机。
    OrthogonalCropInteractionBridgeService();

    // 以下一组接口把 image 输入转发给 backend router。
    void SetInputImage(vtkSmartPointer<vtkImageData> image);
    vtkSmartPointer<vtkImageData> GetInputImage() const;

    // 以下一组接口把 polydata 输入转发给 backend router。
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);
    vtkSmartPointer<vtkPolyData> GetInputPolyData() const;

    // 设置 backend router 的首选数据源。
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);

    // 以下查询接口统一透传给 backend router，方便 bridge 作为单一服务暴露给 main。
    OrthogonalCropDataSource GetActiveDataSource() const;
    std::array<double, 6> GetActiveModelBounds() const;
    OrthogonalCropRequest GetDefaultRequest() const;
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const;
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const;

    // DataManager 只作为 Auto 模式下 image 输入的兜底来源。
    void SetDataManager(std::shared_ptr<AbstractDataManager> dataMgr);

    // 主 interactor 由 3D 参考窗口提供，widget 只会挂到这个 interactor 上。
    void SetPrimaryInteractor(vtkRenderWindowInteractor* interactor);

    // 参考渲染服务负责 world/model 坐标互转。
    void SetReferenceRenderService(std::shared_ptr<AbstractInteractiveService> referenceService);

    // preview 服务列表决定哪些窗口会收到 overlay 与设脏刷新。
    void SetPreviewRenderServices(std::vector<std::shared_ptr<AbstractInteractiveService>> previewRenderServices);

    // 控制 preview 是否必须产出完整后端结果（mask / derived polydata）。
    void SetPreviewRequiresFullArtifacts(bool required);

    // 在轻量 preview 与完整 preview 之间切换；必要时会立即刷新当前 preview。
    void TogglePreviewArtifactMode(bool logStats = true);

    // 对应的裁切模式 toggle 入口。
    bool ToggleInteractiveCrop();

    // 对应的显式退出入口。
    bool ExitInteractiveCrop();

    // 对应的“保留盒内”预览动作。
    void ToggleInsidePreview();

    // 对应的“移除盒内”预览动作。
    void ToggleOutsidePreview();

    // 回到默认裁切盒，并按需要刷新 preview。
    bool ResetInteractiveBoundsToDefault(bool updatePreview = true);

    // 激活交互裁切模式，初始化 widget 与默认 bounds。
    bool ActivateInteractiveCrop();

    // 关闭裁切模式并恢复 preview / 主模型状态。
    bool DeactivateInteractiveCrop();

private:
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

    // 生成默认交互裁切盒，作为第一次进入模式时的初始 bounds。
    std::array<double, 6> GetDefaultInteractiveBounds() const;

    // 响应 widget 交互回调，记录 bounds/phase，并在 Released 时触发 preview。
    void HandleWidgetBoundsChanged(const std::array<double, 6>& bounds, CropInteractionPhase phase);

    // 把 model bounds 提升为 widget 所需的 world bounds。
    // image model 底层由 VTK physical-point API 表达。
    std::array<double, 6> GetModelBoundsAsWorldBounds(const std::array<double, 6>& modelBounds) const;

    // 返回活跃数据在 world 下的 bounds，供 widget 摆放与默认盒生成。
    std::array<double, 6> GetActiveWorldBounds() const;

    // 返回 world -> model 矩阵，供 widget boxToWorld 继续组合为 boxToModel。
    // 这是 widget 交互姿态与后端 model 裁切之间最关键的坐标桥。
    std::array<double, 16> GetWorldToModelMatrix() const;

    // 基于当前 widget 有向盒组装一次 preview request。
    // 这里会把标准盒 [-1,1]^3 转换成 boxToModelMatrix 的统一表达。
    OrthogonalCropRequest BuildPreviewRequest() const;

    // 统一执行一次 preview 刷新：构建 request、拿结果、投递 overlay、刷新窗口。
    void UpdatePreviewFromCurrentBounds(bool logStats);

    // 切换 preview 开关与 removal mode。
    void TogglePreview(CropRemovalMode removalMode, bool logStats);

    // 以下文本 helper 统一服务于日志输出。
    static const char* GetFailureReasonText(OrthogonalCropFailureReason failureReason);
    static const char* GetRemovalModeText(CropRemovalMode removalMode);
    static const char* GetDataSourceText(OrthogonalCropDataSource dataSource);
    static const char* GetResolvedBackendText(OrthogonalCropResolvedBackend backend);

    // preview 列表为空时，取第一个有效目标作为 reference service 的后备来源。
    std::shared_ptr<AbstractInteractiveService> GetFirstPreviewRenderService() const;

    // 替换 preview 目标列表前，先把旧 overlay 从旧窗口上摘掉。
    void ClearPreviewRenderTargets();

    // 退出 preview 或退出交互模式时恢复 overlay，并把主模型管道切回全量直通。
    void RestorePreviewRenderTargets();

    // 向 preview 目标列表新增一个窗口服务，并为其挂载 overlay。
    void AddPreviewRenderService(const std::shared_ptr<AbstractInteractiveService>& service);

    // 把一次 previewResult 分发给所有 preview 窗口。
    bool SetPreviewServicesDirty(const OrthogonalCropResult& previewResult);

    // 在 3D volume 主窗口上执行一次 volume preview；仅接管 VTK mapper 能正确表达的模式。
    bool SetMainVolumePreviewApplied(PreviewRenderTarget& target, const OrthogonalCropResult& previewResult);

    // KeepInside 从统一 cropData 还原世界平面，再交给 VTK volume clipping 表达“只显示盒内”。
    void SetVolumeKeepInsidePreviewApplied(
        vtkVolumeMapper* volumeMapper,
        const OrthogonalCropResult& previewResult) const;

    // RemoveInside 用 GPU volume shader discard 表达旋转盒外补集。
    bool SetVolumeRemoveInsidePreviewApplied(
        vtkVolume* volume,
        vtkGPUVolumeRayCastMapper* volumeMapper,
        const OrthogonalCropResult& previewResult) const;

    // 把某个 3D 主窗口的持久 preview 管道恢复成全量直通状态。
    void RestoreMainPolyDataPreview(PreviewRenderTarget& target);

    // 在满足条件的 3D 主窗口上执行一次临时 polydata clip 预览。
    bool SetMainPolyDataPreviewApplied(PreviewRenderTarget& target, const OrthogonalCropResult& previewResult);

    // 后端分发器，负责 image / polydata 两条执行链。
    OrthogonalCropBackendRouterService m_backend;

    // Auto 模式下 image 输入的兜底来源。
    std::shared_ptr<AbstractDataManager> m_dataMgr;

    // widget 唯一挂载的主 interactor。
    vtkRenderWindowInteractor* m_primaryInteractor = nullptr;

    // 当前是否处于裁切交互模式。
    bool m_cropInteractionEnabled = false;

    // 当前 bounds 是否已经初始化过。
    bool m_boundsInitialized = false;

    // 当前是否有 preview 在显示。
    bool m_previewEnabled = false;

    // 最近一次 widget 交互阶段，用于区分 dragging / released。
    CropInteractionPhase m_lastInteractionPhase = CropInteractionPhase::Idle;

    // 当前 preview 真正使用的 removal mode。
    CropRemovalMode m_currentRemovalMode = CropRemovalMode::KeepInside;

    // 当前 preview 是否强制要求完整后端结果；关闭时会走轻量 preview 路径。
    bool m_previewRequiresFullArtifacts = true;

    // widget 当前 reference render 交互坐标 AABB；真实裁切盒以 GetCurrentLocalBox() 重建的有向盒为准。
    std::array<double, 6> m_currentBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

    // physical crop 重载期间持有提交缓冲，直到主线程回调收口。
    std::shared_ptr<std::vector<float>> m_reloadBuffer;

    // 只负责 vtkBoxWidget2 生命周期与状态同步，不承担业务计算。
    OrthogonalCropWidgetStateController m_widgetStateController;

    // 世界 / 模型坐标互转的参考窗口服务。
    std::shared_ptr<AbstractInteractiveService> m_referenceRenderService;

    // 真正需要被 overlay / preview 联动刷新的窗口目标列表。
    std::vector<PreviewRenderTarget> m_previewRenderTargets;
};
