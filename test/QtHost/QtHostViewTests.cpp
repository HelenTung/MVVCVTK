#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

#include <array>
#include <limits>
#include <utility>
#include <vector>

int GetViewFailCount()
{
    HostSessionConfig config;
    HostRenderViewConfig view;
    view.id = "view";
    view.role = HostRenderViewRole::Primary3D;
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession session(std::move(config));

    int failureCount = 0;
    HostViewSetRequest value;
    value.targetView.viewId = "missing-view";
    failureCount += GetCaseResult(
        !session.SendView({ HostViewAction::Set, value }),
        "View missing target rejection") ? 0 : 1;

    value.targetView.viewId = "view";
    value.mode = HostRenderMode::IsoSurface;
    value.opacity = 0.7;
    value.spacing = std::array<double, 3>{ 1.0, 0.0, 1.0 };
    failureCount += GetCaseResult(
        !session.SendView({ HostViewAction::Set, value }),
        "View late invalid spacing atomic rejection") ? 0 : 1;

    value = HostViewSetRequest{};
    value.targetView.viewId = "view";
    value.transferNodes = std::vector<HostTransferNode>{};
    failureCount += GetCaseResult(
        !session.SendView({ HostViewAction::Set, value }),
        "View explicit empty transfer rejection") ? 0 : 1;

    value = HostViewSetRequest{};
    value.targetView.viewId = "view";
    value.background = HostBackgroundColor{
        std::numeric_limits<double>::infinity(), 0.0, 0.0 };
    failureCount += GetCaseResult(
        !session.SendView({ HostViewAction::Set, value }),
        "View non-finite background rejection") ? 0 : 1;

    return failureCount;
}
