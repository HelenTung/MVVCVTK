#pragma once

#include "Data/DataGraphStore.h"
#include "Data/VtkDataBridge.h"
#include "Host/TrustedDataPort.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// 仓内 Feature 测试使用真实 DataGraphStore 与 VTK bridge；不再仿造第二套 current/pending 状态机。
inline DataRevisionRef GetTestDataRef(
    const std::uint64_t identity, const DataGeneration generation = 1)
{
    DataEntityId entity;
    for (std::size_t index = 0; index < 8; ++index) {
        entity.bytes[index] = static_cast<std::uint8_t>(identity >> (index * 8));
    }
    return { entity, generation };
}

class TestDataPort : public TrustedDataPort {
public:
    DataGraphSnapshot GetDataGraph() const override
    {
        return m_store.GetDataGraph();
    }

    DataSnapshot GetData(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const override
    {
        return graph.view ? graph.view->GetData(ref) : DataSnapshot{};
    }

    DataQueryResult GetDataQuery(
        const DataGraphSnapshot& graph,
        const DataQuery& query) const override
    {
        return graph.view
            ? graph.view->GetDataQuery(query) : DataQueryResult{};
    }

    std::optional<DataBinding> GetDataBinding(
        const DataGraphSnapshot& graph,
        const std::string_view name) const override
    {
        return graph.view
            ? graph.view->GetDataBinding(name)
            : std::optional<DataBinding>{};
    }

    ProjectDataSnapshot GetProjectData() const override
    {
        return m_store.GetProjectData();
    }

    DataRelationStatus GetDataRelation(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& data,
        const std::string_view inputRole,
        const std::string_view binding) const override
    {
        return graph.view
            ? graph.view->GetDataRelation(data, inputRole, binding)
            : DataRelationStatus::Unknown;
    }

    VtkImageGridSnapshot GetImageGrid(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const override
    {
        auto view = m_bridge.GetImageGrid(GetData(graph, ref));
        if (!view) return {};
        return std::make_shared<const VtkImageGridView>(VtkImageGridView{
            graph, {}, DataSnapshot(view, view->data.get()), view->image, view->validityMask });
    }

    VtkImageGridSnapshot GetPrimaryImage() const override
    {
        const auto graph = GetDataGraph();
        const auto binding = GetDataBinding(graph, primaryVolumeBinding);
        if (!binding || !binding->target) return {};
        auto view = m_bridge.GetImageGrid(GetData(graph, *binding->target));
        if (!view) return {};
        return std::make_shared<const VtkImageGridView>(VtkImageGridView{
            graph, binding, DataSnapshot(view, view->data.get()), view->image, view->validityMask });
    }

    VtkLabelMapSnapshot GetLabelMap(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const override
    {
        return m_bridge.GetLabelMap(GetData(graph, ref));
    }

    VtkSurfaceMeshSnapshot GetSurfaceMesh(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const override
    {
        return m_bridge.GetSurfaceMesh(GetData(graph, ref));
    }

    DataEntityId CreateDataEntityId() override
    {
        return m_store.CreateDataEntityId();
    }

    bool SetDataType(DataTypeDescriptor descriptor) override
    {
        return m_store.SetDataType(std::move(descriptor));
    }

    DataCommitResult SetDataCommit(DataTransaction transaction) override
    {
        return m_store.SetDataCommit(std::move(transaction));
    }

    DataObserverId AttachDataChange(DataChangeCallback callback) override
    {
        return m_store.AttachDataChange(std::move(callback));
    }

    bool DetachDataChange(const DataObserverId observerId) override
    {
        return m_store.DetachDataChange(observerId);
    }

    VtkImageGridSnapshot SetPrimaryImage(
        vtkImageData* image,
        vtkImageData* validityMask = nullptr)
    {
        auto payload = m_bridge.CreateImagePayload(image, validityMask);
        if (!payload) return {};
        const auto graph = GetDataGraph();
        DataBinding binding;
        binding.name = std::string(primaryVolumeBinding);
        if (const auto current = GetDataBinding(
                graph, primaryVolumeBinding)) {
            binding = *current;
        }
        const auto entity = CreateDataEntityId();
        const DataRevisionRef ref{ entity, 1 };
        DataTransaction transaction;
        transaction.outputs.push_back(DataRevisionDraft{
            entity, 0, DataTypes::imageGrid3D, {}, std::move(payload),
            DataProvenance{ "TestDataPort", "set-primary", "1", "{}" } });
        transaction.bindings.push_back(DataBindingUpdate{
            std::string(primaryVolumeBinding),
            binding.revision,
            true,
            binding.target,
            ref });
        if (SetDataCommit(std::move(transaction)).status
            != DataCommitStatus::Succeeded) {
            return {};
        }
        return GetPrimaryImage();
    }

    std::pair<VtkLabelMapSnapshot, VtkSurfaceMeshSnapshot>
    SetLabelAndMesh(vtkImageData* labels, vtkPolyData* mesh)
    {
        auto labelPayload = m_bridge.CreateLabelPayload(labels);
        auto meshPayload = m_bridge.CreateMeshPayload(mesh);
        if (!labelPayload || !meshPayload) return {};
        const auto labelEntity = CreateDataEntityId();
        const auto meshEntity = CreateDataEntityId();
        const DataRevisionRef labelRef{ labelEntity, 1 };
        const DataRevisionRef meshRef{ meshEntity, 1 };
        DataTransaction transaction;
        transaction.outputs = {
            DataRevisionDraft{
                labelEntity, 0, DataTypes::labelMap3D, {},
                std::move(labelPayload), {} },
            DataRevisionDraft{
                meshEntity, 0, DataTypes::surfaceMesh, {},
                std::move(meshPayload), {} }
        };
        if (SetDataCommit(std::move(transaction)).status
            != DataCommitStatus::Succeeded) {
            return {};
        }
        const auto graph = GetDataGraph();
        return {
            GetLabelMap(graph, labelRef),
            GetSurfaceMesh(graph, meshRef) };
    }

private:
    DataGraphStore m_store;
    VtkDataBridge m_bridge;
};
