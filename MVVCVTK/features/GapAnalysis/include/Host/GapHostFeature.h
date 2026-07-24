#pragma once

#include "GapAnalysisTypes.h"
#include "Host/HostFeature.h"

#include <functional>
#include <memory>
#include <variant>

enum class GapHostAction {
    None,
    Start,
    Overlay,
    Exit
};

struct GapHostStartRequest {
    HostViewTargets targetViews;
    GapSurfaceRequest surface;
    GapVoidParams voidParams;
};

using GapHostPayload = std::variant<
    std::monostate,
    GapHostStartRequest>;

struct GapHostRequest {
    GapHostAction action = GapHostAction::None;
    GapHostPayload payload;
};

struct GapHostKeys {
    HostKeyChord switchOverlay;
    HostKeyChord exit;
};

struct GapHostConfig {
    GapHostStartRequest defaultStart;
    HostViewTargets inputViews;
    GapHostKeys keys;
};

using GapHostCallback =
    std::function<void(bool isSuccess)>;

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
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;

    bool SendRequest(
        GapHostRequest request,
        GapHostCallback onComplete = nullptr);
    GapHostState GetState() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
