// 测试用途：解析零件稳定绑定和编辑参数，并生成目录与结果摘要。
#pragma once
#include "Host/PartSegmentationHostFeature.h"
#include "Support/JsonInput.h"
namespace Manual {
QJsonObject GetPartRef(const PartBindingRef& part);
PartBindingRef GetPart(const QJsonValue& value, const PartSetSnapshot& catalog);
std::vector<PartBindingRef> GetParts(const QJsonValue& value, const PartSetSnapshot& catalog);
QJsonObject GetPartResult(const PartSegmentationResult& result);
QJsonObject GetPartJson(const PartSnapshot& part);
QJsonArray GetEditingParts(const QJsonObject& context, const PartSetSnapshot& catalog);
QJsonObject GetPreviewChanges(const PartSetSnapshot& previous, const PartSetSnapshot& candidate);
QJsonObject GetCatalog(const PartSegmentationHostFeature& feature);
}
