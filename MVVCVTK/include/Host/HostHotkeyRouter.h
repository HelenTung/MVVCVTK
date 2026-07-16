#pragma once

#include "Host/HostSessionTypes.h"

#include <memory>

class HostCommandRouter;
class HostFeatureBindings;
class HostRenderViewSet;

class HostHotkeyRouter final {
public:
    HostHotkeyRouter(
        const HostRenderViewSet& renderViews,
        std::weak_ptr<HostFeatureBindings> featureBindings,
        std::weak_ptr<HostCommandRouter> commandRouter);
    ~HostHotkeyRouter();

    bool AttachHotkeys(
        const HostContextInput& renderContextInput,
        const HostDataExportConfig& dataExportConfig,
        const HostCommandInputConfig& commandInput,
        const HostHotkeyBindings& hotkeys);
    bool ClearHotkeys();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
