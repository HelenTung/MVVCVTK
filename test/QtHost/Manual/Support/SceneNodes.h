// 测试用途：仅投影当前输入和可视对象；目录、统计、历史归 CatalogNodes 管理。
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
namespace Manual {
QJsonArray GetSceneNodes(const QString& module, const QJsonObject& state, const QJsonObject& input,
    const QStringList& actions);
}
