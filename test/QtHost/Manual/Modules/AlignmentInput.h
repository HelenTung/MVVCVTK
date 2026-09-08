// 测试用途：解析计量对齐参考与方案，校验几何约束并导出参数模板和结果摘要。
#pragma once
#include "Support/JsonInput.h"
#include "Support/ReferenceDataSource.h"
#include "Host/MetrologyAlignmentHostTypes.h"
namespace Manual {
AlignmentRecipe GetRecipe(const QJsonObject& value, const DataRevisionRef& mesh, const DataRevisionRef& nominal);
QJsonObject GetRecipeJson(const AlignmentRecipe& value);
AlignmentInput GetAlignmentInput(const ReferenceInput& reference);
QJsonObject GetInputJson(const AlignmentInput& input);
QJsonObject GetReferenceTemplate(AlignmentMethod method);
QJsonObject GetAlignmentResult(const AlignmentResult& result);
}
