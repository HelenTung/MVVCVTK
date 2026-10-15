#pragma once
#include <QJsonObject>
namespace Manual {
class TestWindow;
void CheckReportWorkflow();
void CheckBusinessReport(TestWindow& window,const QJsonObject& record);
}
