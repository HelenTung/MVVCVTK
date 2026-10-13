#include "Data/RoiService.h"
#include "Data/DataPayloads.h"
#include "Geometry/RoiEvaluator.h"
#include "Data/Internal/ReadBudget.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace {
// Windows x64 数据契约按小端 uint64/f64 编码；摘要不包含运行期 ID 或地址。
class GeometryHash final {
public:
    void Add(std::uint64_t value){for(int b=0;b<8;++b)Byte(static_cast<std::uint8_t>(value>>(8*b)));}
    void Add(double value){if(value==0)value=0;std::uint64_t bits;std::memcpy(&bits,&value,8);Add(bits);}
    std::string Finish(){const auto bits=m_size*8;Byte(0x80);while(m_used!=56)Byte(0);for(int i=7;i>=0;--i)Byte(static_cast<std::uint8_t>(bits>>(8*i)));constexpr char hex[]="0123456789abcdef";std::string result;for(auto word:m_state)for(int i=7;i>=0;--i)result.push_back(hex[(word>>(4*i))&15]);return result;}
private:
    static std::uint32_t Rotate(std::uint32_t value,unsigned n){return (value>>n)|(value<<(32-n));}
    void Byte(std::uint8_t value){m_block[m_used++]=value;++m_size;if(m_used==64){Block();m_used=0;}}
    void Block(){
        static constexpr std::uint32_t constants[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64]{};for(int i=0;i<16;++i)for(int j=0;j<4;++j)w[i]=(w[i]<<8)|m_block[i*4+j];
        for(int i=16;i<64;++i){const auto a=w[i-15],b=w[i-2];w[i]=w[i-16]+(Rotate(a,7)^Rotate(a,18)^(a>>3))+w[i-7]+(Rotate(b,17)^Rotate(b,19)^(b>>10));}
        auto a=m_state[0],b=m_state[1],c=m_state[2],d=m_state[3],e=m_state[4],f=m_state[5],g=m_state[6],h=m_state[7];
        for(int i=0;i<64;++i){const auto t1=h+(Rotate(e,6)^Rotate(e,11)^Rotate(e,25))+((e&f)^(~e&g))+constants[i]+w[i];const auto t2=(Rotate(a,2)^Rotate(a,13)^Rotate(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        m_state[0]+=a;m_state[1]+=b;m_state[2]+=c;m_state[3]+=d;m_state[4]+=e;m_state[5]+=f;m_state[6]+=g;m_state[7]+=h;
    }
    std::array<std::uint32_t,8> m_state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};std::array<std::uint8_t,64> m_block{};std::size_t m_used=0;std::uint64_t m_size=0;
};
ImageDescriptor GetGridDescriptor(const GridGeometry3D& grid)
{
    ImageDescriptor result;
    result.extent=grid.extent; result.dims=grid.dimensions;
    result.spacing=grid.spacing; result.origin=grid.origin; result.direction=grid.direction;
    return result;
}

RoiSourceDescriptor GetSourceDescriptor(const DataSnapshot& source)
{
    RoiSourceDescriptor result;
    if (!source) return result;
    result.type=source->type;
    if (const auto* image=dynamic_cast<const ImageGrid3DPayload*>(source->payload.get())) {
        auto grid=GetGridDescriptor(image->GetGeometry());
        grid.valueType=image->GetValueType(); grid.componentCount=image->GetComponentCount();
        grid.componentBytes=GetImageValueBytes(grid.valueType); grid.scalarRange=image->GetScalarRange();
        grid.metadata.identity=image->GetMetadata().identity;
        grid.metadata.source.digest=image->GetMetadata().source.digest;
        result.image=std::move(grid); result.coordinateFrame=image->GetGeometry().coordinateFrame;
    } else if (const auto* mesh=dynamic_cast<const SurfaceMeshPayload*>(source->payload.get())) {
        result.pointCount=mesh->GetVertices().size()/3; result.triangleCount=mesh->GetTriangles().size()/3;
        result.sourceBounds={INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
        for (std::size_t i=0;i<mesh->GetVertices().size();++i) {
            const auto a=i%3; const auto value=mesh->GetVertices()[i];
            result.sourceBounds[a*2]=std::min(result.sourceBounds[a*2],value);
            result.sourceBounds[a*2+1]=std::max(result.sourceBounds[a*2+1],value);
        }
        result.coordinateFrame=mesh->GetCoordinateFrame();GeometryHash hash;hash.Add(static_cast<std::uint64_t>(result.pointCount));for(auto value:mesh->GetVertices())hash.Add(value);hash.Add(static_cast<std::uint64_t>(result.triangleCount));for(auto value:mesh->GetTriangles())hash.Add(value);result.meshGeometryDigest=hash.Finish();
    }
    return result;
}

bool GetSameGrid(const ImageDescriptor& a,const ImageDescriptor& b) noexcept
{
    return a.extent==b.extent && a.dims==b.dims && a.spacing==b.spacing && a.origin==b.origin && a.direction==b.direction;
}

bool GetSameSource(const RoiSourceDescriptor& a,const RoiSourceDescriptor& b) noexcept
{
    if (a.type!=b.type || a.coordinateFrame!=b.coordinateFrame || a.image.has_value()!=b.image.has_value()) return false;
    if (!a.image) return a.sourceBounds==b.sourceBounds && a.pointCount==b.pointCount && a.triangleCount==b.triangleCount && a.meshGeometryDigest.size()==64 && a.meshGeometryDigest==b.meshGeometryDigest;
    const auto& x=*a.image; const auto& y=*b.image;
    return GetSameGrid(x,y) && x.valueType==y.valueType && x.componentCount==y.componentCount
        && x.componentBytes==y.componentBytes && x.metadata.identity.datasetId==y.metadata.identity.datasetId
        && x.metadata.identity.objectId==y.metadata.identity.objectId && x.metadata.identity.inspectionId==y.metadata.identity.inspectionId
        && x.metadata.identity.batchId==y.metadata.identity.batchId && x.metadata.source.digest==y.metadata.source.digest;
}

bool AddBytes(std::size_t& total,std::size_t count,std::size_t width=1) noexcept
{
    if (width && count>(std::numeric_limits<std::size_t>::max()-total)/width) return false;
    total+=count*width; return true;
}

// 先按只读长度计费；调用方在任何来源字符串复制或网格遍历前执行此检查。
std::size_t GetSourceWorkBytes(const DataSnapshot& source) noexcept
{
    std::size_t count=sizeof(RoiSourceDescriptor);
    if (const auto* image=dynamic_cast<const ImageGrid3DPayload*>(source->payload.get())) {
        const auto& m=image->GetMetadata();
        if (!AddBytes(count,image->GetGeometry().coordinateFrame.size(),2)
            || !AddBytes(count,m.identity.datasetId.size(),2)) return std::numeric_limits<std::size_t>::max();
        for (const auto* text:{&m.identity.inspectionId,&m.identity.objectId,&m.identity.batchId,&m.source.digest})
            if (*text && !AddBytes(count,(*text)->size(),2)) return std::numeric_limits<std::size_t>::max();
    } else if (const auto* mesh=dynamic_cast<const SurfaceMeshPayload*>(source->payload.get())) {
        if (!AddBytes(count,mesh->GetCoordinateFrame().size(),2)
            || !AddBytes(count,mesh->GetVertices().size(),sizeof(double))) return std::numeric_limits<std::size_t>::max();
    }
    return count;
}

bool AddNodeBytes(std::size_t& count,const std::vector<RoiNode>& nodes) noexcept
{
    if(!AddBytes(count,nodes.size(),sizeof(RoiNode)*3))return false;
    for(const auto& node:nodes){const auto& p=node.primitive;
        if(!AddBytes(count,p.contours.size(),sizeof(std::vector<std::array<double,2>>)*3)
            || !AddBytes(count,p.path.size(),sizeof(std::array<double,3>)*3)
            || !AddBytes(count,p.labels.size(),sizeof(std::uint64_t)*3))return false;
        for(const auto& ring:p.contours)if(!AddBytes(count,ring.size(),sizeof(std::array<double,2>)*3))return false;
    }return true;
}
std::size_t GetArchiveBytes(const RoiArchive& archive) noexcept
{
    std::size_t count=sizeof(RoiArchive);
    if (!AddNodeBytes(count,archive.nodes)
        || !AddBytes(count,archive.sourceKey.size(),2)
        || !AddBytes(count,archive.metadata.name.size()+archive.metadata.group.size()+archive.metadata.description.size(),2)
        || !AddBytes(count,archive.source.coordinateFrame.size()+archive.source.meshGeometryDigest.size(),2)
        || !AddBytes(count,archive.provenance.producerId.size()+archive.provenance.operationId.size()+archive.provenance.algorithmVersion.size()+archive.provenance.canonicalParameters.size(),3)) return std::numeric_limits<std::size_t>::max();
    if (archive.source.image) {
        const auto& m=archive.source.image->metadata;
        for (const auto* text:{&m.identity.datasetId,&m.source.uri,&m.scalar.quantity,&m.scalar.unit})
            if (!AddBytes(count,text->size(),2)) return std::numeric_limits<std::size_t>::max();
        for (const auto* text:{&m.identity.inspectionId,&m.identity.objectId,&m.identity.batchId,&m.source.digest})
            if (*text && !AddBytes(count,(*text)->size(),2)) return std::numeric_limits<std::size_t>::max();
        // 归档源描述不保存可扩张的任意属性或原文件路径。
        if (!m.attributes.empty() || !m.source.uri.empty()) return std::numeric_limits<std::size_t>::max();
    }
    for (const auto& mask:archive.masks) {
        if (!AddBytes(count,mask.values.size(),sizeof(std::uint64_t)*3) || !AddBytes(count,mask.nodeIndices.size(),sizeof(std::uint32_t)*3)
            || !AddBytes(count,sizeof(RoiArchiveMask))) return std::numeric_limits<std::size_t>::max();
    }
    for (const auto& saved:archive.voxels)
        if (!AddBytes(count,sizeof(saved)) || !AddBytes(count,saved.values.size(),sizeof(RoiVoxel)*8) || !AddBytes(count,saved.nodeIndices.size(),sizeof(std::uint32_t)*2)) return std::numeric_limits<std::size_t>::max();
    return count;
}
}

RoiArchiveResult RoiService::GetArchive(const DataGraphSnapshot& graph,const DataRevisionRef& ref,
    const std::string& sourceKey,std::size_t maxBytes)
{
    RoiArchiveResult result;
    const auto budget=GetReadBudget(maxBytes);
    if (sourceKey.empty() || sourceKey.size()>256) { result.error=RoiError::SourceUnresolved; return result; }
    try {
        if(!graph.view){result.error=RoiError::Unavailable;return result;}
        const auto data=graph.view->GetData(ref);
        const auto* geometry=data?dynamic_cast<const RoiGeometryPayload*>(data->payload.get()):nullptr;
        if(!geometry){result.error=RoiError::MissingInput;return result;}
        std::size_t definitionBytes=0;
        if(!AddNodeBytes(definitionBytes,geometry->GetDefinition().nodes) || definitionBytes>budget){result.error=RoiError::TooLarge;result.requiredBytes=definitionBytes;return result;}
        const auto descriptor=GetDescriptor(graph,ref);
        if (!descriptor) { result.error=RoiError::MissingInput; return result; }
        const auto source=graph.view->GetData(descriptor->definition.source);
        if (!source) { result.error=RoiError::SourceUnresolved; return result; }
        result.requiredBytes=GetSourceWorkBytes(source);
        if (result.requiredBytes==std::numeric_limits<std::size_t>::max() || result.requiredBytes>budget) { result.error=RoiError::TooLarge; return result; }
        const auto sourceWorkBytes=result.requiredBytes;
        RoiArchive archive;
        archive.sourceKey=sourceKey; archive.source=GetSourceDescriptor(source);
        archive.metadata=descriptor->metadata; archive.nodes=descriptor->definition.nodes;archive.originalRevision=ref;archive.provenance=data->provenance.value_or(DataProvenance{});
        std::map<DataRevisionRef,std::vector<std::uint32_t>> masks;
        std::map<std::shared_ptr<const RoiVoxelSet>,std::vector<std::uint32_t>,std::owner_less<std::shared_ptr<const RoiVoxelSet>>> voxels;
        for (std::size_t i=0;i<archive.nodes.size();++i) {
            auto& p=archive.nodes[i].primitive;
            if (p.mask) { masks[*p.mask].push_back(static_cast<std::uint32_t>(i)); p.mask.reset(); }
            if (p.voxels) {voxels[p.voxels].push_back(static_cast<std::uint32_t>(i));p.voxels.reset();}
        }
        result.requiredBytes=GetArchiveBytes(archive);
        if (!AddBytes(result.requiredBytes,sourceWorkBytes)) { result.error=RoiError::TooLarge; return result; }
        // 在读取/复制任何掩码内容前，先计入全部目标数组和恢复时临时副本的保守预算。
        for (const auto& entry:masks) {
            const auto data=graph.view->GetData(entry.first);
            const auto* mask=data ? dynamic_cast<const BinaryMask3DPayload*>(data->payload.get()):nullptr;
            const auto* labels=data ? dynamic_cast<const LabelMap3DPayload*>(data->payload.get()):nullptr;
            const auto* grid=mask ? &mask->GetGeometry():labels ? &labels->GetGeometry():nullptr;
            if (!grid) { result.error=RoiError::MissingInput; return result; }
            const auto count=GetGridVoxelCount(*grid);
            if (!count || !AddBytes(result.requiredBytes,*count,sizeof(std::uint64_t)*3)
                || !AddBytes(result.requiredBytes,entry.second.size(),sizeof(std::uint32_t)*3)
                || !AddBytes(result.requiredBytes,sizeof(RoiArchiveMask))) {
                result.error=RoiError::TooLarge; return result;
            }
        }
        for (const auto& entry:voxels)
            if (!AddBytes(result.requiredBytes,entry.first->GetVoxels().size(),sizeof(RoiVoxel)*2)) {result.error=RoiError::TooLarge;return result;}
        if (result.requiredBytes==std::numeric_limits<std::size_t>::max() || result.requiredBytes>budget) { result.error=RoiError::TooLarge; return result; }
        for (const auto& entry:masks) {
            const auto data=graph.view->GetData(entry.first);
            const auto* mask=dynamic_cast<const BinaryMask3DPayload*>(data->payload.get());
            const auto* labels=dynamic_cast<const LabelMap3DPayload*>(data->payload.get());
            RoiArchiveMask saved;
            saved.grid=GetGridDescriptor(mask ? mask->GetGeometry():labels->GetGeometry());
            saved.grid.valueType=ImageValueType::UInt64; saved.grid.componentCount=1; saved.grid.componentBytes=8;
            saved.nodeIndices=entry.second;
            if (mask) {
                saved.values.reserve(mask->GetValues()->size());
                for (const auto v:*mask->GetValues()) saved.values.push_back(v);
            } else std::visit([&](const auto& values) {
                saved.values.reserve(values->size());
                for (const auto v:*values) saved.values.push_back(static_cast<std::uint64_t>(v));
            },labels->GetValues());
            archive.masks.push_back(std::move(saved));
        }
        for (const auto& entry:voxels) archive.voxels.push_back({entry.second,entry.first->GetVoxels()});
        result.archive=std::move(archive); result.error=RoiError::None;
    } catch (const std::bad_alloc&) { result.error=RoiError::TooLarge; }
    catch (...) { result.error=RoiError::InvalidRequest; }
    return result;
}

RoiResult RoiService::LoadArchive(const RoiArchive& archive,const std::string& sourceKey,
    const DataRevisionRef& sourceRef,DataBindingRevision expectedCatalogRevision,std::size_t maxBytes)
{
    RoiResult result;
    const auto budget=GetReadBudget(maxBytes);
    result.requiredBytes=GetArchiveBytes(archive);
    if (result.requiredBytes==std::numeric_limits<std::size_t>::max() || result.requiredBytes>budget) { result.error=RoiError::TooLarge; return result; }
    if (sourceKey.empty() || sourceKey!=archive.sourceKey || sourceKey.size()>256) { result.error=RoiError::SourceUnresolved; return result; }
    if (archive.schemaVersion!=roiSchemaVersion || archive.nodes.empty() || archive.nodes.size()>roiNodeLimit
        || archive.masks.size()>roiNodeLimit) { result.error=RoiError::InvalidRequest; return result; }
    try {
        const auto graph=m_store.GetDataGraph();
        const auto source=graph.view->GetData(sourceRef);
        if (!source) { result.error=RoiError::SourceUnresolved; return result; }
        if (!AddBytes(result.requiredBytes,GetSourceWorkBytes(source))
            || result.requiredBytes==std::numeric_limits<std::size_t>::max() || result.requiredBytes>budget) { result.error=RoiError::TooLarge; return result; }
        if (!GetSameSource(archive.source,GetSourceDescriptor(source))) { result.error=RoiError::SourceMismatch; return result; }
        if (archive.source.image && (GetDataRevisionRefValid(archive.source.image->dataRevision)
            || archive.source.image->bindingRevision!=0)) { result.error=RoiError::InvalidRequest; return result; }
        RoiRequest request; request.definition={sourceRef,archive.nodes}; request.metadata=archive.metadata;
        request.expectedCatalogRevision=expectedCatalogRevision;
        for (const auto& node:request.definition.nodes) if (node.primitive.mask || node.primitive.voxels) { result.error=RoiError::InvalidRequest; return result; }
        std::vector<DataRevisionDraft> masks;
        std::set<std::uint32_t> mapped;
        for (const auto& mask:archive.masks) {
            if(mask.sharedPayload){result.error=RoiError::InvalidRequest;return result;}
            if (!archive.source.image || !GetSameGrid(*archive.source.image,mask.grid)
                || mask.grid.valueType!=ImageValueType::UInt64 || mask.grid.componentCount!=1 || mask.grid.componentBytes!=8
                || mask.nodeIndices.empty() || GetDataRevisionRefValid(mask.grid.dataRevision) || mask.grid.bindingRevision!=0) {
                result.error=RoiError::SourceMismatch; return result;
            }
            GridGeometry3D grid{mask.grid.extent,mask.grid.dims,mask.grid.spacing,mask.grid.origin,mask.grid.direction,archive.source.coordinateFrame};
            const auto count=GetGridVoxelCount(grid);
            if (!count || *count!=mask.values.size()) { result.error=RoiError::InvalidGeometry; return result; }
            const DataRevisionRef ref{m_store.CreateDataEntityId(),1};
            for (const auto index:mask.nodeIndices) {
                if (index>=request.definition.nodes.size() || !mapped.insert(index).second
                    || request.definition.nodes[index].kind!=RoiNodeKind::Primitive
                    || request.definition.nodes[index].primitive.shape!=RoiShape::MaskReference) { result.error=RoiError::InvalidRequest; return result; }
                request.definition.nodes[index].primitive.mask=ref;
            }
            masks.push_back({ref.entityId,0,DataTypes::labelMap3D,{{"source-volume",sourceRef}},
                std::make_shared<const LabelMap3DPayload>(grid,std::make_shared<const std::vector<std::uint64_t>>(mask.values)),
                DataProvenance{"Host.Roi","restore-mask","1",{}}});
        }
        for (const auto& saved:archive.voxels) {
            if(saved.sharedPayload){result.error=RoiError::InvalidRequest;return result;}
            auto shared=std::make_shared<const RoiVoxelSet>(saved.values);
            if (saved.nodeIndices.empty()) {result.error=RoiError::InvalidRequest;return result;}
            for (auto index:saved.nodeIndices) {
                if (index>=request.definition.nodes.size() || !mapped.insert(index).second
                    || request.definition.nodes[index].kind!=RoiNodeKind::Primitive
                    || request.definition.nodes[index].primitive.shape!=RoiShape::VoxelSet) {result.error=RoiError::InvalidRequest;return result;}
                request.definition.nodes[index].primitive.voxels=shared;
            }
        }
        const auto requiredBytes=result.requiredBytes;
        result=SetRoiCommit(request,std::move(masks));
        result.requiredBytes=requiredBytes;
    } catch (const std::bad_alloc&) { result.error=RoiError::TooLarge; }
    catch (...) { result.error=RoiError::InvalidRequest; }
    return result;
}

namespace {
std::size_t GetBundleBytes(const RoiArchiveBundle& bundle) noexcept
{
    std::size_t bytes=sizeof(bundle);for(const auto& item:bundle.rois)if(!AddBytes(bytes,GetArchiveBytes(item)))return SIZE_MAX;
    for(const auto& mask:bundle.masks)if(!AddBytes(bytes,mask.values.size(),sizeof(std::uint64_t)*4) || !AddBytes(bytes,mask.nodeIndices.size(),sizeof(std::uint32_t)*3) || !AddBytes(bytes,sizeof(mask)))return SIZE_MAX;
    for(const auto& data:bundle.voxels)if(!AddBytes(bytes,data.values.size(),sizeof(RoiVoxel)*8) || !AddBytes(bytes,data.nodeIndices.size(),sizeof(std::uint32_t)*3) || !AddBytes(bytes,sizeof(data)))return SIZE_MAX;
    return bytes;
}
}
RoiArchiveBundleResult RoiService::GetArchiveBundle(const DataGraphSnapshot& graph,const std::vector<RoiArchiveSelection>& selection,std::size_t maxBytes)
{
    maxBytes=GetReadBudget(maxBytes);RoiArchiveBundleResult result;RoiArchiveBundle bundle;
    try{
        if(selection.empty() || !graph.view){result.error=RoiError::InvalidRequest;return result;}
        std::map<DataRevisionRef,std::uint32_t> masks;
        std::map<std::shared_ptr<const RoiVoxelSet>,std::uint32_t,std::owner_less<std::shared_ptr<const RoiVoxelSet>>> voxels;
        std::set<DataRevisionRef> ids;
        for(const auto& selected:selection){
            if(!ids.insert(selected.roi).second){result.error=RoiError::InvalidRequest;return result;}
            const auto retained=GetBundleBytes(bundle);if(retained>maxBytes){result.error=RoiError::TooLarge;return result;}
            auto exported=GetArchive(graph,selected.roi,selected.sourceKey,maxBytes-retained);if(exported.error!=RoiError::None || !exported.archive){result.error=exported.error;return result;}
            auto item=std::move(*exported.archive);const auto data=graph.view->GetData(selected.roi);const auto& original=dynamic_cast<const RoiGeometryPayload&>(*data->payload).GetDefinition();
            for(auto& mask:item.masks){const auto ref=*original.nodes[mask.nodeIndices.front()].primitive.mask;auto found=masks.find(ref);
                if(found==masks.end()){const auto index=static_cast<std::uint32_t>(bundle.masks.size());masks.emplace(ref,index);RoiArchiveMask payload;payload.grid=mask.grid;payload.values=std::move(mask.values);bundle.masks.push_back(std::move(payload));mask.sharedPayload=index;}
                else{mask.sharedPayload=found->second;mask.values.clear();mask.values.shrink_to_fit();}
            }
            for(auto& saved:item.voxels){const auto shared=original.nodes[saved.nodeIndices.front()].primitive.voxels;auto found=voxels.find(shared);
                if(found==voxels.end()){const auto index=static_cast<std::uint32_t>(bundle.voxels.size());voxels.emplace(shared,index);RoiArchiveVoxels payload;payload.values=std::move(saved.values);bundle.voxels.push_back(std::move(payload));saved.sharedPayload=index;}
                else{saved.sharedPayload=found->second;saved.values.clear();saved.values.shrink_to_fit();}
            }
            bundle.rois.push_back(std::move(item));result.requiredBytes=GetBundleBytes(bundle);
            if(result.requiredBytes==SIZE_MAX || result.requiredBytes>maxBytes){result.error=RoiError::TooLarge;return result;}
        }
        result.archive=std::move(bundle);result.error=RoiError::None;
    }catch(const std::bad_alloc&){result.error=RoiError::TooLarge;}catch(...){result.error=RoiError::InvalidRequest;}return result;
}
RoiArchiveRestoreResult RoiService::LoadArchiveBundle(const RoiArchiveBundle& bundle,const std::vector<RoiSourceMapping>& mappings,std::size_t maxBytes)
{
    maxBytes=GetReadBudget(maxBytes);RoiArchiveRestoreResult result;
    const auto bytes=GetBundleBytes(bundle);if(bytes==SIZE_MAX || bytes>maxBytes){result.error=RoiError::TooLarge;return result;}
    if(bundle.schemaVersion!=roiSchemaVersion || bundle.rois.empty()){result.error=RoiError::InvalidRequest;return result;}
    try{
        const auto graph=m_store.GetDataGraph();std::map<std::string,DataRevisionRef> sources;
        for(const auto& mapping:mappings)if(mapping.sourceKey.empty() || mapping.sourceKey.size()>256 || !GetDataRevisionRefValid(mapping.source) || !sources.emplace(mapping.sourceKey,mapping.source).second){result.error=RoiError::SourceUnresolved;return result;}
        const auto binding=graph.view->GetDataBinding(roiCatalogBinding).value_or(DataBinding{std::string(roiCatalogBinding)});
        const auto catalogData=binding.target?graph.view->GetData(*binding.target):DataSnapshot{};
        const auto* catalog=catalogData?dynamic_cast<const RoiCatalogPayload*>(catalogData->payload.get()):nullptr;
        if(binding.target && !catalog){result.error=RoiError::MissingInput;return result;}
        auto entries=catalog?catalog->GetEntries():std::vector<RoiCatalogEntry>{};DataTransaction tx;
        std::vector<RoiDescriptor> restored;std::vector<std::pair<DataRevisionRef,DataRevisionRef>> revisionMapping;
        std::map<std::pair<std::uint32_t,DataRevisionRef>,DataRevisionRef> maskRefs;
        std::map<std::uint32_t,std::shared_ptr<const RoiVoxelSet>> voxelData;
        std::set<DataRevisionRef> originalIds;
        for(const auto& item:bundle.rois){
            if(item.schemaVersion!=roiSchemaVersion || !GetDataRevisionRefValid(item.originalRevision) || !originalIds.insert(item.originalRevision).second
                || item.metadata.name.empty() || item.metadata.name.size()>256 || item.metadata.group.size()>256 || item.metadata.description.size()>4096){result.error=RoiError::InvalidRequest;return result;}
            const auto mapped=sources.find(item.sourceKey);if(mapped==sources.end()){result.error=RoiError::SourceUnresolved;return result;}
            const auto source=graph.view->GetData(mapped->second);if(!source){result.error=RoiError::SourceUnresolved;return result;}
            if(!GetSameSource(item.source,GetSourceDescriptor(source))){result.error=RoiError::SourceMismatch;return result;}
            RoiDefinition definition{mapped->second,item.nodes};std::set<std::uint32_t> nodes;
            for(const auto& node:definition.nodes)if(node.primitive.mask || node.primitive.voxels){result.error=RoiError::InvalidRequest;return result;}
            for(const auto& mask:item.masks){
                if(!mask.sharedPayload || *mask.sharedPayload>=bundle.masks.size() || !mask.values.empty() || mask.nodeIndices.empty()){result.error=RoiError::InvalidRequest;return result;}
                const auto& payload=bundle.masks[*mask.sharedPayload];
                if(!item.source.image || !GetSameGrid(*item.source.image,payload.grid) || payload.sharedPayload || !payload.nodeIndices.empty()
                    || payload.grid.valueType!=ImageValueType::UInt64 || payload.grid.componentCount!=1 || payload.grid.componentBytes!=8){result.error=RoiError::SourceMismatch;return result;}
                GridGeometry3D grid{payload.grid.extent,payload.grid.dims,payload.grid.spacing,payload.grid.origin,payload.grid.direction,item.source.coordinateFrame};
                const auto count=GetGridVoxelCount(grid);if(!count || *count!=payload.values.size()){result.error=RoiError::InvalidGeometry;return result;}
                const auto key=std::make_pair(*mask.sharedPayload,mapped->second);auto found=maskRefs.find(key);
                if(found==maskRefs.end()){const DataRevisionRef ref{m_store.CreateDataEntityId(),1};found=maskRefs.emplace(key,ref).first;
                    tx.outputs.push_back({ref.entityId,0,DataTypes::labelMap3D,{{"source-volume",mapped->second}},std::make_shared<const LabelMap3DPayload>(grid,std::make_shared<const std::vector<std::uint64_t>>(payload.values)),DataProvenance{"Host.Roi","restore-mask","3",{}}});}
                for(auto index:mask.nodeIndices){if(index>=definition.nodes.size() || !nodes.insert(index).second || definition.nodes[index].kind!=RoiNodeKind::Primitive || definition.nodes[index].primitive.shape!=RoiShape::MaskReference){result.error=RoiError::InvalidRequest;return result;}definition.nodes[index].primitive.mask=found->second;}
            }
            for(const auto& saved:item.voxels){
                if(!saved.sharedPayload || *saved.sharedPayload>=bundle.voxels.size() || !saved.values.empty() || saved.nodeIndices.empty()){result.error=RoiError::InvalidRequest;return result;}
                const auto& payload=bundle.voxels[*saved.sharedPayload];if(payload.sharedPayload || !payload.nodeIndices.empty()){result.error=RoiError::InvalidRequest;return result;}
                auto found=voxelData.find(*saved.sharedPayload);if(found==voxelData.end())found=voxelData.emplace(*saved.sharedPayload,std::make_shared<const RoiVoxelSet>(payload.values)).first;
                for(auto index:saved.nodeIndices){if(index>=definition.nodes.size() || !nodes.insert(index).second || definition.nodes[index].kind!=RoiNodeKind::Primitive || definition.nodes[index].primitive.shape!=RoiShape::VoxelSet){result.error=RoiError::InvalidRequest;return result;}definition.nodes[index].primitive.voxels=found->second;}
            }
            const auto definitionError=RoiEvaluator::GetDefinitionError(definition);if(definitionError!=RoiError::None){result.error=definitionError;return result;}
            const DataRevisionRef ref{m_store.CreateDataEntityId(),1};
            tx.outputs.push_back({ref.entityId,0,DataTypes::roiGeometry,RoiEvaluator::GetInputs(definition),std::make_shared<const RoiGeometryPayload>(definition),item.provenance});
            entries.push_back({ref,item.metadata});restored.push_back({ref,ref,std::move(definition),item.metadata,binding.revision+1});revisionMapping.emplace_back(item.originalRevision,ref);
        }
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.geometry.entityId<b.geometry.entityId;});
        const DataRevisionRef catalogRef{catalogData?catalogData->self.entityId:m_store.CreateDataEntityId(),catalogData?catalogData->self.generation+1:1};
        if(catalogRef.generation==0){result.error=RoiError::TooLarge;return result;}
        std::vector<DataInputRef> inputs;for(std::size_t i=0;i<entries.size();++i)inputs.push_back({"roi-"+std::to_string(i),entries[i].geometry});
        tx.outputs.push_back({catalogRef.entityId,catalogRef.generation-1,DataTypes::roiCatalog,std::move(inputs),std::make_shared<const RoiCatalogPayload>(std::move(entries)),DataProvenance{"Host.Roi","restore-bundle","3",{}}});
        for(auto& output:tx.outputs){for(const auto& input:output.inputs){const auto data=graph.view->GetData(input.source);bool explicitScope=data && data->lifetimePolicy==DataLifetimePolicy::ExplicitRetirement;for(const auto& staged:tx.outputs)if(staged.entityId==input.source.entityId && staged.expectedGeneration+1==input.source.generation && staged.lifetimePolicy==DataLifetimePolicy::ExplicitRetirement)explicitScope=true;if(explicitScope){output.lifetimeScope=m_store.CreateDataEntityId();output.lifetimePolicy=DataLifetimePolicy::ExplicitRetirement;break;}}}
        tx.bindings.push_back({std::string(roiCatalogBinding),binding.revision,true,binding.target,catalogRef});
        const auto committed=m_store.SetDataCommit(std::move(tx));
        if(committed.status!=DataCommitStatus::Succeeded){result.error=committed.failureReason==DataCommitFailure::ExpectationFailed?RoiError::RevisionConflict:RoiError::CommitFailed;result.rois.clear();result.revisionMapping.clear();return result;}
        result.error=RoiError::None;result.commitId=committed.commitId;result.rois=std::move(restored);result.revisionMapping=std::move(revisionMapping);
    }catch(const std::bad_alloc&){result.error=RoiError::TooLarge;result.rois.clear();result.revisionMapping.clear();}catch(...){result.error=RoiError::InvalidRequest;result.rois.clear();result.revisionMapping.clear();}return result;
}
