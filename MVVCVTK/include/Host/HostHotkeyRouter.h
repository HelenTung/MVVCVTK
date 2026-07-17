#pragma once

#include "Host/Types/HostAdapterTypes.h"

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
        const HostHotkeyConfig& config,
        HostHotkeyTemplates templates);
    bool ClearHotkeys();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
