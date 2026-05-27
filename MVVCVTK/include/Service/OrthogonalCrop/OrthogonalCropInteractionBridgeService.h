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
#include "OrthogonalCropPreviewStrategy/OrthogonalCropPreviewOverlayStrategy.h"
#include "AppService.h"

#include <vtkActor.h>
#include <vtkPlanes.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkTransform.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class OrthogonalCropInteractionBridgeService {
public:
    // 交互桥接层启动时就把 widget 事件统一接到本类的状态更新入口，
    // 后续只需要维护 bounds/phase，不需要在 main 里直接碰 widget 细节。
    OrthogonalCropInteractionBridgeService()
    {
        m_widgetStateController.SetBoundsChangedCallback(
            [this](const std::array<double, 6>& bounds, CropInteractionPhase phase) {
                HandleWidgetBoundsChanged(bounds, phase);
            });
    }

    void CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image)
    {
        CallBackend(&OrthogonalCropBackendRouterService::CropPreInit_SetInputImage, std::move(image));
    }

    void SetInputImage(vtkSmartPointer<vtkImageData> image)
    {
        CropPreInit_SetInputImage(std::move(image));
    }

    vtkSmartPointer<vtkImageData> GetInputImage() const
    {
        return CallBackend(&OrthogonalCropBackendRouterService::GetInputImage);
    }

    void CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
    {
        CallBackend(&OrthogonalCropBackendRouterService::CropPreInit_SetInputPolyData, std::move(polyData));
    }

    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
    {
        CropPreInit_SetInputPolyData(std::move(polyData));
    }

    vtkSmartPointer<vtkPolyData> GetInputPolyData() const
    {
        return CallBackend(&OrthogonalCropBackendRouterService::GetInputPolyData);
    }

    void CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource)
    {
        CallBackend(&OrthogonalCropBackendRouterService::CropPreInit_SetPreferredDataSource, dataSource);
    }

    void SetPreferredDataSource(OrthogonalCropDataSource dataSource)
    {
        CropPreInit_SetPreferredDataSource(dataSource);
    }

    OrthogonalCropDataSource GetActiveDataSource() const
    {
        return CallBackend(&OrthogonalCropBackendRouterService::GetActiveDataSource);
    }

    std::array<double, 6> GetActiveInputBounds() const
    {
        return CallBackend(&OrthogonalCropBackendRouterService::GetActiveInputBounds);
    }

    OrthogonalCropRequest GetDefaultRequest() const
    {
        return CallBackend(&OrthogonalCropBackendRouterService::GetDefaultRequest);
    }

    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const
    {
        return CallBackend(&OrthogonalCropBackendRouterService::GetStatistics, request);
    }

    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const
    {
        return CallBackend(&OrthogonalCropBackendRouterService::GetResult, request);
    }

    // 数据管理器仅作为输入兜底来源：当外部还没显式 SetInputImage 时，
    // bridge 会在真正触发交互前尝试从 data manager 取当前 volume。
    void SetDataManager(std::shared_ptr<AbstractDataManager> dataMgr)
    {
        m_dataMgr = std::move(dataMgr);
    }

    // 主交互器绑定到 3D 参考窗口；widget 只挂在这一个 interactor 上。
    void SetPrimaryInteractor(vtkRenderWindowInteractor* interactor)
    {
        m_primaryInteractor = interactor;
        m_widgetStateController.SetInteractor(interactor);
    }

    // 参考服务只承担坐标系转换职责，用来把世界坐标裁切框映射回模型坐标。
    // 如果未显式设置，则在已有 preview 列表时回退到第一个 preview 服务。
    void SetReferenceRenderService(std::shared_ptr<AbstractInteractiveService> referenceService)
    {
        m_referenceRenderService = std::move(referenceService);
        // 如果外部没有单独提供参考服务，就退化成“借用第一个 preview 服务做坐标转换”。
        if (!m_referenceRenderService) {
            m_referenceRenderService = GetFirstPreviewRenderService();
        }
    }

    // preview 服务列表是真正需要联动刷新的目标窗口集合。
    // 它和 reference service 分开维护，避免所有注册窗口都被一律设脏。
    void SetPreviewRenderServices(std::vector<std::shared_ptr<AbstractInteractiveService>> previewRenderServices)
    {
        // 每次都按外部最新注册结果整体替换，避免旧窗口残留在 preview 联动列表里。
        ClearPreviewRenderTargets();
        m_previewRenderTargets.reserve(previewRenderServices.size());

        // 逐个经过 null/重复过滤，保证后续 SetDirtyMarked 不会重复打同一窗口。
        for (const auto& service : previewRenderServices) {
            AddPreviewRenderService(service);
        }

        // reference 未单独设置时，默认借用第一个 preview 服务做世界/模型坐标换算。
        if (!m_referenceRenderService) {
            m_referenceRenderService = GetFirstPreviewRenderService();
        }
    }

    bool ToggleInteractiveCrop()
    {
        return ActivateInteractiveCrop();
    }

    bool ExitInteractiveCrop()
    {
        return DeactivateInteractiveCrop();
    }

    void ToggleInsidePreview()
    {
        TogglePreview(CropRemovalMode::KeepInside, true);
    }

    void ToggleOutsidePreview()
    {
        TogglePreview(CropRemovalMode::RemoveInside, true);
    }

    // 回到默认裁切盒，并可选择是否立即推一次 preview。
    // 这个接口主要给外部显式 reset 状态用，不依赖重新建 widget。
    bool ResetInteractiveBoundsToDefault(bool updatePreview = true)
    {
        if (!EnsureInputReady()) {
            return false;
        }

        // 先把几何状态回到默认中心子盒，并把交互阶段重置成“已释放”。
        m_currentBounds = GetDefaultInteractiveBounds();
        m_boundsInitialized = true;
        m_lastInteractionPhase = CropInteractionPhase::Released;
        // 再把这份状态同步回 widget，保证 UI 和内部缓存重新对齐。
        m_widgetStateController.SetReferenceBounds(GetActiveWorldBounds());
        m_widgetStateController.SetWidgetBounds(m_currentBounds);
        // reset 是否立刻触发 preview 由调用方决定；但只有 preview 已开启时才真正裁预览。
        if (updatePreview && m_previewEnabled) {
            UpdatePreviewFromCurrentBounds(true);
        }
        else if (updatePreview) {
            RestorePreviewRenderTargets();
        }
        return true;
    }

    // 进入裁切模式时只初始化 widget 状态；preview 由 1/2 显式切换。
    // 后续拖拽过程中的同步由 widget callback 驱动。
    bool ActivateInteractiveCrop()
    {
        // 第一步：确认已经有活跃输入；Auto 模式下这里也会尝试走 data manager 兜底。
        if (!EnsureInputReady()) {
            std::cerr << "[Main] Orthogonal crop trigger failed: no active image/polydata input is available yet." << std::endl;
            return false;
        }

        // O 键再次触发时直接退出，保持 toggle 语义一致。
        if (m_cropInteractionEnabled) {
            DeactivateInteractiveCrop();
            return true;
        }

        // widget 只能挂在主 3D 交互器上，没有 interactor 就不允许进入模式。
        if (!m_primaryInteractor) {
            std::cerr << "[Main] Orthogonal crop widget init failed: primary interactor missing." << std::endl;
            return false;
        }

        // 第一次进入时懒初始化默认 bounds；后续再次进入则复用上一次退出时保留下来的 bounds。
        if (!m_boundsInitialized) {
            m_currentBounds = GetDefaultInteractiveBounds();
            m_boundsInitialized = true;
        }

        // 先把 widget 绑定到当前 interactor，并同步参考 bounds / 当前 bounds。
        m_widgetStateController.SetInteractor(m_primaryInteractor);
        m_widgetStateController.SetReferenceBounds(GetActiveWorldBounds());
        m_widgetStateController.SetWidgetBounds(m_currentBounds);
        // 真正开启 vtkBoxWidget2；如果 VTK 侧启用失败，就不要进入交互态。
        if (!m_widgetStateController.SetEnabled(true)) {
            std::cerr << "[Main] Orthogonal crop widget init failed: vtkBoxWidget2 could not be enabled." << std::endl;
            return false;
        }

        // widget 成功后只进入交互状态，不主动裁主模型；预览必须由 1/2 显式触发。
        m_cropInteractionEnabled = true;
        m_lastInteractionPhase = CropInteractionPhase::Released;
        RestorePreviewRenderTargets();
        std::cout << "[Main] Orthogonal crop widget active. UI uses vtkBoxWidget2, backend = "
            << GetDataSourceText(GetActiveDataSource())
            << ". Press 1 to toggle inside preview, press 2 to toggle outside preview; press O or Esc to exit." << std::endl;
        return true;
    }

    bool ExecuteDemo()
    {
        return ActivateInteractiveCrop();
    }

    // 退出裁切模式时保留当前 bounds，但撤掉 widget 交互占用，
    // 这样 3D 模型可继续旋转，同时临时 preview 会恢复成完整模型。
    bool DeactivateInteractiveCrop()
    {
        if (!m_cropInteractionEnabled) {
            return false;
        }

        // 先撤掉 widget 占用，让 3D 窗口立刻恢复普通导航交互。
        m_widgetStateController.SetEnabled(false);
        m_cropInteractionEnabled = false;
        // 退出时撤销临时预览，主模型回到完整状态。
        m_previewEnabled = false;
        m_lastInteractionPhase = CropInteractionPhase::Released;
        RestorePreviewRenderTargets();
        std::cout << "[Main] Orthogonal crop widget deactivated. 3D navigation restored." << std::endl;
        return true;
    }

    bool DeactivateDemo()
    {
        return DeactivateInteractiveCrop();
    }

private:
    template <typename BackendMethod, typename... Args>
    decltype(auto) CallBackend(BackendMethod method, Args&&... args)
    {
        return (m_backend.*method)(std::forward<Args>(args)...);
    }

    template <typename BackendMethod, typename... Args>
    decltype(auto) CallBackend(BackendMethod method, Args&&... args) const
    {
        return (m_backend.*method)(std::forward<Args>(args)...);
    }

    struct PreviewRenderTarget {
        std::shared_ptr<AbstractInteractiveService> service;
        std::shared_ptr<OrthogonalCropPreviewOverlayStrategy> overlayStrategy;
        vtkPolyDataMapper* mainPreviewMapper = nullptr;
        vtkSmartPointer<vtkPolyData> mainPreviewSourcePolyData;
    };

    // 统一把 min/max bounds 展开成 8 个角点，供世界/模型坐标转换复用。
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

    // 当前请求走 Auto 模式时，优先尝试从 data manager 兜底补齐 image 输入。
    // 返回值代表是否已经有可用后端，而不是简单判断 m_dataMgr 是否存在。
    bool EnsureInputReady()
    {
        if (GetActiveDataSource() != OrthogonalCropDataSource::Auto) {
            return true;
        }

        // 只有在外部尚未显式绑定 image 且存在 data manager 时，才做一次兜底同步。
        if (!GetInputImage() && m_dataMgr) {
            SetInputImage(m_dataMgr->GetVtkImage());
        }

        return GetActiveDataSource() != OrthogonalCropDataSource::Auto;
    }

    // 默认裁切盒定义在世界坐标系下，并按输入整体 bounds 的固定比例取中心子盒。
    std::array<double, 6> GetDefaultInteractiveBounds() const
    {
        std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        const auto sourceBounds = GetActiveWorldBounds();
        // 输入 bounds 非法时直接返回零盒，交给上层 decide 是否继续。
        if (!(sourceBounds[0] < sourceBounds[1]
            && sourceBounds[2] < sourceBounds[3]
            && sourceBounds[4] < sourceBounds[5])) {
            return bounds;
        }

        // 默认盒子不是全幅，而是按固定比例取一个中心子盒，便于用户一开始就能看见明显可拖拽范围。
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

    // 拖拽中只更新当前 bounds/phase；只有释放时才触发 preview，避免频繁重算。
    void HandleWidgetBoundsChanged(const std::array<double, 6>& bounds, CropInteractionPhase phase)
    {
        if (!m_cropInteractionEnabled) {
            return;
        }

        // 无论 dragging 还是 released，都先把最新几何状态缓存下来。
        m_currentBounds = bounds;
        m_boundsInitialized = true;
        m_lastInteractionPhase = phase;
        // 只有 preview 已开启且 released 才做重算，dragging 期间只保留轻量状态同步。
        if (m_previewEnabled && phase == CropInteractionPhase::Released) {
            UpdatePreviewFromCurrentBounds(true);
        }
    }

    // 所有 UI 交互都以世界坐标裁切盒为准，真正提交给算法前再转换回模型坐标。
    void GetWorldPositionFromModel(const double modelPos[3], double worldPos[3]) const
    {
        if (m_referenceRenderService) {
            m_referenceRenderService->GetWorldPositionFromModel(modelPos, worldPos);
            return;
        }

        // 没有参考服务时退化成恒等映射，保证非变换场景仍然可用。
        worldPos[0] = modelPos[0];
        worldPos[1] = modelPos[1];
        worldPos[2] = modelPos[2];
    }

    // 世界坐标 -> 模型坐标的反向映射，供 preview 请求提交给算法层前使用。
    void GetModelPositionFromWorld(const double worldPos[3], double modelPos[3]) const
    {
        if (m_referenceRenderService) {
            m_referenceRenderService->GetModelPositionFromWorld(worldPos, modelPos);
            return;
        }

        // 无参考服务时同样退化成恒等映射。
        modelPos[0] = worldPos[0];
        modelPos[1] = worldPos[1];
        modelPos[2] = worldPos[2];
    }

    // 通过角点逐点变换重新包围，确保任意模型矩阵下都能得到轴对齐 bounds。
    std::array<double, 6> GetTransformedBounds(const std::array<double, 6>& sourceBounds, bool modelToWorld) const
    {
        // 先用极值初始化包围盒，后面每个变换角点都会不断收紧/扩张这个范围。
        std::array<double, 6> transformedBounds = {
            std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()
        };

        const auto corners = GetCornersFromBounds(sourceBounds);
        for (const auto& corner : corners) {
            double transformedPoint[3] = { 0.0, 0.0, 0.0 };
            // 角点逐个做坐标变换，再重新计算轴对齐 bounds，避免直接变换 min/max 造成错误包围盒。
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

    // 活跃输入数据原始 bounds 默认在模型坐标系中，这里统一升到世界坐标供 widget 使用。
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

    // LocalCenterAndDimensions 需要一份 local -> model 矩阵；交互盒 local 定义在世界坐标中，
    // 因此这里使用 reference service 的 model matrix 反矩阵，把世界盒子映射回模型空间。
    std::array<double, 16> GetWorldToModelMatrix() const
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

    // preview 请求固定走 VirtualCrop，目的是做轻量联动预览，不在交互层做物理裁切。
    OrthogonalCropRequest BuildPreviewRequest() const
    {
        // 先拿后端默认 request，继承数据源、矩阵和默认执行语义。
        auto previewRequest = GetDefaultRequest();
        // 再把世界坐标中的 widget 盒子作为 local crop 定义交给算法，避免模型旋转后退化成过宽的轴对齐盒。
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

        // 最后把 UI 侧的启用状态和交互阶段打进 request，供后端结果和日志统一携带。
        auto cropState = previewRequest.GetCropStateModel();
        cropState.SetCropEnabled(m_cropInteractionEnabled);
        cropState.SetInteractionPhase(m_lastInteractionPhase);
        previewRequest.SetCropStateModel(cropState);
        return previewRequest;
    }

    // preview 只调一次 GetResult，并从结果里取 statistics，避免重复统计和重复裁切。
    void UpdatePreviewFromCurrentBounds(bool logStats)
    {
        if (!m_boundsInitialized) {
            return;
        }

        // 第一步：按当前世界 bounds 组装 request，并统一走后端 GetResult。
        const auto previewRequest = BuildPreviewRequest();
        const auto previewResult = GetResult(previewRequest);
        const auto& previewStats = previewResult.GetStatistics();
        // 第二步：优先处理 failure reason，避免后续把失败结果继续广播到 preview 窗口。
        if (previewResult.GetFailureReason() != OrthogonalCropFailureReason::None) {
            if (logStats) {
                std::cerr << "[Main] Orthogonal crop preview failed: "
                    << GetFailureReasonText(previewResult.GetFailureReason())
                    << " - " << previewResult.GetMessage() << std::endl;
            }
            return;
        }

        // 第三步：若后端返回的是 warning/unsuccessful 结果，也在这里提前拦掉。
        if (!previewResult.GetSucceeded()) {
            if (logStats) {
                std::cerr << "[Main] Orthogonal crop preview result warning: "
                    << GetFailureReasonText(previewResult.GetFailureReason())
                    << " - " << previewResult.GetMessage() << std::endl;
            }
            return;
        }

        // 第四步：只有成功结果才把 result 投递到 overlay，并通知真正的 preview 服务列表重绘。
        const bool main3DPreviewApplied = SetPreviewServicesDirty(previewResult);

        // 最后再做日志输出，打印此次 preview 真正落到的数据源、backend 和当前 bounds。
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
                << ", main3D = "
                << (main3DPreviewApplied ? "PolyDataClip" : "OverlayOnly")
                << ", bounds = ["
                << m_currentBounds[0] << ", " << m_currentBounds[1] << "; "
                << m_currentBounds[2] << ", " << m_currentBounds[3] << "; "
                << m_currentBounds[4] << ", " << m_currentBounds[5] << "]"
                << std::endl;
        }
    }

    // 1/2 的具体绑定在 main 中完成；bridge 只接收抽象 preview 动作。
    void TogglePreview(CropRemovalMode removalMode, bool logStats)
    {
        if (!m_cropInteractionEnabled) {
            return;
        }

        if (m_previewEnabled && m_currentRemovalMode == removalMode) {
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

        // 拖拽中不重算 preview；等释放后会走 bounds changed 的 released 分支统一刷新。
        if (m_cropInteractionEnabled
            && m_lastInteractionPhase != CropInteractionPhase::Dragging) {
            UpdatePreviewFromCurrentBounds(logStats);
        }
    }

    vtkSmartPointer<vtkPlanes> GetCurrentWidgetPlanesForModelInput() const
    {
        auto worldPlanes = vtkSmartPointer<vtkPlanes>::New();
        if (!m_widgetStateController.GetPlanes(worldPlanes)) {
            return nullptr;
        }

        if (m_referenceRenderService) {
            auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
            matrix->DeepCopy(m_referenceRenderService->GetModelMatrix().data());

            auto modelToWorldTransform = vtkSmartPointer<vtkTransform>::New();
            modelToWorldTransform->SetMatrix(matrix);
            worldPlanes->SetTransform(modelToWorldTransform);
        }

        return worldPlanes;
    }

    // 把 failureReason 枚举转成稳定日志文本，避免多个调用点重复写 switch。
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

    // 把 removal mode 转成日志/调试输出使用的文本。
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

    // 把数据源枚举转成日志文本。
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

    // 把最终 backend 枚举转成日志文本。
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

    // 返回 preview 列表里的第一个有效服务，供 reference service 未显式设置时兜底。
    std::shared_ptr<AbstractInteractiveService> GetFirstPreviewRenderService() const
    {
        for (const auto& target : m_previewRenderTargets) {
            if (target.service) {
                return target.service;
            }
        }
        return nullptr;
    }

    // 切换 preview 列表前，先把旧 overlay 从旧服务上摘掉，避免目标窗口残留旧叠加层。
    void ClearPreviewRenderTargets()
    {
        for (const auto& target : m_previewRenderTargets) {
            if (target.service && target.overlayStrategy) {
                target.service->SetOverlayStrategyRemoved(target.overlayStrategy);
            }
        }
        m_previewRenderTargets.clear();
    }

    void RestorePreviewRenderTargets()
    {
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

    // 向 preview 列表追加一个服务，同时做 null 和重复过滤。
    void AddPreviewRenderService(const std::shared_ptr<AbstractInteractiveService>& service)
    {
        // 空指针直接忽略，避免污染 preview 列表。
        if (!service) {
            return;
        }

        // 同一个服务只保留一份，防止后面重复挂 overlay 或重复 SetDirtyMarked。
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

    // 真正需要联动的只是 preview 服务列表，而不是所有注册过的服务。
    bool SetPreviewServicesDirty(const OrthogonalCropResult& previewResult)
    {
        bool main3DPreviewApplied = false;
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

    void RestoreMainPolyDataPreview(PreviewRenderTarget& target)
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

    bool SetMainPolyDataPreviewApplied(PreviewRenderTarget& target, const OrthogonalCropResult& previewResult)
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

    // 后端路由由 bridge 组合持有，避免交互层在类型上伪装成 backend service。
    OrthogonalCropBackendRouterService m_backend;
    // 输入兜底来源，只在 Auto 模式且外部尚未显式绑定输入时参与补齐。
    std::shared_ptr<AbstractDataManager> m_dataMgr;
    // widget 唯一挂载的 interactor，一般来自主 3D 参考窗口。
    vtkRenderWindowInteractor* m_primaryInteractor = nullptr;
    // 当前是否处于裁切交互模式；决定 widget 是否可见以及热键是否生效。
    bool m_cropInteractionEnabled = false;
    // 当前 bounds 是否已被初始化过；避免对零 bounds 做无效 preview。
    bool m_boundsInitialized = false;
    // 当前是否正在由 1/2 快捷键显示裁切预览。
    bool m_previewEnabled = false;
    // 记录最近一次 widget 交互阶段，防止拖拽过程中重复触发 preview。
    CropInteractionPhase m_lastInteractionPhase = CropInteractionPhase::Idle;
    // 当前真正下发给 preview 请求的 removal mode。
    CropRemovalMode m_currentRemovalMode = CropRemovalMode::KeepInside;
    // widget 当前维护的世界坐标 bounds，也是所有 preview 的唯一输入源。
    std::array<double, 6> m_currentBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // 只负责 vtkBoxWidget2 的生命周期和 UI 状态，不承担业务计算。
    OrthogonalCropWidgetStateController m_widgetStateController;
    // 只做坐标参考；外部也可以把同一个服务加入 preview 列表显示结果。
    std::shared_ptr<AbstractInteractiveService> m_referenceRenderService;
    // 真正需要跟随 preview 结果刷新的目标服务和对应 overlay。
    std::vector<PreviewRenderTarget> m_previewRenderTargets;
};
