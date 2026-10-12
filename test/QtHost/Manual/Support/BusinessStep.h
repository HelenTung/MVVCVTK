// 工作台阶段是既有 Host/Feature 状态的只读投影，不保存第二套业务状态。
#pragma once
#include <QString>
#include <QStringList>
#include <QJsonObject>
namespace Manual {
struct BusinessStep {
    QString stage, next, action, title;
    QStringList secondary;
    bool usesOutput = false;
};
BusinessStep GetBusinessStep(const QString& module, const QJsonObject& state,
    const QJsonObject& input, const QString& parameterAction);
}
