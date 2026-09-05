#pragma once

#include "Data/ImageReadTypes.h"
#include "Data/TrustedImageState.h"
#include "Host/Types/HostViewTypes.h"
#include "Interaction/InteractionTypes.h"
#include "Render/Contracts/RenderEffect.h"

#include <cstddef>
#include <cstdint>
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

    virtual std::optional<ImageReadState> GetImageReadState() const = 0;
    virtual ImageReadResult GetImageReadResult(
        const ImageReadRequest& request) const = 0;
    virtual ImageReadChunkResult GetImageReadChunk(
        const ImageReadRequest& request,
        std::size_t voxelOffset) const = 0;
};

// Feature 只提交已经准备完成的场景变化意图；Host 在 owner frame 中重新校验
// 输入戳、统一置脏并决定提交与渲染时机。priority 只决定本轮处理资格，
// 不改变 Host 固定的 Geometry -> Camera -> Overlay 依赖顺序。
enum class FeatureScenePriority : std::uint8_t {
    Scene,
    Overlay,
    Refinement
};

enum class FeatureSceneScope : std::uint8_t {
    RequiredAllViews,
    TargetViewOnly,
    BestEffort
};

struct FeatureSceneDelta final {
    std::vector<std::string> viewIds;
    RenderInputStamp inputStamp;
    std::uint64_t requestId = 0;
    FeatureScenePriority priority = FeatureScenePriority::Scene;
    FeatureSceneScope scope = FeatureSceneScope::RequiredAllViews;
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
    virtual bool SendSceneDelta(FeatureSceneDelta delta) = 0;
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
