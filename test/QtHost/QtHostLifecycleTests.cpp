#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

#include <vtkRenderWindowInteractor.h>
#include <vtkWeakPointer.h>

#include <memory>
#include <utility>

int GetLifecycleFailCount()
{
    HostRenderViewConfig view;
    view.id = "lifecycle";
    view.role = HostRenderViewRole::Primary3D;

    VtkAppHostSession::Config config;
    config.isInitialRenderEnabled = false;
    config.renderViews.push_back(std::move(view));
    auto session = std::make_unique<VtkAppHostSession>(std::move(config));
    session->BuildSession();
    const auto* endpoint = session->GetPrimaryEndpoint();
    vtkWeakPointer<vtkRenderWindowInteractor> interactor =
        endpoint ? endpoint->interactor : nullptr;
    const bool isBuilt = endpoint && interactor;
    session.reset();
    return GetCaseResult(
        isBuilt && !session && !interactor,
        "Session-owned interactor destruction") ? 0 : 1;
}
