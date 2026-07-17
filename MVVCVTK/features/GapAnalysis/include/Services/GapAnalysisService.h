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

struct GapViewRequest final {
    vtkSmartPointer<vtkImageData> inputImage; // 可选 VTK 输入；display tick 经公共隔离入口转换为分析快照。
    GapSurfaceRequest surface; // 等值面阈值与表面构建参数的本次请求副本。
    GapVoidParams voidParams; // 灰度、最小体积、方向张量和腐蚀参数快照。
    std::vector<std::shared_ptr<OverlayService>> meshTargets; // 接收 3D void mesh overlay 的目标服务。
    std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>> sliceTargets; // 轴向与 2D label overlay 目标配对。
};

// 孔隙分析的异步 worker 与主线程 overlay 会话编排器；算法只在快照上运行，VTK 挂接集中在 display tick。
class GapAnalysisService {
public:
    GapAnalysisService();
    // 活动显示会话必须先在绑定宿主线程调用 ExitView；退出后可在任意线程释放最后 owner。
    ~GapAnalysisService();

    GapAnalysisService(const GapAnalysisService&) = delete;
    GapAnalysisService& operator=(const GapAnalysisService&) = delete;
    GapAnalysisService(GapAnalysisService&&) = delete;
    GapAnalysisService& operator=(GapAnalysisService&&) = delete;

    // 外部可变输入先 DeepCopy，调用返回后修改 metadata/scalars 不会污染分析快照。
    bool SetInputImage(vtkSmartPointer<vtkImageData> image);
    void SetSurface(const GapSurfaceParams& params);
    void SetAdvanced(const GapAdvancedParams& params);
    void SetVoid(const GapVoidParams& params);

    // 领取当前 VolumeBuffer 与参数副本后启动 worker；返回值表示请求是否被真实接纳。
    // 完成链发布执行状态、成功结果和可选 pending callback，调用方通过 GetDoneEvent/SendCallback 消费回调。
    bool StartAsync(std::function<void(bool isSuccess)> onComplete = nullptr);
    // 只发布停止请求，不等待线程退出；已结束的 worker 线程槽由下一次真正启动或析构时 join。
    void StopAsync();

    // 原子领取一次 callback 门铃；返回 true 后应在宿主期望的线程调用 SendCallback。
    bool GetDoneEvent();
    // 取走 pending callback 后在锁外执行；本函数不负责切换线程。
    void SendCallback();

    GapAnalysisState GetAnalysisState() const;
    std::vector<VoidRegion> GetVoidRegions() const;

    vtkSmartPointer<vtkPolyData> BuildVoidMesh() const;
    vtkSmartPointer<vtkImageData> BuildLabelImage() const;

    // GapAnalysis 显示模式由 feature 持有状态；host 只注入已降级的 overlay 目标和主线程 tick。
    // 本入口清理旧显示后登记新请求并置启动门铃；至少一个有效 target 才接受，worker 由后续 OnDisplayTick 启动。
    // 首次成功调用绑定当前宿主线程；本组显示会话接口必须继续由该线程调用。
    bool StartView(
        GapViewRequest request,
        std::function<void(bool isSuccess)> onComplete = nullptr);
    bool SwitchOverlay();
    // 清除显示会话与已挂载 overlay；若 worker 正在执行，仅发布停止请求。
    bool ExitView();
    void ClearView();
    bool GetViewOn() const;
    bool GetDisplayTickNeeded() const;
    // 宿主主线程 tick：非空 image 走公共隔离入口；空 image 复用预先 SetInputSnapshot 的只读输入。
    // 非绑定线程调用会被拒绝，不读取或修改 VTK/overlay 会话状态。
    void OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
