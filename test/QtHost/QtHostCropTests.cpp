#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

int GetCropFailCount()
{
    int failureCount{0};
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        failureCount += GetCaseResult(
            !session.StartCrop({}),
            "Crop start missing reference rejection") ? 0 : 1;
    }
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        failureCount += GetCaseResult(
            !session.SwitchCropBox({}),
            "Crop box missing reference rejection") ? 0 : 1;
    }
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        failureCount += GetCaseResult(
            !session.SwitchCropPlane({}),
            "Crop plane missing reference rejection") ? 0 : 1;
    }
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        failureCount += GetCaseResult(
            !session.SwitchCropView({}, HostCropPreviewMode::KeepInside),
            "Crop preview missing reference rejection") ? 0 : 1;
    }
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        failureCount += GetCaseResult(
            !session.SendCrop({}),
            "Crop submit missing reference rejection") ? 0 : 1;
    }
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        failureCount += GetCaseResult(
            !session.ExitCrop(),
            "Crop inactive exit rejection") ? 0 : 1;
    }
    return failureCount;
}
