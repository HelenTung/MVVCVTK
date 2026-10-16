#pragma once
#include "Support/ReportDocument.h"
namespace Manual {
class ResultReport;
// 复用完整真实报告验证生产配对，无需重新执行原生算法。
void CheckSavedReportComparisons(const QString& csv,bool spatialOnly=false);
void CheckNativeReportCancellation(ResultReport& view,const std::function<void()>& open,int timeoutMs);
void CheckLargeReportOperations(ResultReport& view,const ReportDocument& native,const ReportDocument& imported,const QString& csv,int timeoutMs);
}
