#include "GapResultReader.h"
#include <algorithm>
#include <limits>
#include <cmath>

namespace {
class Reader final : public GapResultReader {
public:
    Reader(DataRevisionRef result,DataRevisionRef source,std::shared_ptr<const RecordTablePayload> table,
        std::shared_ptr<const SurfaceMeshPayload> mesh,GridGeometry3D grid)
        : m_table(std::move(table)),m_mesh(std::move(mesh)),m_grid(std::move(grid)) {
        if(m_table)m_table=std::dynamic_pointer_cast<const RecordTablePayload>(m_table->CreateSnapshot());
        if(m_mesh)m_mesh=std::dynamic_pointer_cast<const SurfaceMeshPayload>(m_mesh->CreateSnapshot());
        if(!m_table || !m_mesh || !m_table->GetValid() || !m_mesh->GetValid())throw GapReadError::Unavailable;
        m_metadata={result,source,m_mesh->GetCoordinateFrame(),m_table->GetRowCount(),
            m_mesh->GetVertices().size()/3,m_mesh->GetTriangles().size()/3};
        m_ids=&Column<std::int64_t>("void-id");m_order=&Column<std::uint64_t>("object-row-by-id");
        m_firstPoint=&Column<std::uint64_t>("mesh-first-point");m_pointCount=&Column<std::uint64_t>("mesh-point-count");
        m_firstTriangle=&Column<std::uint64_t>("mesh-first-triangle");m_triangleCount=&Column<std::uint64_t>("mesh-triangle-count");
        m_hasGeometry=&Column<std::uint8_t>("mesh-range-valid");
    }
    const GapResultDescriptor& GetMetadata()const noexcept override {return m_metadata;}
    std::shared_ptr<const GapResultReader> CreateObjectReader()const override {
        auto reader=std::make_shared<Reader>(*this);reader->m_mesh.reset();return reader;
    }
    GapObjectPage GetObjects(std::uint64_t offset,std::size_t count,const std::function<bool()>& cancelled)const override {
        GapObjectPage page;page.result=m_metadata.result;page.total=m_metadata.objectCount;
        try {
            if(!count || offset>page.total)throw GapReadError::InvalidRequest;
            Check(cancelled);page.next=offset+std::min<std::uint64_t>({count,4096,page.total-offset});
            page.objects.reserve(static_cast<std::size_t>(page.next-offset));
            for(auto row=offset;row<page.next;++row){if(!(row%4096))Check(cancelled);page.objects.push_back(ObjectAt(row));}
            Check(cancelled);page.error=GapReadError::None;
        }catch(GapReadError error){page.error=error;page.objects.clear();}catch(...){page.error=GapReadError::Failed;page.objects.clear();}
        return page;
    }
    GapObjectReadResult GetObjectById(std::int32_t id)const override {
        GapObjectReadResult result;result.result=m_metadata.result;
        try {result.object=ObjectAt(Find(id));result.error=GapReadError::None;}
        catch(GapReadError error){result.error=error;}catch(...){result.error=GapReadError::Failed;}
        return result;
    }
    GapGeometryChunk GetGeometry(std::int32_t id,std::uint64_t pointOffset,std::uint64_t triangleOffset,
        std::size_t count,const std::function<bool()>& cancelled)const override {
        GapGeometryChunk chunk;chunk.result=m_metadata.result;chunk.objectId=id;
        try {
            Check(cancelled);if(!m_mesh)throw GapReadError::Unavailable;
            const auto row=Find(id);const auto object=ObjectAt(row);
            if(!(*m_hasGeometry)[row])throw GapReadError::Unavailable;
            if(!count || pointOffset>object.pointCount || triangleOffset>object.triangleCount)throw GapReadError::InvalidRequest;
            chunk.totalPoints=object.pointCount;chunk.totalTriangles=object.triangleCount;
            chunk.firstPoint=object.firstPoint+pointOffset;chunk.firstTriangle=object.firstTriangle+triangleOffset;
            chunk.nextPoint=pointOffset+std::min<std::uint64_t>({count,65536,object.pointCount-pointOffset});
            chunk.nextTriangle=triangleOffset+std::min<std::uint64_t>({count,65536,object.triangleCount-triangleOffset});
            chunk.points.reserve(static_cast<std::size_t>(chunk.nextPoint-pointOffset));
            for(auto i=chunk.firstPoint;i<object.firstPoint+chunk.nextPoint;++i) {
                if(!(i%4096))Check(cancelled);const auto& points=m_mesh->GetVertices();
                chunk.points.push_back({points[i*3],points[i*3+1],points[i*3+2]});
            }
            const auto& triangles=m_mesh->GetTriangles();
            const auto first=chunk.firstTriangle*3,last=(object.firstTriangle+chunk.nextTriangle)*3;
            chunk.triangleIndices.reserve(static_cast<std::size_t>(last-first));
            for(auto i=first;i<last;++i) {
                if(!(i%4096))Check(cancelled);
                chunk.triangleIndices.push_back(triangles[static_cast<std::size_t>(i)]);
            }
            Check(cancelled);chunk.error=GapReadError::None;
        }catch(GapReadError error){chunk.error=error;chunk.points.clear();chunk.triangleIndices.clear();}
        catch(...){chunk.error=GapReadError::Failed;chunk.points.clear();chunk.triangleIndices.clear();}
        return chunk;
    }
private:
    static void Check(const std::function<bool()>& cancelled) {
        try{if(cancelled && cancelled())throw GapReadError::Cancelled;}catch(...){throw GapReadError::Cancelled;}
    }
    template<class T> const DataArray<T>& Column(const char* name)const {
        for(const auto& column:m_table->GetColumns())if(column.name==name)
            if(const auto* values=std::get_if<DataArray<T>>(&column.values))return *values;
        throw GapReadError::Unavailable;
    }
    std::uint64_t Find(std::int32_t id)const {
        if(id<=0)throw GapReadError::NotFound;
        std::uint64_t first=0,last=m_metadata.objectCount;
        while(first<last) {
            const auto middle=first+(last-first)/2,row=(*m_order)[middle];
            if(row>=m_metadata.objectCount)throw GapReadError::Failed;
            if((*m_ids)[row]<id)first=middle+1;else last=middle;
        }
        if(first==m_metadata.objectCount)throw GapReadError::NotFound;
        const auto row=(*m_order)[first];
        if(row>=m_metadata.objectCount)throw GapReadError::Failed;
        if((*m_ids)[row]!=id)throw GapReadError::NotFound;
        return row;
    }
    GapObjectRecord ObjectAt(std::uint64_t row)const {
        if(row>=m_metadata.objectCount)throw GapReadError::InvalidRequest;
        const auto id=(*m_ids)[row];if(id<=0 || id>INT32_MAX)throw GapReadError::Failed;
        GapObjectRecord object;object.id=static_cast<std::int32_t>(id);
        const auto voxels=Column<std::int64_t>("voxel-count")[row];if(voxels<0)throw GapReadError::Failed;
        object.voxelCount=static_cast<std::uint64_t>(voxels);
        object.volumeMM3=Column<double>("volume-mm3")[row];object.parentVolumeMM3=Column<double>("parent-volume-mm3")[row];
        object.equivalentDiameterMM=Column<double>("equivalent-diameter-mm")[row];
        object.intersectionVolumeMM3=Column<double>("intersection-volume-mm3")[row];
        object.isRoiPartial=Column<std::uint64_t>("roi-partial")[row]!=0;
        object.hasWholeObjectGeometry=Column<std::uint64_t>("whole-object-geometry")[row]!=0;
        object.firstPoint=(*m_firstPoint)[row];object.pointCount=(*m_pointCount)[row];
        object.firstTriangle=(*m_firstTriangle)[row];object.triangleCount=(*m_triangleCount)[row];
        if(object.firstPoint>m_metadata.pointCount || object.pointCount>m_metadata.pointCount-object.firstPoint
            || object.firstTriangle>m_metadata.triangleCount || object.triangleCount>m_metadata.triangleCount-object.firstTriangle)
            throw GapReadError::Failed;
        const auto& bounds=Column<std::array<std::int64_t,6>>("voxel-bbox")[row];
        object.boundsModel={INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
        for(int corner=0;corner<8;++corner) {
            auto p=m_grid.origin;
            for(int r=0;r<3;++r)for(int c=0;c<3;++c) {
                const double index=static_cast<double>(bounds[c*2+((corner>>c)&1)])+(((corner>>c)&1)?0.5:-0.5);
                p[r]+=m_grid.direction[r*3+c]*m_grid.spacing[c]*index;
            }
            for(int a=0;a<3;++a){object.boundsModel[a*2]=std::min(object.boundsModel[a*2],p[a]);object.boundsModel[a*2+1]=std::max(object.boundsModel[a*2+1],p[a]);}
        }
        return object;
    }
    GapResultDescriptor m_metadata;
    std::shared_ptr<const RecordTablePayload> m_table;
    std::shared_ptr<const SurfaceMeshPayload> m_mesh;
    GridGeometry3D m_grid;
    const DataArray<std::int64_t>* m_ids=nullptr;
    const DataArray<std::uint64_t> *m_order=nullptr,*m_firstPoint=nullptr,*m_pointCount=nullptr,*m_firstTriangle=nullptr,*m_triangleCount=nullptr;
    const DataArray<std::uint8_t>* m_hasGeometry=nullptr;
};
}
std::shared_ptr<const GapResultReader> BuildGapResultReader(DataRevisionRef result,DataRevisionRef source,
    std::shared_ptr<const RecordTablePayload> table,std::shared_ptr<const SurfaceMeshPayload> mesh,GridGeometry3D geometry) {
    try{return std::make_shared<const Reader>(result,source,std::move(table),std::move(mesh),std::move(geometry));}
    catch(...){return {};}
}
