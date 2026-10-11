#include "InputRoiSource.h"
#include "JsonInput.h"
#include "Data/DataPayloads.h"
#include "Host/TrustedDataPort.h"
#include <algorithm>
namespace Manual {
bool InputRoiSource::AttachHost(const HostFeatureContext& context)
{
    m_data = context.data;
    return static_cast<bool>(m_data);
}
bool InputRoiSource::DetachHost()
{
    if (!m_data) return true;
    DataTransaction retirement;
    for (const auto& scope : m_scopes) {
        const auto status = m_data->GetDataLifetime(scope.scopeId).status;
        if (status == DataLifetimeStatus::Published) retirement.retireScopes.push_back(scope);
        else if (status != DataLifetimeStatus::Releasing && status != DataLifetimeStatus::Released) return false;
    }
    // 消费者仍引用范围时保留账本，Host 在下游清理后重试。
    if (!retirement.retireScopes.empty()
        && m_data->SetDataCommit(std::move(retirement)).status != DataCommitStatus::Succeeded) return false;
    for (auto scope = m_scopes.begin(); scope != m_scopes.end();)
        if (m_data->SetDataRelease(scope->scopeId).status == DataLifetimeStatus::Released) scope = m_scopes.erase(scope);
        else ++scope;
    if (!m_scopes.empty()) return false;
    m_data.reset();
    return true;
}
std::optional<DataRevisionRef> InputRoiSource::Create(DataRevisionRef source,
    const QJsonValue& extentValue, const QJsonValue& maskValue, const char* name, bool isPhysicalBounds)
{
    const bool hasExtent = !extentValue.isNull() && !extentValue.isUndefined();
    const bool hasMask = !maskValue.isNull() && !maskValue.isUndefined();
    if (!hasExtent && !hasMask) return std::nullopt;
    if (!m_data) throw std::invalid_argument("测试范围发布器未挂载");
    const auto sourceData = m_data->GetData(m_data->GetDataGraph(), source);
    const auto image = sourceData ? std::dynamic_pointer_cast<const ImageGrid3DPayload>(sourceData->payload) : nullptr;
    if (!image) throw std::invalid_argument("测试范围源图像不可用");
    const auto& grid = image->GetGeometry();
    RoiDefinition definition; definition.source = source;
    if (hasExtent && isPhysicalBounds) {
        const auto bounds = GetArray<double, 6>(extentValue);
        RoiNode node;
        for (int axis = 0; axis < 3; ++axis) {
            if (bounds[axis*2] >= bounds[axis*2+1]) throw std::invalid_argument("物理范围必须有正长度");
            node.primitive.localToSource[axis*4+axis] = (bounds[axis*2+1]-bounds[axis*2])*0.5;
            node.primitive.localToSource[axis*4+3] = (bounds[axis*2+1]+bounds[axis*2])*0.5;
        }
        definition.nodes.push_back(node);
    } else if (hasExtent) {
        const auto extent = GetArray<int, 6>(extentValue);
        RoiNode node;
        for (int axis = 0; axis < 3; ++axis)
            if (extent[axis*2] < grid.extent[axis*2] || extent[axis*2+1] > grid.extent[axis*2+1]
                || extent[axis*2] > extent[axis*2+1]) throw std::invalid_argument("编辑范围超出源网格");
        for (int row = 0; row < 3; ++row) {
            node.primitive.localToSource[row*4+3] = grid.origin[row];
            for (int axis = 0; axis < 3; ++axis) {
                const double center = (static_cast<double>(extent[axis*2])+extent[axis*2+1])*0.5;
                const double half = std::max(0.25,(static_cast<double>(extent[axis*2+1])-extent[axis*2])*0.5);
                node.primitive.localToSource[row*4+axis] = grid.direction[row*3+axis]*grid.spacing[axis]*half;
                node.primitive.localToSource[row*4+3] += grid.direction[row*3+axis]*grid.spacing[axis]*center;
            }
        }
        definition.nodes.push_back(node);
    }
    std::vector<DataInputRef> inputs{{"source-data", source}};
    if (hasMask) {
        RoiNode node; node.primitive.shape = RoiShape::MaskReference; node.primitive.mask = GetRef(maskValue);
        inputs.push_back({"mask-0", *node.primitive.mask});
        definition.nodes.push_back(node);
    }
    if (hasExtent && hasMask) {
        RoiNode intersection; intersection.kind = RoiNodeKind::Intersection; intersection.left = 0; intersection.right = 1;
        definition.nodes.push_back(intersection);
    }
    const DataRevisionRef roi{m_data->CreateDataEntityId(), 1};
    const DataLifetimeRetirement scope{m_data->CreateDataEntityId(), DataLifetimeStatus::Published, {roi}, true};
    DataRevisionDraft draft{roi.entityId, 0, DataTypes::roiGeometry, std::move(inputs),
        std::make_shared<const RoiGeometryPayload>(std::move(definition)),
        DataProvenance{"manual.roi-input", "prepare-input-roi", "1", name}};
    draft.lifetimeScope = scope.scopeId;
    draft.lifetimePolicy = DataLifetimePolicy::OwnerRetirement;
    // 发布前保留所需容量，避免成功提交后因账本分配失败丢失所有权。
    m_scopes.reserve(m_scopes.size()+1);
    auto batch = m_data->StartDataChanges();
    if (!batch) throw std::runtime_error("测试输入 ROI 事务不可用");
    DataTransaction transaction; transaction.outputs.push_back(std::move(draft));
    const auto result = m_data->SetDataCommit(std::move(transaction));
    if (result.status != DataCommitStatus::Succeeded) throw std::invalid_argument("测试输入 ROI 发布失败: " + result.message);
    m_scopes.push_back(scope);
    return roi;
}
}
