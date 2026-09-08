// 测试用途：为功能测试受控发布名义参考和二值掩码，保持真实数据图修订链。
#include "ReferenceDataSource.h"
#include "JsonInput.h"
#include "Data/DataPayloads.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
namespace Manual {
bool ReferenceDataSource::AttachHost(const HostFeatureContext& context)
{
    m_data = context.data;
    return static_cast<bool>(m_data);
}
bool ReferenceDataSource::DetachHost() { m_data.reset(); m_sceneCommit.reset(); m_sceneGraph = {}; m_sceneOrder.clear(); return true; }
QJsonObject ReferenceDataSource::GetPublishedGraph()
{
    if (!m_data) return {};
    const auto graph = m_data->GetDataGraph();
    if (!graph.view) return {};
    if (m_sceneCommit == graph.commitId) return m_sceneGraph;
    QJsonArray nodes;
    // 只读取不可变修订的元信息；不复制体素或网格，不按绘制帧重新查询。
    for (const auto& data : graph.view->GetDataQuery({}).data) {
        const auto ref = GetRefText(data->self);
        if (!m_sceneOrder.count(ref)) m_sceneOrder[ref] = m_sceneOrder.size()+1;
        QJsonArray parents;
        for (const auto& input : data->inputs) if (!parents.contains(GetRefText(input.source))) parents.append(GetRefText(input.source));
        nodes.append(QJsonObject{{"ref", ref}, {"type", QString::fromStdString(data->type.name)}, {"parents", parents},
            {"producer", data->provenance ? QString::fromStdString(data->provenance->producerId) : QString()},
            {"operation", data->provenance ? QString::fromStdString(data->provenance->operationId) : QString()},
            {"isVolume", data->type == DataTypes::imageGrid3D}, {"order", QString::number(m_sceneOrder.at(ref))}});
    }
    m_sceneCommit = graph.commitId;
    m_sceneGraph = {{"commitId", QString::number(graph.commitId)}, {"nodes", nodes}};
    return m_sceneGraph;
}
ReferenceInput ReferenceDataSource::LoadReference(const QString& path, DataRevisionRef source, DataRevisionRef mesh)
{
    if (!m_data) throw std::runtime_error("可信输入适配器尚未挂载");
    const auto document = LoadJson(path);
    for (const auto key : {"sourceFrameId", "scope", "unit", "evidenceKind", "provenance"})
        if (GetText(document, key).isEmpty()) throw std::invalid_argument(std::string("参考声明不能为空: ") + key);
    if (GetText(document, "evidenceKind") == "unconfigured-template")
        throw std::invalid_argument("请填写参考来源和配方，并将 evidenceKind 声明为 real-reference 或 synthetic-regression");
    if (!document["recipe"].isObject()) throw std::invalid_argument("参考文件需要 recipe 对象");
    const auto graph = m_data->GetDataGraph();
    const auto sourceData = m_data->GetData(graph, source);
    const auto meshData = m_data->GetData(graph, mesh);
    const auto payload = meshData ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(meshData->payload) : nullptr;
    if (!sourceData || !payload) throw std::invalid_argument("参考输入需要已发布的源数据与网格");
    const auto coordinateFrame = GetText(document, "coordinateFrame");
    if (coordinateFrame.toStdString() != payload->GetCoordinateFrame()) throw std::invalid_argument("参考声明的源坐标系与网格不一致");
    const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    const DataRevisionRef nominal{m_data->CreateDataEntityId(), 1};
    const DataRevisionRef scope{m_data->CreateDataEntityId(), 1};
    const std::vector<DataInputRef> inputs{{"source-volume", source}, {"source-mesh", mesh}};
    const DataProvenance provenance{"manual.reference-input", "import-reference", "1",
        (QFileInfo(path).absoluteFilePath() + "; canonical-json-sha256=" + digest + "; " + GetText(document, "provenance")).toStdString()};
    // 冻结导入文档，而不是从当前网格制造名义坐标；原始配方可追溯。
    auto nominalPayload = std::make_shared<const RecordTablePayload>(DataTypes::recordTable, "manual-reference-1",
        std::vector<RecordColumn>{{"document", std::vector<std::string>{bytes.toStdString()}},
            {"unit", std::vector<std::string>{GetText(document, "unit").toStdString()}}});
    auto scopePayload = std::make_shared<const RecordTablePayload>(DataTypes::recordTable, "manual-scope-1",
        std::vector<RecordColumn>{{"scope", std::vector<std::string>{GetText(document, "scope").toStdString()}},
            {"sourceFrameId", std::vector<std::string>{GetText(document, "sourceFrameId").toStdString()}},
            {"coordinateFrame", std::vector<std::string>{coordinateFrame.toStdString()}}});
    DataTransaction transaction;
    transaction.outputs = {{nominal.entityId, 0, DataTypes::recordTable, inputs, nominalPayload, provenance},
        {scope.entityId, 0, DataTypes::recordTable, inputs, scopePayload, provenance}};
    if (m_data->SetDataCommit(std::move(transaction)).status != DataCommitStatus::Succeeded)
        throw std::runtime_error("参考数据发布失败");
    return {source, mesh, nominal, scope, coordinateFrame, document};
}
DataRevisionRef ReferenceDataSource::CreateMask(DataRevisionRef source, const QJsonObject& params)
{
    if (!m_data) throw std::runtime_error("可信输入适配器尚未挂载");
    const auto data = m_data->GetData(m_data->GetDataGraph(), source);
    const auto image = data ? std::dynamic_pointer_cast<const ImageGrid3DPayload>(data->payload) : nullptr;
    if (!image) throw std::invalid_argument("掩码源必须是已发布的图像网格");
    const auto& geometry = image->GetGeometry();
    const auto count = GetGridVoxelCount(geometry);
    if (!count || *count > 64U * 1024U * 1024U) throw std::invalid_argument("测试掩码只支持最多64 Mi体素；请使用真实ROI");
    auto values = std::make_shared<std::vector<std::uint8_t>>(*count, GetBool(params, "defaultValue") ? 1 : 0);
    const auto inside = GetBool(params, "boxValue") ? 1 : 0;
    if (!params["indexBoxes"].isArray()) throw std::invalid_argument("indexBoxes 必须为含端点范围数组");
    for (const auto boxValue : params["indexBoxes"].toArray()) {
        const auto box = GetArray<int, 6>(boxValue);
        for (int axis = 0; axis < 3; ++axis)
            if (box[axis * 2] < geometry.extent[axis * 2] || box[axis * 2 + 1] > geometry.extent[axis * 2 + 1]
                || box[axis * 2] > box[axis * 2 + 1]) throw std::invalid_argument("掩码范围超出源网格");
        for (int z = box[4]; z <= box[5]; ++z) for (int y = box[2]; y <= box[3]; ++y) for (int x = box[0]; x <= box[1]; ++x) {
            const auto offset = static_cast<std::size_t>(x - geometry.extent[0]) + static_cast<std::size_t>(geometry.dimensions[0])
                * (static_cast<std::size_t>(y - geometry.extent[2]) + static_cast<std::size_t>(geometry.dimensions[1]) * (z - geometry.extent[4]));
            (*values)[offset] = static_cast<std::uint8_t>(inside);
        }
    }
    const DataRevisionRef output{m_data->CreateDataEntityId(), 1};
    const auto payload = std::make_shared<const BinaryMask3DPayload>(geometry, values);
    DataTransaction transaction;
    transaction.outputs = {{output.entityId, 0, DataTypes::binaryMask3D, {{"source-volume", source}}, payload,
        DataProvenance{"manual.reference-input", "parameterized-mask", "1", GetJsonText(params).toStdString()}}};
    if (m_data->SetDataCommit(std::move(transaction)).status != DataCommitStatus::Succeeded) throw std::runtime_error("掩码发布失败");
    return output;
}
}
