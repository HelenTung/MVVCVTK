#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

int GetViewFailCount()
{
    VtkAppHostSession session(HostSessionConfig{});
    HostViewSetRequest value;
    value.targetView.viewId = "missing-view";
    return GetCaseResult(
        !session.SendView({ HostViewAction::Set, value }),
        "View missing target rejection") ? 0 : 1;
}
