// 测试宿主进程的实测资源；操作预算和设备显存不参与这些计数。
#pragma once
#include <QJsonObject>
namespace Manual {
QJsonObject GetProcessMeasurements();
}
