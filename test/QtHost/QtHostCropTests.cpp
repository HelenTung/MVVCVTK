#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

int GetCropFailCount()
{
    VtkAppHostSession session(HostSessionConfig{});
    int failureCount = 0;
    const HostCropTargetRequest target;
    failureCount += GetCaseResult(
        !session.SendCrop({ HostCropAction::Start, target }),
        "Crop start missing reference rejection") ? 0 : 1;
    failureCount += GetCaseResult(
        !session.SendCrop({ HostCropAction::Preview, target }),
        "Crop preview payload mismatch rejection") ? 0 : 1;
    failureCount += GetCaseResult(
        !session.SendCrop({ HostCropAction::Exit, std::monostate{} }),
        "Crop inactive exit rejection") ? 0 : 1;
    return failureCount;
}
