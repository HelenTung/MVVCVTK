#include "AlignmentVoxelVolumeData.h"
#include "AlignmentData.h"
#include <algorithm>
#include <cmath>

namespace AlignmentVoxelVolumeData {
namespace {
std::vector<DataInputRef> Inputs(const VoxelVolumeRequest &r) {
    std::vector<DataInputRef> inputs{{"source",r.source}};
    if(r.validityImage)inputs.push_back({"validity-image",*r.validityImage});return inputs;
}
}
bool GetSnapshotValid(const VoxelVolumeSnapshot &s) {
    if(!GetDataRevisionRefValid(s.result) || !s.parameters || s.source!=s.parameters->source ||
       !AlignmentVoxelVolume::GetRequestValid(*s.parameters,{}) || !GetGridGeometryValid(s.geometry) || s.algorithmVersion!="voxel-cell-volume-1")return false;
    const auto &v=s.statistics;std::uint64_t sum=0,total=1;
    try { if(v.cellVolumeMm3!=AlignmentVoxelVolume::GetCellVolumeMm3(s.geometry))return false; } catch(...) { return false; }
    for(auto count:{v.selected,v.unselected,v.masked,v.noData,v.nonFinite,v.invalidLabels}) { if(count>v.total-sum)return false;sum+=count; }
    if(sum!=v.total || !v.total)return false;
    for(int a=0;a<3;++a) {
        if(v.evaluatedExtent[2*a]<s.geometry.extent[2*a] || v.evaluatedExtent[2*a+1]>s.geometry.extent[2*a+1] || v.evaluatedExtent[2*a]>v.evaluatedExtent[2*a+1])return false;
        const auto width=static_cast<std::uint64_t>(static_cast<std::int64_t>(v.evaluatedExtent[2*a+1])-v.evaluatedExtent[2*a]+1);
        if(width>std::numeric_limits<std::uint64_t>::max()/total)return false;total*=width;
    }
    const auto validity=v.masked || v.noData || v.nonFinite || v.invalidLabels ? VoxelVolumeValidity::IncompleteSamples
        : s.parameters->requireEnclosedSelection && !v.selected ? VoxelVolumeValidity::EmptySelection
        : s.parameters->requireEnclosedSelection && v.touchesRegionBoundary ? VoxelVolumeValidity::TouchesBoundary : VoxelVolumeValidity::Complete;
    return total==v.total && v.evaluatedExtent==s.parameters->extent.value_or(s.geometry.extent) &&
        std::isfinite(v.cellVolumeMm3) && v.cellVolumeMm3>0 && std::isfinite(v.observedVolumeMm3) && v.observedVolumeMm3>=0 &&
        v.observedVolumeMm3==v.selected*v.cellVolumeMm3 && v.validity==validity &&
        (v.volumeMm3.has_value()==(validity==VoxelVolumeValidity::Complete)) && (!v.volumeMm3 || *v.volumeMm3==v.observedVolumeMm3);
}
bool SetType(TrustedDataPort &data) {
    const DataTypeDescriptor type{resultType,{{"org.mvvcvtk.metrology-alignment.voxel-volume"}},[](const IDataPayload &value,std::string &message) {
        const auto *p=dynamic_cast<const Payload *>(&value);if(p && GetSnapshotValid(p->snapshot))return true;message="Invalid voxel volume result.";return false;
    }};
    if(data.SetDataType(type))return true;const auto graph=data.GetDataGraph();return graph.view && graph.view->GetDataFacets(resultType)==type.facets;
}
std::optional<Context> Prepare(const DataGraphSnapshot &graph,VoxelVolumeRequest r,const AlignmentConfig &config) {
    if(!graph.view)return {};
    std::sort(r.labels.begin(),r.labels.end());r.labels.erase(std::unique(r.labels.begin(),r.labels.end()),r.labels.end());
    if(!AlignmentVoxelVolume::GetRequestValid(r,config))return {};
    const auto node=graph.view->GetData(r.source);if(!node)return {};
    Context c;c.work.config=config;
    if(r.method==VoxelVolumeMethod::StoredValueRange)c.source.image=std::dynamic_pointer_cast<const ImageGrid3DPayload>(node->payload);
    else c.source.labels=std::dynamic_pointer_cast<const LabelMap3DPayload>(node->payload);
    if(!c.source.image && !c.source.labels)return {};
    if(c.source.image && (!c.source.image->GetValid() || c.source.image->GetComponentCount()!=1))return {};
    if(c.source.labels && !c.source.labels->GetValid())return {};
    const auto &g=c.source.image ? c.source.image->GetGeometry() : c.source.labels->GetGeometry();
    if(r.validityImage) {
        const auto mask=graph.view->GetData(*r.validityImage);if(!mask)return {};
        c.source.validityImage=std::dynamic_pointer_cast<const ImageGrid3DPayload>(mask->payload);
        if(!c.source.validityImage || !c.source.validityImage->GetValid() || c.source.validityImage->GetComponentCount()!=1 ||
            !AlignmentVoxelVolume::GetGeometrySame(g,c.source.validityImage->GetGeometry()))return {};
    }
    for(const auto &input:Inputs(r)) { DataExpectation e;e.kind=DataExpectationKind::EntityHead;e.entityId=input.source.entityId;e.expectedGeneration=input.source.generation;c.expectations.push_back(e); }
    if(!AlignmentData::GetExpectationsCurrent(graph,c.expectations))return {};
    c.snapshot.source=r.source;c.snapshot.geometry=g;c.snapshot.parameters=std::make_shared<const VoxelVolumeRequest>(std::move(r));return c;
}
DataTransaction BuildTransaction(TrustedDataPort &data,Context &c,const VoxelVolumeData &result,DataRevisionRef &ref) {
    ref={data.CreateDataEntityId(),1};c.snapshot.result=ref;c.snapshot.statistics=result.statistics;
    DataTransaction tx;tx.expectations=c.expectations;
    tx.outputs.push_back({ref.entityId,0,resultType,Inputs(*c.snapshot.parameters),std::make_shared<const Payload>(c.snapshot,c.expectations),
        DataProvenance{"metrology-alignment","voxel-volume",c.snapshot.algorithmVersion,"stored-value/label selection; full affine cell volume; typed parameters"}});return tx;
}
std::optional<VoxelVolumeSnapshot> GetResult(const DataGraphSnapshot &graph,const DataRevisionRef &ref) {
    const auto node=graph.view ? graph.view->GetData(ref) : nullptr;
    const auto p=node && node->type==resultType ? std::dynamic_pointer_cast<const Payload>(node->payload) : nullptr;
    if(!p || p->snapshot.result!=ref)return {};
    for(const auto &input:Inputs(*p->snapshot.parameters)) {
        const auto source=graph.view->GetData(input.source);if(!source)return {};
        const auto *image=dynamic_cast<const ImageGrid3DPayload *>(source->payload.get());
        const auto *labels=dynamic_cast<const LabelMap3DPayload *>(source->payload.get());
        if(input.role=="validity-image" ? !image : p->snapshot.parameters->method==VoxelVolumeMethod::StoredValueRange ? !image : !labels)return {};
        const auto *geometry=image ? &image->GetGeometry() : labels ? &labels->GetGeometry() : nullptr;
        if(!geometry || !AlignmentVoxelVolume::GetGeometrySame(*geometry,p->snapshot.geometry))return {};
        if(std::none_of(node->inputs.begin(),node->inputs.end(),[&](const auto &i){return i.role==input.role && i.source==input.source;}) ||
           std::none_of(p->expectations.begin(),p->expectations.end(),[&](const auto &e){return e.kind==DataExpectationKind::EntityHead && e.use==DataExpectationUse::Required && e.entityId==input.source.entityId && e.expectedGeneration==input.source.generation;}))return {};
    }
    auto result=p->snapshot;result.isCurrent=AlignmentData::GetExpectationsCurrent(graph,p->expectations);return result;
}
}
