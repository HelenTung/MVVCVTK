// 测试用途：为功能测试界面提供中文业务名称、操作和参数说明，并格式化业务流程日志。
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <utility>

namespace Manual {
QString GetActionDescription(const QString& module, const QString& action);
QString GetModuleText(const QString& module);
QString GetActionText(const QString& module, const QString& action);
QString GetParameterSectionText(const QString& module, const QString& action);
QString GetParameterText(const QString& key);
QString GetParameterHelp(const QString& key);
using ParameterChoices = QVector<std::pair<QString, QString>>;
ParameterChoices GetParameterChoices(const QString& module, const QString& key);
QStringList GetBoundParameters(const QString& module, const QString& action);
QString GetFlowText(const QJsonObject& record);
}
