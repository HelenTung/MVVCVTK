#pragma once

#include "Data/ImageReadTypes.h"
#include "Data/TrustedImageState.h"
#include "Host/Types/HostViewTypes.h"
#include "Interaction/InteractionTypes.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class FeatureViewService;
class FeatureViewLease;
class OverlayService;
class vtkRenderer;
class vtkRenderWindowInteractor;

struct HostFeatureView final {
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
};

struct HostInputView final {
    HostFeatureView view;
    vtkRenderer* renderer = nullptr;
    vtkRenderWindowInteractor* interactor = nullptr;
    std::weak_ptr<const FeatureViewLease> lease;
};

// 所有 Feature 共用一份输入基元；具体 Feature 的按键集合留在各自模块。
struct HostKeyChord {
    char keyCode = 0;
    std::string keySym;
    bool isCtrlDown = false;
    bool isAltDown = false;
    bool isShiftDown = false;
};

struct HostInputBinding final {
    std::string featureId;
    HostViewTargets targetViews;
    std::function<InteractionResult(const InteractionEvent&)> onInput;
};

class HostInputPort {
public:
    virtual ~HostInputPort() noexcept = default;

    virtual bool AttachInput(HostInputBinding binding) = 0;
    virtual bool DetachInput(std::string_view featureId) = 0;
};

class FeatureViewDirectory {
public:
    virtual ~FeatureViewDirectory() noexcept = default;

    virtual std::vector<HostFeatureView> GetViews(
        const HostViewTargets& targets) const = 0;
    // 视图 DTO 只描述身份；可调用能力按稳定 id 单独发放。
    virtual std::shared_ptr<FeatureViewService> GetFeaturePort(
        const std::string& viewId) const = 0;
    virtual std::shared_ptr<OverlayService> GetOverlayPort(
        const std::string& viewId) const = 0;
    virtual std::optional<HostInputView> GetInputView(
        const HostViewTarget& target) const = 0;
};

class TrustedFeatureDataPort {
public:
    virtual ~TrustedFeatureDataPort() noexcept = default;

    virtual TrustedImageSnapshot GetImageSnapshot() const = 0;
    virtual bool SetImageState(
        TrustedImageState imageState,
        const TrustedImageSnapshot& expected,
        TrustedImageSnapshot& published) = 0;
};

// 普通只读端口不暴露 VTK identity；可信 Feature 也通过同一值语义读取边界。
class ImageReadPort {
public:
    virtual ~ImageReadPort() noexcept = default;

    virtual std::optional<ImageDescriptor> GetImageDescriptor() const = 0;
    virtual std::optional<ImageReadState> GetImageReadState() const = 0;
    virtual ImageReadResult GetImageReadResult(
        const ImageReadRequest& request) const = 0;
    virtual ImageReadChunkResult GetImageReadChunk(
        const ImageReadRequest& request,
        std::size_t voxelOffset) const = 0;
};

struct TrustedLabelMapCandidate final {
    std::string id;
    std::string displayName;
    std::string datasetId;
    DataVersion sourceVersion = 0;
    std::optional<LabelMapVersion> expectedVersion;
    vtkSmartPointer<vtkImageData> image;
};

struct TrustedLabelMapState final {
    LabelMapDescriptor descriptor;
    // 只在可信 SPI 内按不可变快照使用；普通 Host 读取永不获得该 VTK identity。
    vtkSmartPointer<vtkImageData> image;
};

using TrustedLabelMapSnapshot =
    std::shared_ptr<const TrustedLabelMapState>;

struct TrustedLabelMapStageResult final {
    LabelMapError error = LabelMapError::Unavailable;
    LabelMapStageToken token = 0;
    TrustedLabelMapSnapshot candidate;
};

struct TrustedLabelMapCommitResult final {
    LabelMapError error = LabelMapError::Unavailable;
    TrustedLabelMapSnapshot published;
};

struct TrustedLabelMapRemoveResult final {
    LabelMapError error = LabelMapError::Unavailable;
    bool isRemoved = false;
    LabelMapVersion removedVersion = 0;
};

// 每个 AttachHost 获得绑定自身 Feature ID 的端口；写操作只允许 session owner thread。
class TrustedLabelMapPort {
public:
    virtual ~TrustedLabelMapPort() noexcept = default;

    virtual std::vector<LabelMapDescriptor> GetLabelMapDescriptors() const = 0;
    virtual std::optional<LabelMapDescriptor> GetLabelMapDescriptor(
        std::string_view id) const = 0;
    virtual LabelMapReadResult GetLabelMapReadResult(
        const LabelMapReadRequest& request) const = 0;
    virtual LabelMapReadChunkResult GetLabelMapReadChunk(
        const LabelMapReadRequest& request,
        std::size_t voxelOffset) const = 0;
    virtual TrustedLabelMapSnapshot GetLabelMapSnapshot(
        std::string_view id) const = 0;
    virtual TrustedLabelMapStageResult StageLabelMap(
        TrustedLabelMapCandidate candidate) = 0;
    virtual TrustedLabelMapCommitResult CommitLabelMap(
        LabelMapStageToken token) = 0;
    virtual bool DiscardLabelMapStage(
        LabelMapStageToken token) noexcept = 0;
    virtual TrustedLabelMapRemoveResult RemoveLabelMap(
        std::string_view id,
        std::optional<LabelMapVersion> expectedVersion) = 0;
};

class FeatureHostControl : public HostInputPort {
public:
    ~FeatureHostControl() noexcept override = default;

    // Feature 只提交稳定 view id；Host 验证归属并维护活动来源事务。
    virtual bool SetActiveViews(
        const std::vector<std::string>& viewIds) = 0;
    // 状态文本由 Host 映射到窗口；Feature 不获得 context/window 具体对象。
    virtual bool SetViewStatus(
        const std::vector<std::string>& viewIds,
        const std::string& status) = 0;
    virtual bool SendOwnerComplete(std::function<void()> complete) = 0;
};

struct HostFeatureContext final {
    std::shared_ptr<FeatureViewDirectory> views;
    std::shared_ptr<ImageReadPort> read;
    std::shared_ptr<TrustedFeatureDataPort> data;
    std::shared_ptr<TrustedLabelMapPort> labelMaps;
    std::shared_ptr<FeatureHostControl> host;
};

class HostFeature {
public:
    virtual ~HostFeature() noexcept = default;

    virtual std::string_view GetFeatureId() const noexcept = 0;
    virtual bool AttachHost(const HostFeatureContext& context) = 0;
    virtual bool DetachHost() = 0;
    virtual bool OnHostTick() = 0;
};
