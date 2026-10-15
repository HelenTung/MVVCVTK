// 编号、结果身份和范围读取的值语义回归，不依赖私有检测内核。
#include "GapResultReader.h"
#include "MappedArrayStorage.h"
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::shared_ptr<const RecordTablePayload> Table(std::size_t count=2) {
    ResultArrayScope tableStorage(0);
    DataArray<std::int64_t> ids,voxels;DataArray<std::uint64_t> order,firstPoint,points,firstTriangle,triangles,partial,whole;
    DataArray<double> volume;DataArray<std::uint8_t> valid;DataArray<std::array<std::int64_t,6>> boxes;
    for(std::size_t i=0;i<count;++i) {
        ids.push_back(static_cast<std::int64_t>((count-i)*2+1));order.push_back(count-i-1);voxels.push_back(1);
        firstPoint.push_back(count==2?i*3:0);points.push_back(count==2?3:0);
        firstTriangle.push_back(count==2?i:0);triangles.push_back(count==2?1:0);
        partial.push_back(i==0);whole.push_back(i!=0);volume.push_back(1);valid.push_back(1);boxes.push_back({0,1,0,1,0,1});
    }
    auto table=std::make_shared<const RecordTablePayload>(DataTypes::recordTable,"gap-analysis.void-regions",
        std::vector<RecordColumn>{{"void-id",std::move(ids)},{"object-row-by-id",std::move(order)},
            {"voxel-count",std::move(voxels)},{"mesh-first-point",std::move(firstPoint)},
            {"mesh-point-count",std::move(points)},{"mesh-first-triangle",std::move(firstTriangle)},
            {"mesh-triangle-count",std::move(triangles)},{"mesh-range-valid",std::move(valid)},
            {"volume-mm3",volume},{"parent-volume-mm3",volume},{"intersection-volume-mm3",std::move(volume)},
            {"roi-partial",std::move(partial)},{"whole-object-geometry",std::move(whole)},{"voxel-bbox",std::move(boxes)},
            {"equivalent-diameter-mm",DataArray<double>(count,1)}});
    tableStorage.Finish();return table;
}
}
int main() {
    try {
        DataRevisionRef result,source;result.entityId.bytes[0]=1;result.generation=7;source.entityId.bytes[0]=2;source.generation=3;
        GridGeometry3D grid;grid.origin={100,200,300};grid.spacing={2,3,4};
        std::shared_ptr<const GapResultReader> reader;std::weak_ptr<DataArrayStorage> storage;
        {
            ResultArrayScope scope(0);
            DataArray<double> points{0,0,0,1,0,0,0,1,0, 2,0,0,3,0,0,2,1,0};
            storage=points.get_allocator().storage;
            auto mesh=std::make_shared<const SurfaceMeshPayload>(std::move(points),DataArray<std::uint64_t>{0,1,2,3,4,5});
            reader=BuildGapResultReader(result,source,Table(),mesh,grid);scope.Finish();
        }
        Require(reader&&!storage.expired(),"reader did not retain immutable backing after producer exit");
        const auto page=reader->GetObjects(0,1);
        Require(page.error==GapReadError::None&&page.result==result&&page.total==2&&page.next==1
            &&page.objects[0].id==5&&page.objects[0].isRoiPartial,"object paging changed formal table order, ROI flags or revision");
        const auto found=reader->GetObjectById(3);
        Require(found.error==GapReadError::None&&found.object&&found.object->firstPoint==3
            &&found.object->boundsModel==std::array<double,6>{99,103,198.5,204.5,298,306},"ID lookup or physical bounds ignored stored ranges/grid");
        Require(reader->GetObjectById(0).error==GapReadError::NotFound&&reader->GetObjectById(4).error==GapReadError::NotFound,"background or absent ID was accepted");
        const auto geometry=reader->GetGeometry(3,1,0,1);
        Require(geometry.error==GapReadError::None&&geometry.result==result&&geometry.firstPoint==4
            &&geometry.nextPoint==2&&geometry.nextTriangle==1&&geometry.points[0]==std::array<double,3>{3,0,0}
            &&geometry.triangleIndices==std::vector<std::uint64_t>({3,4,5}),"geometry page changed global IDs or copied a different object");
        const auto cancelled=reader->GetGeometry(3,0,0,64,[]{return true;});
        Require(cancelled.error==GapReadError::Cancelled&&cancelled.points.empty()&&cancelled.triangleIndices.empty(),"cancelled read exposed a partial success");
        Require(reader->GetGeometry(3,4,0,1).error==GapReadError::InvalidRequest
            &&reader->GetObjects(3,1).error==GapReadError::InvalidRequest,"out of range read was accepted");
        const auto catalog=reader->CreateObjectReader();
        reader.reset();Require(storage.expired(),"object-only reader kept the unneeded geometry backing alive");
        Require(catalog->GetMetadata().result==result&&catalog->GetObjectById(3).object->equivalentDiameterMM==1
            &&catalog->GetGeometry(3,0,0,1).error==GapReadError::Unavailable,"object-only reader lost identity/values or falsely offered geometry access");
        auto emptyMesh=std::make_shared<const SurfaceMeshPayload>(DataArray<double>{},DataArray<std::uint64_t>{});
        reader=BuildGapResultReader(result,source,Table(5000),emptyMesh,grid);
        const auto bounded=reader->GetObjects(0,SIZE_MAX);
        Require(bounded.error==GapReadError::None&&bounded.objects.size()==4096&&bounded.next==4096,"object page request bypassed the bounded transfer size");
        reader=BuildGapResultReader(result,source,Table(0),emptyMesh,grid);
        Require(reader&&reader->GetObjects(0,1).error==GapReadError::None&&reader->GetObjects(0,1).objects.empty(),"empty complete result cannot be read");
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
