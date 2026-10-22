#pragma once
#include <QJsonObject>
#include <QString>
namespace Manual {
class TestWindow;
void CheckReportWorkflow();
void CheckGapBoundaryReports(const QString& outputDirectory,const QString& realInput);
void CheckReportRepairs();
void CheckReportStop(TestWindow& window);
void CheckBusinessReport(TestWindow& window,const QJsonObject& record,bool checkExport = true,const QString& previewPath = {},int timeoutMs = 0,bool checkLarge = false);
}
