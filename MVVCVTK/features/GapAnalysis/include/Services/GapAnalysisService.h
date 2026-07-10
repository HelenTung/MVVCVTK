#pragma once
// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/include/Services/GapAnalysisService.h
// GapAnalysisService.h - GapAnalysis 插件编排服务
// =====================================================================

#include "AppTypes.h"
#include "GapAnalysisTypes.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

class OverlayService;

class GapAnalysisService {
public:
    GapAnalysisService();
    ~GapAnalysisService();

    GapAnalysisService(const GapAnalysisService&) = delete;
    GapAnalysisService& operator=(const GapAnalysisService&) = delete;
    GapAnalysisService(GapAnalysisService&&) = delete;
    GapAnalysisService& operator=(GapAnalysisService&&) = delete;

    // 输入边界放在插件 API 上，而不是在 feature 内部持有 App/DataManager。
    // 这样 GapAnalysis 可以随插随拔：宿主只负责在合适时机交付 VTK 图像，后台任务只消费复制后的快照。
    bool SetInputImage(vtkSmartPointer<vtkImageData> image);

    void SetSurface(const SurfaceParams& params);
    void SetAdvanced(const AdvancedSurfaceParams& params);
    void SetVoid(const VoidDetectionParams& params);

    void StartAsync(std::function<void(bool isSuccess)> onComplete = nullptr);
    void StopAsync();

    bool GetDoneEvent();
    void SendCallback();

    GapAnalysisState GetAnalysisState() const;
    std::vector<VoidRegion> GetVoidRegions() const;

    vtkSmartPointer<vtkPolyData> BuildVoidMesh() const;
    vtkSmartPointer<vtkImageData> BuildLabelImage() const;

    // GapAnalysis 显示模式由 feature 持有状态；host 只注入已降级的 overlay 目标和主线程 tick。
    bool StartView(
        const GapAnalysisSurfaceRequest& surfaceRequest,
        const VoidDetectionParams& voidParams,
        const std::vector<std::shared_ptr<OverlayService>>& meshOverlayTargets,
        const std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>>& sliceOverlayTargets,
        std::function<void(double isoValue)> onIsoValueResolved = nullptr);
    bool SwitchOverlay();
    bool ExitView();
    bool GetViewOn() const;
    void OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
