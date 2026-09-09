// 测试用途：场景只包含当前体数据及本功能的可视对象，不承载结果目录或发布历史。
#include "SceneNodes.h"
#include <QJsonDocument>
namespace Manual {
namespace {
QJsonObject Object(const QString& id, const QString& title, const QString& status)
{
    return {{"id", id}, {"title", title}, {"status", status}};
}
}
QJsonArray GetSceneNodes(const QString& module, const QJsonObject& s, const QJsonObject& input,
    const QStringList&)
{
    QJsonArray objects;
    if (input["available"].toBool()) objects.append(Object("input:" + input["dataRevision"].toString(),
        input["datasetId"].toString(), "体数据"));
    if (module == "Crop" && s["isActive"].toBool()) objects.append(Object("crop-tools", "裁剪工具", "编辑中"));
    if ((module == "Part" || module == "PartEdit") && s["hasCurrentParts"].toBool()) {
        for (const auto value : s["parts"].toArray()) {
            const auto part = value.toObject();
            const auto id = QString::fromUtf8(QJsonDocument(part["binding"].toObject()).toJson(QJsonDocument::Compact));
            auto node = Object("part:" + id, part["name"].toString().isEmpty() ? "零件 " + part["labelId"].toString() : part["name"].toString(),
                !s["isOverlayVisible"].toBool() || !part["visible"].toBool(true) ? "隐藏" : part["selected"].toBool() ? "高亮" : "显示");
            node["binding"] = part["binding"]; objects.append(node);
        }
    }
    if (module == "Surface" && s["hasPreview"].toBool())
        objects.append(Object("surface-preview:" + s["previewRequestId"].toString(), "等值面预览", s["isOverlayVisible"].toBool() ? "显示" : "隐藏"));
    if (module == "Surface" && s["hasResult"].toBool() && s["hasMesh"].toBool() && !s["mesh"].toString().isEmpty() && !s["mesh"].toString().endsWith(":0"))
        objects.append(Object("surface:" + s["mesh"].toString(), "表面网格", s["isOverlayVisible"].toBool() ? "显示" : "隐藏"));
    if (module == "Gap" && s["hasResult"].toBool() && s["isCurrent"].toBool())
        objects.append(Object("gap-result:" + s["resultSet"].toString(), "孔隙结果", "可视对象"));
    if (module == "Alignment" && s["isResultCurrent"].toBool())
        objects.append(Object("alignment-result:" + s["lastResult"].toString(), "对齐结果", s["isApplied"].toBool() ? "已应用" : "待应用"));
    if (module == "Wall" && s["isCurrent"].toBool() && s["hasResult"].toBool())
        objects.append(Object("wall-result:" + s["result"].toString(), "壁厚分布", s["isVisible"].toBool() ? "显示" : "隐藏"));
    auto scene = Object("root", "场景", objects.isEmpty() ? "等待输入" : QString("%1 个对象").arg(objects.size()));
    scene["children"] = objects;
    return {scene};
}
}
