#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

int GetGapFailCount()
{
    VtkAppHostSession session(HostSessionConfig{});
    int failureCount = 0;
    failureCount += GetCaseResult(
        !session.SendGap({ HostGapAction::Start, HostGapStartRequest{} }),
        "Gap missing target rejection") ? 0 : 1;
    failureCount += GetCaseResult(
        !session.SendGap({ HostGapAction::Overlay, std::monostate{} }),
        "Gap inactive switch rejection") ? 0 : 1;
    failureCount += GetCaseResult(
        !session.SendGap({ HostGapAction::Exit, std::monostate{} }),
        "Gap inactive exit rejection") ? 0 : 1;
    return failureCount;
}
