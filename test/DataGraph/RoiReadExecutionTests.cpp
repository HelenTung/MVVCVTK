#include "RoiArchiveTestReader.h"
#include "Data/RoiService.h"
#include "Geometry/RoiEvaluator.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Fixture final {
    DataGraphStore store;
    RoiService service{store};
    DataRevisionRef source{store.CreateDataEntityId(),1};
    Fixture(){
        GridGeometry3D grid;grid.extent={0,8,0,8,0,8};grid.dimensions={9,9,9};
        DataTransaction transaction;transaction.outputs={{source.entityId,0,DataTypes::imageGrid3D,{},
            std::make_shared<const ImageGrid3DPayload>(grid,ImageValueType::UInt8,1,std::make_shared<const std::vector<std::uint8_t>>(729,1))}};
        Require(store.SetDataCommit(std::move(transaction)).status==DataCommitStatus::Succeeded,"query fixture failed");
    }
};
void QueryAndChunks(){
    Fixture fixture;RoiDefinition definition{fixture.source,{{RoiNodeKind::SourceDomain}}};
    RoiPrimitive left;left.shape=RoiShape::HalfSpace;left.origin={2,0,0};left.normal={1,0,0};
    definition.nodes.push_back({RoiNodeKind::Primitive,left});definition.nodes.push_back({RoiNodeKind::Intersection,{},0,1});
    auto right=left;right.origin={5,0,0};right.normal={-1,0,0};
    definition.nodes.push_back({RoiNodeKind::Primitive,right});definition.nodes.push_back({RoiNodeKind::Intersection,{},2,3});
    for(unsigned i=0;i<160;++i){const auto previous=static_cast<std::uint32_t>(definition.nodes.size()-1);definition.nodes.push_back({RoiNodeKind::Intersection,{},previous,previous});}
    const auto read=RoiEvaluator::GetPreview(fixture.store.GetDataGraph(),definition);
    Require(read.error==RoiError::None && read.roi,"long expression preparation failed");
    std::atomic<bool> valid{true};std::vector<std::thread> workers;
    // 每个线程只准备一次上下文；独立预期来自两个半空间的交集。
    for(unsigned worker=0;worker<4;++worker)workers.emplace_back([&]{
        const auto query=read.roi->GetQuery();if(query.error!=RoiError::None || !query.query){valid=false;return;}
        for(int z=0;z<9;++z)for(int y=0;y<9;++y)for(int x=0;x<9;++x)
            if(query.query->GetContains({double(x),double(y),double(z)})!=(x>=2 && x<=5))valid=false;
    });
    for(auto& worker:workers)worker.join();Require(valid,"concurrent expression membership differs from half-space truth");
    RoiMaskRequest request;request.region={{0,0,0},{9,9,9}};request.chunkBytes=7;std::size_t count=0,total=0;
    for(;;){const auto chunk=read.roi->GetMaskChunk(request);Require(chunk.error==RoiError::None && chunk.values.size()<=7,"mask batch failed");
        for(auto value:chunk.values)count+=value;total+=chunk.values.size();if(chunk.isComplete)break;
        Require(chunk.nextOffset>request.voxelOffset,"mask did not advance");request.voxelOffset=chunk.nextOffset;
    }
    Require(total==729 && count==4*9*9,"multi-batch mask was truncated or changed membership");
    const RoiTriangle triangle{{{0,0,4},{8,0,4},{0,8,4}}};std::size_t pieces=0;double area=0;
    const auto selected=read.roi->GetTriangleChunks(triangle,1,[&](const auto& batch){
        Require(batch.size()==1,"triangle batch exceeds its capacity");++pieces;
        const auto& p=batch.front();area+=std::abs((p[1][0]-p[0][0])*(p[2][1]-p[0][1])-(p[1][1]-p[0][1])*(p[2][0]-p[0][0]))*.5;return true;
    });
    // 三角形 y<=8-x 在 2<=x<=5 的积分面积为 13.5。
    Require(selected.error==RoiError::None && selected.triangles.empty() && pieces>1 && std::abs(area-13.5)<1e-12,"streamed clipped area changed");
    Require(read.roi->GetTriangleChunks(triangle,1,[](const auto&){return false;}).error==RoiError::Cancelled,"consumer cancellation became successful truncated selection");
    auto context=read.roi->GetQuery();Require(context.query && context.query->GetContains({3,4,5}),"owned query lost its read view");
}
void LargeChunk(){
    DataGraphStore store;const DataRevisionRef source{store.CreateDataEntityId(),1};const auto count=roiChunkBytes+1;
    GridGeometry3D grid;grid.extent={0,static_cast<int>(count-1),0,0,0,0};grid.dimensions={static_cast<int>(count),1,1};
    DataTransaction transaction;transaction.outputs={{source.entityId,0,DataTypes::imageGrid3D,{},std::make_shared<const ImageGrid3DPayload>(grid,ImageValueType::UInt8,1,std::make_shared<const std::vector<std::uint8_t>>(count,1))}};
    Require(store.SetDataCommit(std::move(transaction)).status==DataCommitStatus::Succeeded,"large chunk fixture failed");const auto read=RoiEvaluator::GetPreview(store.GetDataGraph(),{source,{{RoiNodeKind::SourceDomain}}});Require(read.roi!=nullptr,"large chunk ROI failed");
    RoiMaskRequest request;request.region={{0,0,0},{count,1,1}};request.chunkBytes=count;const auto chunk=read.roi->GetMaskChunk(request);
    Require(chunk.error==RoiError::None && chunk.isComplete && chunk.nextOffset==count && chunk.values.size()==count && std::all_of(chunk.values.begin(),chunk.values.end(),[](auto value){return value==1;}),"explicit chunk capacity still acts as a fixed eight-MiB limit");
}
class InterruptedReader final:public RoiArchiveReader {
public:
    explicit InterruptedReader(std::shared_ptr<const RoiArchiveReader> reader,bool early=false):m_reader(std::move(reader)),m_early(early){}
    const RoiArchiveDescriptor& GetMetadata()const noexcept override{return m_reader->GetMetadata();}
    std::optional<std::size_t> GetMaskCount(std::size_t index)const noexcept override{return m_reader->GetMaskCount(index);}
    std::optional<std::size_t> GetVoxelPayloadCount(std::size_t index)const noexcept override{return m_reader->GetVoxelPayloadCount(index);}
    RoiArchiveMaskChunk GetMaskChunk(const RoiArchiveChunkRequest& request)const override{return m_reader->GetMaskChunk(request);}
    RoiArchiveVoxelPayloadChunk GetVoxelPayloadChunk(const RoiArchiveChunkRequest& request)const override{
        if(request.elementOffset)return {RoiError::Cancelled};auto small=request;small.chunkElements=1;auto chunk=m_reader->GetVoxelPayloadChunk(small);if(m_early)chunk.isComplete=true;return chunk;
    }
private:std::shared_ptr<const RoiArchiveReader> m_reader;bool m_early=false;
};
// 历史 v3 解码器仍逐块给出体素；新 C++ 接口不携带完整历史载荷。
class LegacyReader final:public RoiArchiveTestReader {
public:
    explicit LegacyReader(std::shared_ptr<const RoiArchiveReader> reader):RoiArchiveTestReader(std::move(reader)){
        metadata.schemaVersion=3;for(auto& item:metadata.rois)item.schemaVersion=3;
    }
    mutable std::size_t calls=0;
    std::optional<std::size_t> GetVoxelPayloadCount(std::size_t i)const noexcept override{return i==0?std::optional<std::size_t>(2):std::nullopt;}
    RoiArchiveVoxelPayloadChunk GetVoxelPayloadChunk(const RoiArchiveChunkRequest& r)const override{
        ++calls;RoiArchiveVoxelPayloadChunk result;const std::array<RoiVoxel,2> values{{{{7,7,7},2},{{1,1,1},1}}};
        if(r.payloadIndex||r.elementOffset>values.size()||!r.chunkElements)return {RoiError::InvalidRequest};
        const auto count=std::min(r.chunkElements,values.size()-r.elementOffset);
        result.legacyVoxels.assign(values.begin()+r.elementOffset,values.begin()+r.elementOffset+count);
        result.error=RoiError::None;result.nextOffset=r.elementOffset+count;result.isComplete=result.nextOffset==values.size();return result;
    }
};
void SharedArchive(){
    Fixture source;RoiPrimitive primitive;primitive.shape=RoiShape::VoxelSet;
    primitive.voxels=MakeRoiVoxelFixture(std::vector<RoiVoxel>{{{1,1,1},1},{{7,7,7},1}});
    RoiRequest request;request.metadata.name="共享拆分";request.definition={source.source,{{RoiNodeKind::Primitive,primitive}}};
    const auto created=source.service.SetRoi(request);Require(created.roi.has_value(),"archive fixture ROI failed");
    RoiOperationRequest split;split.source=source.source;split.target=created.roi->revision;RoiComponentParams parameters;parameters.operation=RoiComponentOperation::Split;split.params=parameters;
    const auto parts=source.service.SetOperation(split);Require(parts.error==RoiError::None && parts.rois.size()==2,"split fixture failed");
    const auto planned=RoiService::GetArchiveReader(source.store.GetDataGraph(),{{parts.rois[0].revision,"ct"},{parts.rois[1].revision,"ct"}});
    Require(planned.error==RoiError::None && planned.reader,"streamed archive planning failed");
    const auto& metadata=planned.reader->GetMetadata();
    Require(metadata.rois.size()==2 && metadata.voxels.size()==1,"payload copied before shared archive planning");
    RoiArchiveChunkRequest chunk;chunk.chunkElements=1;const auto first=planned.reader->GetVoxelPayloadChunk(chunk);chunk.elementOffset=first.nextOffset;
    const auto second=planned.reader->GetVoxelPayloadChunk(chunk);
    Require(first.error==RoiError::None && !first.isComplete && first.runs.size()==1 && first.runs[0].first==std::array<int,3>{1,1,1}
        && second.error==RoiError::None && second.isComplete && second.runs.size()==1 && second.runs[0].first==std::array<int,3>{7,7,7},"payload chunks did not preserve exact indices");
    Fixture target;const auto before=target.store.GetDataGraph().commitId;
    const auto interrupted=target.service.LoadArchiveReader(InterruptedReader(planned.reader),{{"ct",target.source}});
    Require(interrupted.error==RoiError::Cancelled && interrupted.rois.empty() && target.store.GetDataGraph().commitId==before,"interrupted payload published a partial catalog");
    const auto truncated=target.service.LoadArchiveReader(InterruptedReader(planned.reader,true),{{"ct",target.source}});Require(truncated.error==RoiError::InvalidRequest && target.store.GetDataGraph().commitId==before,"premature complete flag restored truncated voxel support");
    RoiArchiveRestoreOptions cancelled;cancelled.getCancelled=[] {return true;};
    Require(target.service.LoadArchiveReader(*planned.reader,{{"ct",target.source}},cancelled).error==RoiError::Cancelled
        &&target.store.GetDataGraph().commitId==before,"explicit restore cancellation published a candidate");
    cancelled.getCancelled=[]()->bool {throw std::runtime_error("cancelled reader");};
    Require(target.service.LoadArchiveReader(*planned.reader,{{"ct",target.source}},cancelled).error==RoiError::Cancelled
        &&target.store.GetDataGraph().commitId==before,"throwing cancellation callback changed error semantics or published data");
    for(std::size_t width:{1,2,7}){
        Fixture legacyTarget;LegacyReader legacy(planned.reader);RoiArchiveRestoreOptions options;options.chunkElements=width;
        const auto loaded=legacyTarget.service.LoadArchiveReader(legacy,{{"ct",legacyTarget.source}},options);
        Require(loaded.error==RoiError::None&&loaded.rois.size()==2&&legacy.calls==(width==1?2:1),"legacy chunks were duplicated or not restored");
        const auto& shared=loaded.rois[0].definition.nodes[0].primitive.voxels;
        Require(shared==loaded.rois[1].definition.nodes[0].primitive.voxels&&shared->GetLabel({1,1,1})==1&&shared->GetLabel({7,7,7})==2,
            "legacy block boundaries changed exact labels or shared payload ownership");
    }
    RoiArchiveTestReader unsupported(planned.reader);unsupported.metadata.schemaVersion=99;
    Require(target.service.LoadArchiveReader(unsupported,{{"ct",target.source}}).error==RoiError::InvalidRequest
        &&target.store.GetDataGraph().commitId==before,"unsupported schema was silently interpreted as current data");
    const auto restored=target.service.LoadArchiveReader(*planned.reader,{{"ct",target.source}});
    Require(restored.error==RoiError::None && restored.rois.size()==2 && target.store.GetDataGraph().commitId==before+1,"streamed restore was not atomic");
    const auto& a=restored.rois[0].definition.nodes.front().primitive;const auto& b=restored.rois[1].definition.nodes.front().primitive;
    Require(a.voxels==b.voxels && a.labels!=b.labels,"streamed restore duplicated split support or merged output identities");
    for(unsigned i=0;i<2;++i){const auto read=RoiEvaluator::GetRoi(target.store.GetDataGraph(),restored.rois[i].revision,target.source);
        Require(read.roi && read.roi->GetContains(i?std::array<double,3>{7,7,7}:std::array<double,3>{1,1,1})
            && !read.roi->GetContains(i?std::array<double,3>{1,1,1}:std::array<double,3>{7,7,7}),"split membership changed during streamed restore");}
}
}
bool TestRoiReadExecution(){try{QueryAndChunks();LargeChunk();SharedArchive();return true;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return false;}}
