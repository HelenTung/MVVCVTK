#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

int GetExportFailCount()
{
    VtkAppHostSession session(HostSessionConfig{});
    return GetCaseResult(
        !session.SendData({ HostDataAction::ExportVolume,
            HostVolumeExportRequest{} }),
        "Export empty path rejection") ? 0 : 1;
}
