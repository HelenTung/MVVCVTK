#pragma once

#include "OrthogonalCropTypes.h"
#include "App/Services/FeatureViewService.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class vtkRenderWindowInteractor;
class vtkRenderer;

struct CropViewRequest final {
    vtkRenderWindowInteractor* interactor = nullptr;
    vtkRenderer* renderer = nullptr;
    std::weak_ptr<const FeatureViewLease> lease;
    std::shared_ptr<FeatureViewService> referenceService;
    std::vector<std::shared_ptr<FeatureViewService>> targetServices;
};

using CropBuildCallback = std::function<void(CropBuildResult)>;

class CropBridge final {
private:
    class Impl;

public:
    // 发布令牌只拥有已完成分配和验证的历史候选；销毁令牌等价于放弃提交。
    class PreparedCommit final {
    public:
        ~PreparedCommit();
        PreparedCommit(PreparedCommit&&) noexcept;
        PreparedCommit& operator=(PreparedCommit&&) noexcept;

        PreparedCommit(const PreparedCommit&) = delete;
        PreparedCommit& operator=(const PreparedCommit&) = delete;

    private:
        friend class CropBridge;
        friend class CropBridge::Impl;

        class Impl;
        explicit PreparedCommit(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> m_impl;
    };

    CropBridge();
    ~CropBridge();

    CropBridge(const CropBridge&) = delete;
    CropBridge& operator=(const CropBridge&) = delete;

    bool StartView(const CropViewRequest& request);
    // Host 已冻结输入候选时，把 input 与 view binding 作为一次事务提交。
    bool StartView(
        const CropViewRequest& request,
        CropInputSnapshot input);
    bool ClearBindings();
    bool SetCropInput(CropInputSnapshot input);
    // DataManager 发布前完成全部可失败准备；baseNodeCount=0 表示显式恢复原始基线。
    std::optional<PreparedCommit> BuildCropCommit(
        CropInputSnapshot input,
        std::size_t baseNodeCount);
    // 外部快照成功发布后接管准备令牌；这里只移动内部状态，不检查 lease，也不调用外部端口。
    void SetCropCommit(PreparedCommit&& prepared) noexcept;
    // 状态接管后的渲染通知可以延迟重试，不参与数据/历史提交结果。
    bool SendCropCommit() noexcept;
    bool SwitchCropBox();
    bool SwitchCropPlane();
    bool SetCropMode(CropRemovalMode removalMode);
    bool PreviousCrop();
    bool NextCrop();
    bool SetCropNode(std::size_t nodeCount);
    bool ExitCrop();
    bool GetCropActive() const;
    // binding 生命周期独立于 widget 编辑态；Exit 后仍可导航 committed history。
    bool GetCropBound() const;
    CropHistoryState GetCropHistory() const;

    bool GetShaderTickNeeded() const;
    bool SendShaderCommit();
    // 从 rootInput 对完整 allHistory 前缀做一次融合物化，不生成节点级中间 mask。
    bool BuildCropResult(
        CropInputSnapshot rootInput,
        CropBuildCallback onComplete);
    bool GetBuildTickNeeded() const;
    bool SendBuildResult();

private:
    std::unique_ptr<Impl> m_impl;
};
