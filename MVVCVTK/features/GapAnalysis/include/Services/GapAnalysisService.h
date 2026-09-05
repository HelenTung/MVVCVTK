#pragma once
// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/include/Services/GapAnalysisService.h
// GapAnalysisService.h - GapAnalysis 插件编排服务
// =====================================================================

#include "App/ViewTypes.h"
#include "Data/TrustedImageState.h"
#include "GapAnalysisTypes.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

class OverlayService;
class TrustedLabelMapPort;

struct GapViewRequest final {
    // GapHost 从 TrustedFeatureDataPort 取得的不可变 owner；存在时只共享 scalar，不复制整卷。
    TrustedImageSnapshot trustedInput;
    // Host 显示路径必须提供 Feature-scoped Store 端口；直接算法入口不经过本字段。
    std::shared_ptr<TrustedLabelMapPort> labelMaps;
    // 低层直接调用使用的可变输入；StartView 同步 DeepCopy 后才接纳 worker。
    vtkSmartPointer<vtkImageData> inputImage;
    // 当前私有内核没有有效域接口；仅允许为空，非空请求会被明确拒绝且不会触发本地补算。
    vtkSmartPointer<vtkImageData> validityMask;
    GapSurfaceConfig surface; // DefX 材料均值与等值面阈值配置快照。
    GapVoidParams voidParams; // DefX 过滤开关与最小体积参数快照。
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
    bool SetGapInput(vtkSmartPointer<vtkImageData> image);
    void SetSurface(const GapSurfaceParams& params);
    void SetVoid(const GapVoidParams& params);

    // 领取当前只读输入快照与参数副本后启动 worker；返回值表示请求是否被真实接纳。
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
    GapStatistics GetStatistics() const;

    vtkSmartPointer<vtkPolyData> BuildVoidMesh() const;
    vtkSmartPointer<vtkImageData> BuildLabelImage() const;

    // GapAnalysis 显示模式由 feature 持有状态；host 只注入已降级的 overlay 目标和主线程 tick。
    // 本入口先在局部冻结 image、拒绝非空 mask、校验参数和 target；返回 true 时 worker 已被接纳。
    // 任一准备步骤失败时，既有 overlay、callback 和 owner-thread binding 保持不变。
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
    // 宿主主线程 tick 只消费已接纳 worker 的终态；inputImage 保留为兼容形参，不再延迟启动任务。
    // 非绑定线程调用会被拒绝，不读取或修改 VTK/overlay 会话状态。
    void OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
