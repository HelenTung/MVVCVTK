#pragma once

#include "Host/HostFeature.h"
#include "Host/GapHostTypes.h"

#include <functional>
#include <memory>
#include <optional>

enum class GapHostAction {
    None,
    Start,
    Overlay,
    Exit,
    SetDisplay, // owner thread 的短显示命令；失败保留原显示。
    Evaluate
};

struct GapHostStartParams {
    HostViewTargets targetViews;
    GapSurfaceConfig surface;
    GapVoidParams voidParams;
    std::optional<DataRevisionRef> evaluationRoi, detectionRoi;
    GapRoiPolicy roiPolicy = GapRoiPolicy::WholeIfIntersects;
    bool isEvaluationOnly = false;
    std::optional<DataRevisionRef> parentResult;
};

struct GapHostRequest {
    GapHostAction action = GapHostAction::None;
    std::optional<GapHostStartParams> start;
    std::optional<GapDisplayParams> display;
    DataRevisionRef displayResult;
};

struct GapHostKeys {
    HostKeyChord switchOverlay;
    HostKeyChord exit;
};

struct GapHostConfig {
    GapHostStartParams defaultStart;
    HostViewTargets inputViews;
    GapHostKeys keys;
};

using GapHostCallback =
    std::function<void(GapHostResult result)>;

class GapHostFeature final
    : public HostFeature
    , public std::enable_shared_from_this<GapHostFeature> {
public:
    explicit GapHostFeature(GapHostConfig config);
    ~GapHostFeature() noexcept override;

    GapHostFeature(const GapHostFeature&) = delete;
    GapHostFeature& operator=(const GapHostFeature&) = delete;
    GapHostFeature(GapHostFeature&&) = delete;
    GapHostFeature& operator=(GapHostFeature&&) = delete;

    std::string_view GetFeatureId() const noexcept override;
    FeatureDataContract GetDataContract() const override;
    std::vector<FeatureOperationState> GetOperationStates() const override;
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;

    // 拒绝不回调；Accepted 恰好一次 owner 回调，短命令可同步完成。
    // Exit 在视图解绑完成后兑现；Stop/Detach 使未完成退出以失败终态收口。
    GapAdmission SendRequest(
        GapHostRequest request,
        GapHostCallback onComplete = nullptr);
    GapHostState GetState() const;
    std::optional<GapDisplayParams> GetDisplay(const DataRevisionRef& result) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
