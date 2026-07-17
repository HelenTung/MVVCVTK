#pragma once

#include "Host/Types/HostAdapterTypes.h"
#include "Host/Types/HostCommandTypes.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/Types/HostSessionTypes.h"

#include <memory>
#include <string>
#include <vector>

class VtkAppHostSession final {
public:
    explicit VtkAppHostSession(HostSessionConfig config);
    ~VtkAppHostSession();

    VtkAppHostSession(const VtkAppHostSession&) = delete;
    VtkAppHostSession& operator=(const VtkAppHostSession&) = delete;
    VtkAppHostSession(VtkAppHostSession&&) noexcept;
    VtkAppHostSession& operator=(VtkAppHostSession&&) noexcept;

    bool BuildSession();
    bool AttachTimer(const HostTimerConfig& config);
    bool AttachHotkeys(
        const HostHotkeyConfig& config,
        HostHotkeyTemplates templates);
    bool Start();

    bool SendData(
        HostDataRequest request,
        HostCompleteCallback onComplete = nullptr);
    bool SendView(HostViewRequest request);
    bool SendTool(HostToolRequest request);
    bool SendCrop(
        HostCropRequest request,
        HostCompleteCallback onComplete = nullptr);
    bool SendGap(
        HostGapRequest request,
        HostCompleteCallback onComplete = nullptr);

    const std::vector<HostRenderViewEndpoint>& GetRenderViewEndpoints();
    const HostRenderViewEndpoint* GetRenderViewEndpoint(const std::string& viewId);
    const HostRenderViewEndpoint* GetPrimaryEndpoint();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
