#include "Data/RoiService.h"
#include "Geometry/RoiEvaluator.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace {
struct CatalogView final {
    DataBinding binding{std::string(roiCatalogBinding)};
    DataSnapshot data;
    bool isReadable=true;
    std::vector<RoiCatalogEntry> entries;
};

CatalogView GetCatalog(const DataGraphSnapshot& graph)
{
    CatalogView result;
    if (!graph.view) {result.isReadable=false;return result;}
    if (auto binding=graph.view->GetDataBinding(roiCatalogBinding)) result.binding=*binding;
    if (!result.binding.target) return result;
    result.data=graph.view->GetData(*result.binding.target);
    if (result.data) {
        const auto* catalog=dynamic_cast<const RoiCatalogPayload*>(result.data->payload.get());
        if (catalog) result.entries=catalog->GetEntries();else result.isReadable=false;
    }else result.isReadable=false;
    return result;
}

bool GetMetadataValid(const RoiMetadata& metadata) noexcept
{
    return !metadata.name.empty() && metadata.name.size()<=256
        && metadata.group.size()<=256 && metadata.description.size()<=4096;
}

bool GetMetadataEmpty(const RoiMetadata& metadata) noexcept
{
    return metadata.name.empty() && metadata.group.empty() && metadata.description.empty() && !metadata.isArchived;
}

RoiResult GetFailure(RoiError error,const char* message)
{
    RoiResult result; result.error=error; result.message=message; return result;
}
}

bool RoiService::GetInputTransient(const DataSnapshot& input) noexcept
{
    // Host 原始体数据也有 owner scope，但其来源由 Session 管理，不属于 Feature 临时结果。
    return input && GetDataEntityIdValid(input->lifetimeScope)
        && (input->lifetimePolicy == DataLifetimePolicy::ExplicitRetirement
            || !input->provenance || input->provenance->producerId != "org.mvvcvtk.host");
}

std::optional<RoiDescriptor> RoiService::GetDescriptor(const DataGraphSnapshot& graph,const DataRevisionRef& ref)
{
    if (!graph.view) return {};
    const auto data=graph.view->GetData(ref);
    const auto* payload=data ? dynamic_cast<const RoiGeometryPayload*>(data->payload.get()):nullptr;
    if (!payload || data->type!=DataTypes::roiGeometry) return {};
    const auto binding=graph.view->GetDataBinding(roiCatalogBinding);
    const auto catalogData=binding && binding->target ? graph.view->GetData(*binding->target):DataSnapshot{};
    const auto* catalog=catalogData ? dynamic_cast<const RoiCatalogPayload*>(catalogData->payload.get()):nullptr;
    if (!catalog) return {};
    // 单对象读取只定位目录项，不复制整个目录。
    const auto& entries=catalog->GetEntries();
    const auto entry=std::find_if(entries.begin(),entries.end(),[&](const RoiCatalogEntry& item) {
        return item.geometry.entityId==ref.entityId;
    });
    if (entry==entries.end()) return {};
    return RoiDescriptor{ref,entry->geometry,payload->GetDefinition(),entry->metadata,binding->revision};
}

std::vector<RoiDescriptor> RoiService::GetDescriptors(const DataGraphSnapshot& graph,bool includeArchived)
{
    std::vector<RoiDescriptor> result;
    const auto catalog=GetCatalog(graph);
    result.reserve(catalog.entries.size());
    for (const auto& entry:catalog.entries) {
        if (!includeArchived && entry.metadata.isArchived) continue;
        const auto data=graph.view->GetData(entry.geometry);
        const auto* payload=data ? dynamic_cast<const RoiGeometryPayload*>(data->payload.get()):nullptr;
        if (payload) result.push_back({entry.geometry,entry.geometry,payload->GetDefinition(),entry.metadata,catalog.binding.revision});
    }
    return result;
}

RoiResult RoiService::SetRoi(const RoiRequest& request)
{
    return SetRoiCommit(request,{});
}

RoiResult RoiService::SetRoiCommit(const RoiRequest& request,std::vector<DataRevisionDraft> masks)
{
    try {
        // 1. 在一个不可变 graph 中解析目录和请求；正式写入仍由 Store CAS 决定。
        const auto graph=m_store.GetDataGraph();
        auto catalog=GetCatalog(graph);
        if(!catalog.isReadable)return GetFailure(RoiError::MissingInput,"ROI catalog cannot be read.");
        if (request.expectedCatalogRevision && catalog.binding.revision!=*request.expectedCatalogRevision)
            return GetFailure(RoiError::RevisionConflict,"ROI catalog changed.");
        const bool creates=request.action==RoiAction::Create || request.action==RoiAction::Copy;
        const bool removes=request.action==RoiAction::Remove;
        const bool writesGeometry=request.action!=RoiAction::SetMetadata && !removes;
        if(!writesGeometry && (request.provenance || !request.inputs.empty()))return GetFailure(RoiError::InvalidRequest,"Metadata/removal requests cannot carry geometry provenance.");
        if (request.action!=RoiAction::Create && request.action!=RoiAction::Copy
            && request.action!=RoiAction::SetGeometry && request.action!=RoiAction::SetMetadata && !removes)
            return GetFailure(RoiError::InvalidRequest,"Unknown ROI action.");
        if ((creates && request.expectedRoi) || (!creates && !request.expectedRoi)
            || (request.action==RoiAction::Copy)!=request.copyFrom.has_value())
            return GetFailure(RoiError::InvalidRequest,"ROI action fields conflict.");
        if ((request.action==RoiAction::Copy || request.action==RoiAction::SetMetadata || removes)
            && (!request.definition.nodes.empty() || GetDataRevisionRefValid(request.definition.source)))
            return GetFailure(RoiError::InvalidRequest,"This ROI action cannot contain geometry.");
        if ((request.action==RoiAction::SetGeometry && !GetMetadataEmpty(request.metadata) && !GetMetadataValid(request.metadata))
            || (removes && !GetMetadataEmpty(request.metadata))
            || (request.action!=RoiAction::SetGeometry && !removes && !GetMetadataValid(request.metadata)))
            return GetFailure(RoiError::InvalidRequest,"ROI metadata is invalid for the action.");
        auto selected=catalog.entries.end();
        if (!creates) {
            selected=std::find_if(catalog.entries.begin(),catalog.entries.end(),[&](const RoiCatalogEntry& item) {
                return item.geometry.entityId==request.expectedRoi->entityId;
            });
            if (selected==catalog.entries.end()) return GetFailure(RoiError::MissingInput,"ROI is not in the catalog.");
            if (selected->geometry!=*request.expectedRoi)
                return GetFailure(RoiError::RevisionConflict,"ROI geometry changed.");
        }
        RoiDefinition definition;
        if (writesGeometry) {
            if (request.copyFrom) {
                const auto copy=graph.view->GetData(*request.copyFrom);
                const auto* payload=copy ? dynamic_cast<const RoiGeometryPayload*>(copy->payload.get()):nullptr;
                if (!payload) return GetFailure(RoiError::MissingInput,"Copy source is not an ROI revision.");
                definition=payload->GetDefinition();
            } else {
                const auto error=RoiEvaluator::GetDefinitionError(request.definition);
                if (error!=RoiError::None) return GetFailure(error,"ROI definition is invalid.");
                definition=request.definition;
            }
            if (!creates) {
                const auto previous=graph.view->GetData(selected->geometry);
                const auto* payload=previous ? dynamic_cast<const RoiGeometryPayload*>(previous->payload.get()):nullptr;
                if (!payload || payload->GetDefinition().source!=definition.source)
                    return GetFailure(RoiError::SourceMismatch,"Geometry edits cannot change the ROI source.");
            }
        }
        // 2. 几何与目录只有一次正式提交；元信息操作不改几何修订。
        DataTransaction transaction;
        transaction.outputs=std::move(masks);
        if (request.expectedSourceBinding) {
            const auto& binding=*request.expectedSourceBinding;
            if (!writesGeometry || binding.name.empty() || binding.target!=definition.source)
                return GetFailure(RoiError::InvalidRequest,"ROI source binding expectation is invalid.");
            DataExpectation expectation; expectation.kind=DataExpectationKind::Binding;
            expectation.binding=binding.name; expectation.expectedBindingRevision=binding.revision;
            expectation.isTargetChecked=true; expectation.expectedTarget=binding.target;
            transaction.expectations.push_back(std::move(expectation));
        }
        DataRevisionRef target=creates ? DataRevisionRef{m_store.CreateDataEntityId(),1}:selected->geometry;
        if (writesGeometry) {
            if (!creates) {
                if (target.generation==std::numeric_limits<DataGeneration>::max()) return GetFailure(RoiError::TooLarge,"ROI generation exhausted.");
                ++target.generation;
            }
            DataRevisionDraft draft{target.entityId,target.generation-1,DataTypes::roiGeometry,
                RoiEvaluator::GetInputs(definition),std::make_shared<const RoiGeometryPayload>(definition),
                request.provenance.value_or(DataProvenance{"Host.Roi","set-geometry","2",{}})};
            draft.inputs.insert(draft.inputs.end(),request.inputs.begin(),request.inputs.end());
            if (request.copyFrom) draft.inputs.push_back({"copy-source",*request.copyFrom});
            const DataRevision candidate{target,draft.type,draft.inputs,draft.payload,draft.provenance};
            const auto error=RoiEvaluator::GetRelationsError(candidate,[&](const DataRevisionRef& ref)->DataSnapshot {
                for (const auto& mask:transaction.outputs) if (mask.entityId==ref.entityId && mask.expectedGeneration+1==ref.generation)
                    return std::make_shared<const DataRevision>(DataRevision{ref,mask.type,mask.inputs,mask.payload,mask.provenance});
                return graph.view->GetData(ref);
            });
            if (error!=RoiError::None) return GetFailure(error,"ROI input relations are invalid.");
            // 输入引用固定的不可变修订；Store 校验其存在，不要求对应实体的最新 head 仍相同。
            transaction.outputs.push_back(std::move(draft));
        } else {
            DataExpectation expectation;
            expectation.entityId=target.entityId; expectation.expectedGeneration=target.generation;
            transaction.expectations.push_back(expectation);
        }
        if (removes) catalog.entries.erase(selected);
        else if (creates) catalog.entries.push_back({target,request.metadata});
        else {
            selected->geometry=target;
            if (request.action==RoiAction::SetMetadata || (request.action==RoiAction::SetGeometry && !GetMetadataEmpty(request.metadata))) selected->metadata=request.metadata;
        }
        std::sort(catalog.entries.begin(),catalog.entries.end(),[](const RoiCatalogEntry& a,const RoiCatalogEntry& b){return a.geometry.entityId<b.geometry.entityId;});
        const DataRevisionRef catalogRef=catalog.data ? DataRevisionRef{catalog.data->self.entityId,catalog.data->self.generation+1}
            : DataRevisionRef{m_store.CreateDataEntityId(),1};
        if (catalogRef.generation==0) return GetFailure(RoiError::TooLarge,"ROI catalog generation exhausted.");
        std::vector<DataInputRef> inputs;
        for (std::size_t i=0;i<catalog.entries.size();++i) inputs.push_back({"roi-"+std::to_string(i),catalog.entries[i].geometry});
        const auto outcomeEntry=std::find_if(catalog.entries.begin(),catalog.entries.end(),[&](const RoiCatalogEntry& item){return item.geometry==target;});
        RoiDefinition outcomeDefinition=writesGeometry ? definition
            : dynamic_cast<const RoiGeometryPayload&>(*graph.view->GetData(target)->payload).GetDefinition();
        RoiResult result;
        if (!removes) result.roi=RoiDescriptor{target,target,std::move(outcomeDefinition),outcomeEntry->metadata,catalog.binding.revision+1};
        transaction.outputs.push_back({catalogRef.entityId,catalogRef.generation-1,DataTypes::roiCatalog,
            std::move(inputs),std::make_shared<const RoiCatalogPayload>(std::move(catalog.entries)),
            DataProvenance{"Host.Roi","set-catalog","1",{}}});
        // 几何、mask 与共享目录分别使用独立 scope，目录不能把其他数据集的 ROI 夹带进退役。
        for (auto& output : transaction.outputs) {
            bool explicitRetirement = false;
            for (const auto& input : output.inputs) {
                const auto source = graph.view->GetData(input.source);
                if (GetInputTransient(source)) explicitRetirement = true;
                for (const auto& staged : transaction.outputs)
                    if (staged.entityId == input.source.entityId && staged.expectedGeneration + 1 == input.source.generation
                        && staged.lifetimeScope) explicitRetirement = true;
            }
            if (explicitRetirement && !output.lifetimeScope) {
                output.lifetimeScope = m_store.CreateDataEntityId();
                output.lifetimePolicy = DataLifetimePolicy::ExplicitRetirement;
            }
        }
        transaction.bindings.push_back({std::string(roiCatalogBinding),catalog.binding.revision,true,catalog.binding.target,catalogRef});
        const auto committed=m_store.SetDataCommit(std::move(transaction));
        if (committed.status!=DataCommitStatus::Succeeded)
            return GetFailure(committed.failureReason==DataCommitFailure::ExpectationFailed ? RoiError::RevisionConflict:RoiError::CommitFailed,
                "ROI transaction was rejected.");
        // 3. 返回预先构建的本次提交结果，观察者重入不改变它且不再分配。
        result.error=RoiError::None;
        result.commitId=committed.commitId;
        return result;
    } catch (const std::bad_alloc&) { return GetFailure(RoiError::TooLarge,"ROI allocation failed."); }
    catch (...) { return GetFailure(RoiError::InvalidRequest,"ROI request failed validation."); }
}
