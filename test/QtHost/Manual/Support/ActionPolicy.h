// 测试用途：声明动作前置条件检查，统一真实按钮与自动化入口的接纳规则。
#pragma once
#include <QString>
#include <QJsonObject>
namespace Manual { QString GetActionRequirement(const QString&, const QString&, const QJsonObject&, bool); }
