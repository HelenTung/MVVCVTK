#pragma once

#include "Host/HostFeature.h"
#include "Host/GapHostTypes.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

enum class GapHostAction {
    None,
    Start,
    Overlay,
    Exit,
    Export
};

struct GapHostStartParams {
    HostViewTargets targetViews;
    GapSurfaceConfig surface;
    GapVoidParams voidParams;
};

struct GapHostRequest {
    GapHostAction action = GapHostAction::None;
    std::optional<GapHostStartParams> start;
    // 仅 Export 使用；保存当前成功分析的孔隙区域 CSV，父目录须已存在，同名文件会覆盖。
    std::optional<std::string> outputPath;
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

    // Export 在 owner thread 同步执行，返回最终保存结果，不接受 callback。
    // 分析未成功、数据版本已变化、退出中或路径无效时拒绝；保存失败不清除分析结果。
    bool SendRequest(
        GapHostRequest request,
        GapHostCallback onComplete = nullptr);
    GapHostState GetState() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
