#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

int GetViewFailCount()
{
    VtkAppHostSession session(VtkAppHostSession::Config{});
    HostViewConfig request;
    request.viewId = "missing-view";
    return GetCaseResult(
        !session.SetViewConfig(request),
        "View empty edit rejection") ? 0 : 1;
}
