#pragma once
// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/include/Services/GapAnalysisService.h
// GapAnalysisService.h - GapAnalysis 插件编排服务
// =====================================================================

#include "Algorithms/VolumeBuffer.h"
#include "GapAnalysisTypes.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class GapAnalysisCompletionCallbackState;

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

    void SetSurfaceParams(const SurfaceParams& p) override;
    void SetAdvancedParams(const AdvancedSurfaceParams& p) override;
    void SetVoidParams(const VoidDetectionParams& p) override;

    void RunAsync(std::function<void(bool success)> onComplete = nullptr) override;
    void CancelRun() override;

    bool ConsumePendingCompletionCallback() override;
    void ExecutePendingCompletionCallback() override;

    GapAnalysisState GetAnalysisState() const override;
    std::vector<VoidRegion> GetVoidRegions() const override;

    vtkSmartPointer<vtkPolyData> BuildVoidMesh() const override;
    vtkSmartPointer<vtkImageData> BuildLabelImage() const override;

private:
    using VolumeBufferSnapshot = std::shared_ptr<const VolumeBuffer>;

    struct ParameterSnapshot {
        SurfaceParams surfParams;
        AdvancedSurfaceParams advParams;
        VoidDetectionParams voidParams;
    };

    VolumeBufferSnapshot GetInputSnapshot() const;
    ParameterSnapshot GetParameterSnapshot() const;

    void RunAnalysisWorker(
        VolumeBufferSnapshot inputSnapshot,
        ParameterSnapshot params);
    void WaitForWorkerThread();
    void SetAnalysisState(GapAnalysisState state);

    static bool BuildVolumeBuffer(
        vtkSmartPointer<vtkImageData> image,
        VolumeBuffer& out);
    static vtkSmartPointer<vtkImageData> BuildLabelImage(
        const std::vector<int>& labelVolume,
        const VolumeBuffer& volBuf);

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
    std::atomic<bool> m_cancelFlag{ false };
    std::atomic<int> m_analysisState{ static_cast<int>(GapAnalysisState::Idle) };

    // 插件内部 pending callback 状态；只暴露主线程轮询接口，不反向 include App 层。
    std::unique_ptr<GapAnalysisCompletionCallbackState> m_completionCallbackState;
};
