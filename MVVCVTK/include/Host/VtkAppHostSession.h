#pragma once

#include "Data/ImageReadTypes.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/Types/HostSessionTypes.h"

#include <memory>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include <vtkSmartPointer.h>

class HostFeature;

// 一次 VTK 宿主会话的顶层 owner：惰性组装共享数据/状态服务、窗口拓扑、feature 注册、
// 命令路由和 standalone 输入适配。外部只通过 HostRequest/Endpoint 交互，不直接依赖 AppService。
// Endpoint 中的 VTK 裸指针由本会话内部窗口持有；会话移动、重建或析构后不得继续缓存使用。
class VtkAppHostSession final {
public:
    explicit VtkAppHostSession(HostSessionConfig config);
    ~VtkAppHostSession();

    VtkAppHostSession(const VtkAppHostSession&) = delete;
    VtkAppHostSession& operator=(const VtkAppHostSession&) = delete;
    VtkAppHostSession(VtkAppHostSession&&) noexcept;
    VtkAppHostSession& operator=(VtkAppHostSession&&) noexcept;

    // 幂等构建；首次成功后复用既有服务和窗口，空 renderViews 或任一构建步骤失败返回 false。
    bool BuildSession();
    // 把主线程 TimerEvent pump 绑定到指定视图；重复调用会替换旧 timer handler。
    bool AttachTimer(const HostTimerConfig& config);
    // 替换 standalone 按键 observer；主体热键直接进入统一请求路由。
    bool AttachHotkeys(const HostHotkeyConfig& config);
    bool AttachFeature(const std::shared_ptr<HostFeature>& feature);
    bool DetachFeature(const HostFeature& feature);
    // 仅用于 standalone VTK 事件循环；Qt host 已有外部事件循环时不调用。
    bool Start();
    // owner thread 显式停止；幂等且失败后保留未完成阶段，可修复后重试。
    bool Stop() noexcept;
    bool GetIsStopped() const noexcept;
    HostStopState GetStopState() const noexcept;
    // Qt owner thread 可在 dispatcher 拒绝/抛异常或首次 Stop 失败后显式泵送滞留项。
    static bool SendPendingStops() noexcept;
    static std::size_t GetPendingStopCount() noexcept;

    // 兼容旧调用方；SDK consumer 应迁移到结果语义稳定的 SendRequestResult。
#if defined(MVVCVTK_SDK_CONSUMER)
    [[deprecated("Use SendRequestResult to separate acceptance from outcome.")]]
#endif
    bool SendRequest(
        HostRequest&& request,
        HostCompleteCallback onComplete = nullptr);
    // 推荐接口：投递失败也同步返回一次结构化结果，callback 对每个请求只执行一次。
    bool SendRequestResult(
        HostRequest&& request,
        HostResultCallback onComplete);

    // 返回会话内部 endpoint 集合的只读引用；引用和元素地址只在本会话拓扑不变且存活期间有效。
    const std::vector<HostRenderViewEndpoint>& GetRenderViewEndpoints();
    const HostRenderViewEndpoint* GetRenderViewEndpoint(const std::string& viewId);
    const HostRenderViewEndpoint* GetPrimaryEndpoint();
    // 返回 Session 拥有的值语义输入端点；Session move 后必须从目标对象重新获取。
    HostInputEndpoint* GetInputEndpoint() noexcept;
    // 返回指定视图的独立状态快照；查询失败返回空，不改变既有 SendRequest 流程。
    std::optional<HostRenderViewState> GetRenderViewState(
        const HostViewTarget& target);
    // 按会话拓扑顺序返回所有视图的独立状态快照。
    std::vector<HostRenderViewState> GetRenderViewStates();
    // 仅 owner thread 可读；精确 id 可返回 isAvailable=false 的已知 View，
    // role 仍选择首个可用 View；失败不回退，也不改变 View、Feature 或 VTK 状态。
    std::optional<HostSceneViewState> GetSceneViewState(
        const HostViewTarget& target);
    // 仅 owner thread 可读；按配置顺序返回全部 View，包括当前不可用的 View。
    std::vector<HostSceneViewState> GetSceneViewStates();
    // 深拷贝当前体素为不含 VTK identity 的只读值；无有效体数据时返回空。
    std::optional<ImageReadState> GetImageReadState();
    // 扩展接口：在分配前检查同步复制预算，并返回稳定失败原因与所需字节数。
    ImageReadResult GetImageReadResult(
        std::size_t maxReadBytes = imageReadLimit);
    // region 使用相对源图像的半开区间；成功结果的 extent 从 0 开始。
    ImageReadResult GetImageReadResult(
        const ImageReadRequest& request);
    // 每次最多复制 8 MiB，并以 region 内 x-fast voxel offset 续读。
    ImageReadChunkResult GetImageReadChunk(
        const ImageReadRequest& request,
        std::size_t voxelOffset);
    // worker 只复制不可变快照；结果在 owner timer 上回调。一次会话只接纳一个未回调读取。
    ImageReadAdmission StartImageRead(
        ImageReadRequest request,
        ImageReadCallback onComplete);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
