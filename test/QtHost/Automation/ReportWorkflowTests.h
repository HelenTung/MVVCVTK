#pragma once
#include <QJsonObject>
#include <QString>
namespace Manual {
class TestWindow;
void CheckReportWorkflow();
void CheckBusinessReport(TestWindow& window,const QJsonObject& record,bool checkExport = true,const QString& previewPath = {});
}
