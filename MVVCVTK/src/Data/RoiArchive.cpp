#include "Data/RoiService.h"
#include "Data/DataPayloads.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace {
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
        result.coordinateFrame=mesh->GetCoordinateFrame();
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
    if (!a.image) return a.sourceBounds==b.sourceBounds && a.pointCount==b.pointCount && a.triangleCount==b.triangleCount;
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

std::size_t GetArchiveBytes(const RoiArchive& archive) noexcept
{
    std::size_t count=sizeof(RoiArchive);
    if (!AddBytes(count,archive.nodes.size(),sizeof(RoiNode)*3)
        || !AddBytes(count,archive.sourceKey.size(),2)
        || !AddBytes(count,archive.metadata.name.size()+archive.metadata.group.size()+archive.metadata.description.size(),2)
        || !AddBytes(count,archive.source.coordinateFrame.size(),2)) return std::numeric_limits<std::size_t>::max();
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
        if (!AddBytes(count,mask.values.size(),3) || !AddBytes(count,mask.nodeIndices.size(),sizeof(std::uint32_t)*3)
            || !AddBytes(count,sizeof(RoiArchiveMask))) return std::numeric_limits<std::size_t>::max();
    }
    return count;
}
}

RoiArchiveResult RoiService::GetArchive(const DataGraphSnapshot& graph,const DataRevisionRef& ref,
    const std::string& sourceKey,std::size_t maxBytes)
{
    RoiArchiveResult result;
    if (sourceKey.empty() || sourceKey.size()>256) { result.error=RoiError::SourceUnresolved; return result; }
    try {
        const auto descriptor=GetDescriptor(graph,ref);
        if (!descriptor) { result.error=RoiError::MissingInput; return result; }
        const auto source=graph.view->GetData(descriptor->definition.source);
        if (!source) { result.error=RoiError::SourceUnresolved; return result; }
        result.requiredBytes=GetSourceWorkBytes(source);
        if (result.requiredBytes>std::min(maxBytes,roiCopyLimit)) { result.error=RoiError::TooLarge; return result; }
        const auto sourceWorkBytes=result.requiredBytes;
        RoiArchive archive;
        archive.sourceKey=sourceKey; archive.source=GetSourceDescriptor(source);
        archive.metadata=descriptor->metadata; archive.nodes=descriptor->definition.nodes;
        std::map<DataRevisionRef,std::vector<std::uint32_t>> masks;
        for (std::size_t i=0;i<archive.nodes.size();++i) {
            auto& p=archive.nodes[i].primitive;
            if (p.mask) { masks[*p.mask].push_back(static_cast<std::uint32_t>(i)); p.mask.reset(); }
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
            if (!count || !AddBytes(result.requiredBytes,*count,3)
                || !AddBytes(result.requiredBytes,entry.second.size(),sizeof(std::uint32_t)*3)
                || !AddBytes(result.requiredBytes,sizeof(RoiArchiveMask))) {
                result.error=RoiError::TooLarge; return result;
            }
        }
        if (result.requiredBytes>std::min(maxBytes,roiCopyLimit)) { result.error=RoiError::TooLarge; return result; }
        for (const auto& entry:masks) {
            const auto data=graph.view->GetData(entry.first);
            const auto* mask=dynamic_cast<const BinaryMask3DPayload*>(data->payload.get());
            const auto* labels=dynamic_cast<const LabelMap3DPayload*>(data->payload.get());
            RoiArchiveMask saved;
            saved.grid=GetGridDescriptor(mask ? mask->GetGeometry():labels->GetGeometry());
            saved.grid.valueType=ImageValueType::UInt8; saved.grid.componentCount=1; saved.grid.componentBytes=1;
            saved.nodeIndices=entry.second;
            if (mask) {
                saved.values.reserve(mask->GetValues()->size());
                for (const auto v:*mask->GetValues()) saved.values.push_back(v ? 1:0);
            } else std::visit([&](const auto& values) {
                saved.values.reserve(values->size());
                for (const auto v:*values) saved.values.push_back(v ? 1:0);
            },labels->GetValues());
            archive.masks.push_back(std::move(saved));
        }
        result.archive=std::move(archive); result.error=RoiError::None;
    } catch (const std::bad_alloc&) { result.error=RoiError::TooLarge; }
    catch (...) { result.error=RoiError::InvalidRequest; }
    return result;
}

RoiResult RoiService::LoadArchive(const RoiArchive& archive,const std::string& sourceKey,
    const DataRevisionRef& sourceRef,DataBindingRevision expectedCatalogRevision,std::size_t maxBytes)
{
    RoiResult result;
    result.requiredBytes=GetArchiveBytes(archive);
    if (result.requiredBytes>std::min(maxBytes,roiCopyLimit)) { result.error=RoiError::TooLarge; return result; }
    if (sourceKey.empty() || sourceKey!=archive.sourceKey || sourceKey.size()>256) { result.error=RoiError::SourceUnresolved; return result; }
    if (archive.schemaVersion!=roiSchemaVersion || archive.nodes.empty() || archive.nodes.size()>roiNodeLimit
        || archive.masks.size()>roiNodeLimit) { result.error=RoiError::InvalidRequest; return result; }
    try {
        const auto graph=m_store.GetDataGraph();
        const auto source=graph.view->GetData(sourceRef);
        if (!source) { result.error=RoiError::SourceUnresolved; return result; }
        if (!AddBytes(result.requiredBytes,GetSourceWorkBytes(source))
            || result.requiredBytes>std::min(maxBytes,roiCopyLimit)) { result.error=RoiError::TooLarge; return result; }
        if (!GetSameSource(archive.source,GetSourceDescriptor(source))) { result.error=RoiError::SourceMismatch; return result; }
        if (archive.source.image && (GetDataRevisionRefValid(archive.source.image->dataRevision)
            || archive.source.image->bindingRevision!=0)) { result.error=RoiError::InvalidRequest; return result; }
        RoiRequest request; request.definition={sourceRef,archive.nodes}; request.metadata=archive.metadata;
        request.expectedCatalogRevision=expectedCatalogRevision;
        for (const auto& node:request.definition.nodes) if (node.primitive.mask) { result.error=RoiError::InvalidRequest; return result; }
        std::vector<DataRevisionDraft> masks;
        std::set<std::uint32_t> mapped;
        for (const auto& mask:archive.masks) {
            if (!archive.source.image || !GetSameGrid(*archive.source.image,mask.grid)
                || mask.grid.valueType!=ImageValueType::UInt8 || mask.grid.componentCount!=1 || mask.grid.componentBytes!=1
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
            masks.push_back({ref.entityId,0,DataTypes::binaryMask3D,{{"source-volume",sourceRef}},
                std::make_shared<const BinaryMask3DPayload>(grid,std::make_shared<const std::vector<std::uint8_t>>(mask.values)),
                DataProvenance{"Host.Roi","restore-mask","1",{}}});
        }
        const auto requiredBytes=result.requiredBytes;
        result=SetRoiCommit(request,std::move(masks));
        result.requiredBytes=requiredBytes;
    } catch (const std::bad_alloc&) { result.error=RoiError::TooLarge; }
    catch (...) { result.error=RoiError::InvalidRequest; }
    return result;
}
