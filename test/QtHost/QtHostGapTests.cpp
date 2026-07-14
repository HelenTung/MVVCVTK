#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

int GetGapFailCount()
{
    int failureCount{0};
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        failureCount += GetCaseResult(
            !session.StartGapView({}),
            "Gap missing target rejection") ? 0 : 1;
    }
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        failureCount += GetCaseResult(
            !session.SwitchGapLayer(),
            "Gap inactive switch rejection") ? 0 : 1;
    }
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        failureCount += GetCaseResult(
            !session.ExitGapView(),
            "Gap inactive exit rejection") ? 0 : 1;
    }
    return failureCount;
}
