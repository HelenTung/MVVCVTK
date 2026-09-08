// 测试用途：将公共业务快照转换为可选中对象的场景树，保留精确版本和操作参数。
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
namespace Manual {
QJsonArray GetSceneNodes(const QString& module, const QJsonObject& state, const QJsonObject& input,
    const QStringList& actions, const QJsonObject& lastResult, const QJsonObject& publishedGraph = {});
}
