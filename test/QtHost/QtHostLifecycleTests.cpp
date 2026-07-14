#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

#include <memory>

int GetLifecycleFailCount()
{
    auto session =
        std::make_unique<VtkAppHostSession>(VtkAppHostSession::Config{});
    session->BuildSession();
    session.reset();
    return GetCaseResult(!session, "Session destruction") ? 0 : 1;
}
