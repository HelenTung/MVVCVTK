#pragma once
#include "Support/ReportDocument.h"
namespace Manual {
class ResultReport;
void CheckNativeReportCancellation(ResultReport& view,const std::function<void()>& open,int timeoutMs);
void CheckLargeReportOperations(ResultReport& view,const ReportDocument& native,const ReportDocument& imported,const QString& csv,int timeoutMs);
}
