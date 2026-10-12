// 测试用途：协调功能测试中的计算占用、输入选择和交互结束，避免互斥操作并发。
#include "TestWorkflow.h"
#include "Support/JsonInput.h"
#include "Data/DataPayloads.h"
#include <stdexcept>
#include <algorithm>
namespace Manual {
bool TestWorkflow::StartOperation(std::uint64_t id, TestPolicy policy, bool exitTools, const QString& module)
{
    if (m_isClosing) return false;
    const bool isRead = policy == TestPolicy::Read || policy == TestPolicy::View || policy == TestPolicy::Stop;
    if (m_busyId && !isRead) return false;
    if (exitTools) for (const auto& exit : m_exits)
        if ((exit.first != module || policy == TestPolicy::Input) && !exit.second()) return false;
    if (policy == TestPolicy::Compute || policy == TestPolicy::Input) m_busyId = id;
    return true;
}
void TestWorkflow::SetComplete(std::uint64_t id)
{
    if (m_busyId == id) m_busyId = 0;
}
QJsonObject TestWorkflow::GetPublishedGraph(VtkAppHostSession& session)
{
    const auto catalog = session.GetDataCatalog();
    if (!catalog) return {};
    QJsonArray nodes;
    for (const auto& data : catalog->data) {
        const auto ref = GetRefText(data.revision);
        if (!m_catalogOrder.count(ref)) m_catalogOrder[ref] = m_catalogOrder.size() + 1;
        QJsonArray parents;
        for (const auto& input : data.inputs)
            if (!parents.contains(GetRefText(input.source))) parents.append(GetRefText(input.source));
        nodes.append(QJsonObject{{"ref", ref}, {"type", QString::fromStdString(data.type.name)}, {"parents", parents},
            {"producer", data.provenance ? QString::fromStdString(data.provenance->producerId) : QString()},
            {"operation", data.provenance ? QString::fromStdString(data.provenance->operationId) : QString()},
            {"isRetained", getResultRetained && getResultRetained(data.revision)},
            {"isVolume", data.type == DataTypes::imageGrid3D}, {"order", QString::number(m_catalogOrder.at(ref))}});
    }
    return {{"commitId", QString::number(catalog->commitId)}, {"nodes", nodes}};
}
#if defined(MANUAL_WALL)
ThicknessStartRequest TestWorkflow::BuildThicknessRequest(VtkAppHostSession& session,
    const ThicknessInputSelection& selection, ThicknessParams params) const
{
    const auto current = session.GetImageDescriptor();
    const auto source = selection.source ? session.GetImageDescriptor(*selection.source) : current;
    if (!source) throw std::invalid_argument("所选体数据不可用");
    const auto surface = GetMeasurementSurface();
    ThicknessInput input;
    input.source = source->dataRevision;
    input.labels = selection.labels.value_or(getPartLabels ? getPartLabels() : DataRevisionRef{});
    if (selection.mesh) input.mesh = *selection.mesh;
    else {
        if (!surface || surface->source != input.source)
            throw std::invalid_argument("请先生成同一输入的有效测量表面");
        input.mesh = surface->mesh;
    }
    if (!GetDataRevisionRefValid(input.labels) || !GetDataRevisionRefValid(input.mesh))
        throw std::invalid_argument("需要有效标签图和正式测量表面修订");
    const auto labels=session.GetDataCatalog();
    bool matched=false;
    if(labels)for(const auto& value:labels->data)if(value.revision==input.labels && value.type==DataTypes::labelMap3D)
        matched=std::any_of(value.inputs.begin(),value.inputs.end(),[&](const auto& dependency){return dependency.role=="source-volume" && dependency.source==input.source;});
    if(!matched)throw std::invalid_argument("所选材料标签与测量体数据的精确源修订不匹配");
    if (!params.materialThreshold && surface && surface->source == input.source
        && surface->mesh == input.mesh) params.materialThreshold = surface->threshold;
    if (!params.materialThreshold) throw std::invalid_argument("请显式提供原始灰度材料阈值");
    input.materialLabels = selection.materialLabels;
    input.unit = selection.unit;
    return {std::move(input), std::move(params), {}, {}};
}
#endif
}
