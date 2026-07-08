#pragma once
// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/include/Services/GapAnalysisService.h
// GapAnalysisService.h - GapAnalysis 插件编排服务
// =====================================================================

#include "Algorithms/VolumeBuffer.h"
#include "AppTypes.h"
#include "GapAnalysisTypes.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

class AbstractAppService;
class AbstractVisualStrategy;
struct GapParamSnapshot {
    SurfaceParams surfParams;
    AdvancedSurfaceParams advParams;
    VoidDetectionParams voidParams;
};

struct GapOverlayBinding {
    std::shared_ptr<AbstractAppService> service;
    std::shared_ptr<AbstractVisualStrategy> overlayStrategy;
};

class GapAnalysisService : public IGapAnalysisService {
public:
    GapAnalysisService();
    ~GapAnalysisService() override;

    GapAnalysisService(const GapAnalysisService&) = delete;
    GapAnalysisService& operator=(const GapAnalysisService&) = delete;
    GapAnalysisService(GapAnalysisService&&) = delete;
    GapAnalysisService& operator=(GapAnalysisService&&) = delete;

    // 输入边界放在插件 API 上，而不是在 feature 内部持有 App/DataManager。
    // 这样 GapAnalysis 可以随插随拔：宿主只负责在合适时机交付 VTK 图像，后台任务只消费复制后的快照。
    bool SetInputImage(vtkSmartPointer<vtkImageData> image) override;

    void SetSurface(const SurfaceParams& p) override;
    void SetAdvanced(const AdvancedSurfaceParams& p) override;
    void SetVoid(const VoidDetectionParams& p) override;

    void StartAsync(std::function<void(bool isSuccess)> onComplete = nullptr) override;
    void StopAsync() override;

    bool GetDoneEvent() override;
    void SendCallback() override;

    GapAnalysisState GetAnalysisState() const override;
    std::vector<VoidRegion> GetVoidRegions() const override;

    vtkSmartPointer<vtkPolyData> BuildVoidMesh() const override;
    vtkSmartPointer<vtkImageData> BuildLabelImage() const override;

    // GapAnalysis 显示模式由 feature 持有状态；host 只注入已降级的 overlay 目标和主线程 tick。
    bool StartView(
        const GapAnalysisSurfaceRequest& surfaceRequest,
        const VoidDetectionParams& voidParams,
        const std::vector<std::shared_ptr<AbstractAppService>>& meshOverlayTargets,
        const std::vector<std::pair<Orientation, std::shared_ptr<AbstractAppService>>>& sliceOverlayTargets,
        std::function<void(double isoValue)> onIsoValueResolved = nullptr);
    bool SwitchOverlay();
    bool ExitView();
    bool GetViewOn() const;
    void OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage);

private:
    using VolumeBufferSnapshot = std::shared_ptr<const VolumeBuffer>;

    VolumeBufferSnapshot GetInputSnapshot() const;
    GapParamSnapshot GetParamSnapshot() const;

    void StartWorker(
        VolumeBufferSnapshot inputSnapshot,
        GapParamSnapshot params);
    void StopWorker();
    void SetAnalysisState(GapAnalysisState state);

    bool StartRun(vtkSmartPointer<vtkImageData> inputImage);
    void SetDisplayView();
    bool SetOverlayOff();
    bool SetStoredView();
    void ClearDisplayState();
    double GetDisplayIso(const VolumeBuffer& inputSnapshot) const;

    mutable std::mutex m_inputMutex;
    VolumeBufferSnapshot m_inputSnapshot;

    mutable std::mutex m_paramsMutex;
    SurfaceParams m_surfParams;
    AdvancedSurfaceParams m_advParams;
    VoidDetectionParams m_voidParams;

    mutable std::mutex m_resultMutex;
    GapAnalysisResult m_result;

    // service 拥有后台线程的生命周期；析构 join 只保证任务不会访问已销毁对象，
    // 不把 service 变成进程生命周期管理器，也不让 feature 了解宿主对象树。
    mutable std::mutex m_workerMutex;
    std::thread m_workerThread;
    std::atomic<bool> m_isStopping{ false };
    std::atomic<int> m_analysisState{ static_cast<int>(GapAnalysisState::Idle) };
    class Impl;
    std::unique_ptr<Impl> m_impl;

    std::vector<std::shared_ptr<AbstractAppService>> m_meshTargets;
    std::vector<std::pair<Orientation, std::shared_ptr<AbstractAppService>>> m_sliceTargets;
    std::vector<GapOverlayBinding> m_displayOverlayBindings;
    vtkSmartPointer<vtkPolyData> m_displayVoidMesh;
    vtkSmartPointer<vtkImageData> m_displayLabelImage;
    GapAnalysisSurfaceRequest m_displaySurfaceRequest;
    VoidDetectionParams m_displayVoidParams;
    std::function<void(double isoValue)> m_isoCallback;
    bool m_isViewOn = false;
    bool m_isOverlayOn = false;
    bool m_hasRunRequest = false;
    bool m_hasDone = false;
    bool m_hasFailLog = false;
};
