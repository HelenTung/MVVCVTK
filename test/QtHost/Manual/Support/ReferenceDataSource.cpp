// 测试用途：为功能测试受控发布名义参考和二值掩码，保持真实数据图修订链。
#include "ReferenceDataSource.h"
#include "Host/VtkAppHostSession.h"
#include "App/Services/FeatureViewService.h"
#include <algorithm>
#include <type_traits>
#include "JsonInput.h"
#include "Data/DataPayloads.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include "App/Services/FeatureViewService.h"
namespace Manual {
std::optional<DataRevisionRef> CreateInputRoi(VtkAppHostSession& session, DataRevisionRef source,
    const QJsonValue& extentValue, const QJsonValue& maskValue, const char* name, bool isPhysicalBounds)
{
    const bool hasExtent = !extentValue.isNull() && !extentValue.isUndefined();
    const bool hasMask = !maskValue.isNull() && !maskValue.isUndefined();
    if (!hasExtent && !hasMask) return std::nullopt;
    RoiRequest request;
    request.definition.source = source;
    request.metadata.name = name;
    if (hasExtent && isPhysicalBounds) {
        const auto bounds = GetArray<double, 6>(extentValue);
        RoiNode node;
        for (int axis = 0; axis < 3; ++axis) {
            if (bounds[axis*2] >= bounds[axis*2+1]) throw std::invalid_argument("物理范围必须有正长度");
            node.primitive.localToSource[axis*4+axis] = (bounds[axis*2+1]-bounds[axis*2])*0.5;
            node.primitive.localToSource[axis*4+3] = (bounds[axis*2+1]+bounds[axis*2])*0.5;
        }
        request.definition.nodes.push_back(node);
    } else if (hasExtent) {
        const auto grid = session.GetImageDescriptor();
        if (!grid || grid->dataRevision != source) throw std::invalid_argument("索引范围需要对应源图像几何");
        const auto extent = GetArray<int, 6>(extentValue);
        RoiNode node;
        for (int axis = 0; axis < 3; ++axis)
            if (extent[axis*2] < grid->extent[axis*2] || extent[axis*2+1] > grid->extent[axis*2+1]
                || extent[axis*2] > extent[axis*2+1]) throw std::invalid_argument("编辑范围超出源网格");
        for (int row = 0; row < 3; ++row) {
            node.primitive.localToSource[row*4+3] = grid->origin[row];
            for (int axis = 0; axis < 3; ++axis) {
                const double center = (static_cast<double>(extent[axis*2])+extent[axis*2+1])*0.5;
                // 单体素轴使用小于半间距的厚度，保持含端点的体素中心集合。
                const double half = std::max(0.25,(static_cast<double>(extent[axis*2+1])-extent[axis*2])*0.5);
                node.primitive.localToSource[row*4+axis] = grid->direction[row*3+axis]*grid->spacing[axis]*half;
                node.primitive.localToSource[row*4+3] += grid->direction[row*3+axis]*grid->spacing[axis]*center;
            }
        }
        request.definition.nodes.push_back(node);
    }
    if (hasMask) {
        RoiNode node; node.primitive.shape = RoiShape::MaskReference; node.primitive.mask = GetRef(maskValue);
        request.definition.nodes.push_back(node);
    }
    if (hasExtent && hasMask) {
        RoiNode intersection; intersection.kind = RoiNodeKind::Intersection; intersection.left = 0; intersection.right = 1;
        request.definition.nodes.push_back(intersection);
    }
    const auto catalog = session.GetRoiDescriptors(true);
    request.expectedCatalogRevision = catalog.empty() ? 0 : catalog.front().catalogRevision;
    const auto result = session.SetRoi(request);
    if (result.error != RoiError::None || !result.roi) throw std::invalid_argument("测试输入 ROI 发布失败: " + result.message);
    return result.roi->revision;
}

bool ReferenceDataSource::AttachHost(const HostFeatureContext& context)
{
    m_data = context.data;
    m_views = context.views;
    return static_cast<bool>(m_data);
}
bool ReferenceDataSource::DetachHost()
{
    // 临时输入派生的测试参考/掩码须由本生产者退休；仍有消费者时保留状态以便重试。
    if (m_data && !m_resultScopes.Clear(*m_data)) return false;
    m_data.reset(); m_views.reset(); m_sceneCommit.reset(); m_sceneGraph = {}; m_sceneOrder.clear();
    return true;
}
QJsonObject ReferenceDataSource::GetViewTransforms()
{
    QJsonArray values;
    if (m_views) for (const auto& view:m_views->GetViews({{},{
        HostRenderViewRole::Primary3D,HostRenderViewRole::Composite3D,
        HostRenderViewRole::TopDownSlice,HostRenderViewRole::FrontBackSlice,HostRenderViewRole::LeftRightSlice}})) {
        const auto port=m_views->GetFeaturePort(view.id);
        const auto matrix=port ? port->GetModelToWorld() : std::optional<std::array<double,16>>{};
        if (matrix) values.append(QJsonObject{{"view",QString::fromStdString(view.id)},
            {"modelToWorld",GetValues(*matrix)}});
    }
    return {{"views",values}};
}
QJsonObject ReferenceDataSource::GetResultEvidence()
{
    QJsonArray outputs;
    if (!m_data) return {};
    const auto graph=m_data->GetDataGraph();
    for(const auto& data:graph.view->GetDataQuery({}).data) {
        if(!data->payload || !data->provenance)continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const auto bytes=[&](const void* values,std::size_t size) {
            auto* p=static_cast<const char*>(values);
            while(size) {const auto chunk=std::min<std::size_t>(size,8U*1024U*1024U);hash.addData(p,static_cast<int>(chunk));p+=chunk;size-=chunk;}
        };
        const auto vector=[&](const auto& values) {
            const std::uint64_t count=values.size();bytes(&count,sizeof(count));
            using Value=typename std::decay_t<decltype(values)>::value_type;
            if constexpr(std::is_same_v<Value,std::string>) {
                for(const auto& value:values){const std::uint64_t length=value.size();bytes(&length,sizeof(length));bytes(value.data(),value.size());}
            } else bytes(values.data(),values.size()*sizeof(Value));
        };
        const auto geometry=[&](const GridGeometry3D& g) {
            bytes(g.extent.data(),sizeof(g.extent));bytes(g.spacing.data(),sizeof(g.spacing));
            bytes(g.origin.data(),sizeof(g.origin));bytes(g.direction.data(),sizeof(g.direction));
        };
        bool supported=true;
        QString schema;
        if(const auto* labels=dynamic_cast<const LabelMap3DPayload*>(data->payload.get())) {
            geometry(labels->GetGeometry());std::visit([&](const auto& values){vector(*values);},labels->GetValues());
        } else if(const auto* mesh=dynamic_cast<const SurfaceMeshPayload*>(data->payload.get())) {
            vector(mesh->GetVertices());vector(mesh->GetTriangles());
            for(const auto* attributes:{&mesh->GetPointAttributes(),&mesh->GetCellAttributes()})
                for(const auto& attribute:*attributes){bytes(attribute.name.data(),attribute.name.size());vector(attribute.values);}
        } else if(const auto* table=dynamic_cast<const RecordTablePayload*>(data->payload.get())) {
            schema=QString::fromStdString(table->GetSchemaName());
            for(const auto& column:table->GetColumns()) {
                const auto name=QString::fromStdString(column.name).toLower();
                // 计时/工作集是执行诊断，不作为数值精度对拍内容。
                if(name.contains("elapsed")||name.contains("duration")||name.contains("working-bytes"))continue;
                bytes(column.name.data(),column.name.size());
                std::visit(vector,column.values);
            }
        } else if(const auto* matrix=dynamic_cast<const Transform3DPayload*>(data->payload.get())) {
            bytes(matrix->GetSourceToTarget().data(),sizeof(matrix->GetSourceToTarget()));
        } else if(const auto* image=dynamic_cast<const ImageGrid3DPayload*>(data->payload.get())) {
            geometry(image->GetGeometry());
            const auto& values = *image->GetValues();
            const std::uint64_t count = values.size();
            bytes(&count, sizeof(count));
            bytes(values.data(), values.size());
        } else supported=false;
        if(supported) outputs.append(QJsonObject{{"producer",QString::fromStdString(data->provenance->producerId)},
            {"operation",QString::fromStdString(data->provenance->operationId)},
            {"type",QString::fromStdString(data->type.name)},{"schema",schema},
            {"sha256",QString::fromLatin1(hash.result().toHex())}});
    }
    return {{"outputs",outputs}};
}
QJsonObject ReferenceDataSource::ReadTransform()
{
    const auto view = m_views ? m_views->GetFeaturePort("primary-3d") : nullptr;
    const auto matrix = view ? view->GetModelToWorld() : std::nullopt;
    if (!matrix) throw std::runtime_error("model transform unavailable");
    return {{"modelToWorld", GetValues(*matrix)}};
}
QJsonObject ReferenceDataSource::ReadRoi(DataRevisionRef ref, DataRevisionRef source)
{
    const auto graph = m_data->GetDataGraph();
    const auto read = m_data->GetRoi(graph, ref, source);
    const auto data = m_data->GetData(graph, source);
    const auto image = data ? std::dynamic_pointer_cast<const ImageGrid3DPayload>(data->payload) : nullptr;
    if (!read.roi || !image) throw std::runtime_error("ROI/source unavailable");
    const auto& grid = image->GetGeometry();
    RoiMaskRequest request; request.region.offset = {0,0,0};
    for (int a=0;a<3;++a) request.region.size[a]=grid.dimensions[a];
    request.maxBytes=8U*1024U*1024U;
    std::uint64_t selected=0,total=0; QCryptographicHash hash(QCryptographicHash::Sha256);
    for (;;) {
        const auto chunk=read.roi->GetMaskChunk(request);
        if (chunk.error!=RoiError::None) throw std::runtime_error("ROI read failed");
        for (auto value:chunk.values) selected += value!=0;
        total+=chunk.values.size();
        hash.addData(reinterpret_cast<const char*>(chunk.values.data()),static_cast<int>(chunk.values.size()));
        if(chunk.isComplete)break;
        request.voxelOffset=chunk.nextOffset;
    }
    return {{"roi",GetRefText(ref)},{"source",GetRefText(source)}, {"bounds",GetValues(read.roi->GetBounds())},
        {"voxelCount",QString::number(selected)},{"gridCount",QString::number(total)},
        {"volumeMM3",selected*grid.spacing[0]*grid.spacing[1]*grid.spacing[2]},
        {"binaryMaskSha256",QString::fromLatin1(hash.result().toHex())}};
}
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
    if (m_resultScopes.Commit(*m_data, std::move(transaction)).status != DataCommitStatus::Succeeded)
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
    if (m_resultScopes.Commit(*m_data, std::move(transaction)).status != DataCommitStatus::Succeeded) throw std::runtime_error("掩码发布失败");
    return output;
}
}
