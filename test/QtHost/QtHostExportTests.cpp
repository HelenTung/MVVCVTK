#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

int GetExportFailCount()
{
    VtkAppHostSession session(VtkAppHostSession::Config{});
    return GetCaseResult(
        !session.ExportData({}),
        "Export flag rejection") ? 0 : 1;
}
