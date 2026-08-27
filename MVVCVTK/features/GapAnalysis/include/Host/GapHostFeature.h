#pragma once

#include "Host/HostFeature.h"
#include "Host/GapHostTypes.h"
#include "Host/Types/HostInputTypes.h"

#include <functional>
#include <memory>
#include <optional>

enum class GapHostAction {
    None,
    Start,
    Overlay,
    Exit
};

struct GapHostStartParams {
    HostViewTargets targetViews;
    GapSurfaceConfig surface;
    GapVoidParams voidParams;
};

struct GapHostRequest {
    GapHostAction action = GapHostAction::None;
    std::optional<GapHostStartParams> start;
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
