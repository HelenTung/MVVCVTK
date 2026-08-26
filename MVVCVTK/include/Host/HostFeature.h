#pragma once

#include "Data/ImageReadTypes.h"
#include "Data/TrustedImageState.h"
#include "Host/Types/HostFeatureViewTypes.h"
#include "Host/Types/HostValueTypes.h"
#include "Interaction/InteractionTypes.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class FeatureViewService;
class OverlayService;

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

    virtual std::optional<ImageReadState> GetImageReadState() const = 0;
    virtual ImageReadResult GetImageReadResult(
        const ImageReadRequest& request) const = 0;
    virtual ImageReadChunkResult GetImageReadChunk(
        const ImageReadRequest& request,
        std::size_t voxelOffset) const = 0;
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
