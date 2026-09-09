// 测试用途：结果目录保留文档、零件目录、统计及真实发布来源，独立于场景对象。
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
namespace Manual {
QJsonArray GetCatalogNodes(const QString& module, const QJsonObject& state, const QJsonObject& input,
    const QStringList& actions, const QJsonObject& lastResult, const QJsonObject& publishedGraph = {});
}
