#include "AlignmentVoxelVolume.h"
#include <cstring>
#include <iostream>
#include <numeric>

namespace {
int failures=0;
void Check(bool value,const char *message) { if(!value){++failures;std::cerr<<"FAILED: "<<message<<'\n';} }
DataRevisionRef Ref(unsigned char id=1) { DataRevisionRef r;r.entityId.bytes[0]=id;r.generation=1;return r; }
GridGeometry3D Grid(int x,int y=1,int z=1) { GridGeometry3D g;g.dimensions={x,y,z};g.extent={0,x-1,0,y-1,0,z-1};return g; }
template<class T> std::shared_ptr<const ImageGrid3DPayload> Image(const GridGeometry3D &g,ImageValueType type,const std::vector<T> &v,DataBytes mask={},ImageMetadata metadata={}) {
    auto bytes=std::make_shared<std::vector<std::uint8_t>>(v.size()*sizeof(T));std::memcpy(bytes->data(),v.data(),bytes->size());
    return std::make_shared<const ImageGrid3DPayload>(g,type,1,bytes,std::move(mask),std::array<double,2>{0,0},std::move(metadata));
}
struct Fixture {
    AlignmentWork work;VoxelVolumeWork source;VoxelVolumeRequest request;
    Fixture() { work.cancelled=std::make_shared<std::atomic<bool>>(false);request.id="volume";request.source=Ref();request.storedValueRange=std::array<double,2>{0,100}; }
    VoxelVolumeData Run() { return AlignmentVoxelVolume::Build(work,source,request); }
};
void GeometryAndRoi() {
    Fixture f;auto g=Grid(4,3,2);g.extent={10,13,20,22,30,31};g.spacing={.5,.25,2};g.direction={2,.2,0,0,-1,0,0,0,1};
    std::vector<int> values(24);std::iota(values.begin(),values.end(),0);f.source.image=Image(g,ImageValueType::Int32,values);
    f.request.extent=std::array<int,6>{11,12,21,22,30,31};f.request.storedValueRange=std::array<double,2>{6,21};
    const auto owner=f.source.image->GetValues();const auto result=f.Run();const auto &s=result.statistics;
    Check(result.status==AlignmentStatus::FullyDetermined && s.total==8 && s.selected==6 && s.unselected==2 && s.volumeMm3 &&
        std::abs(*s.volumeMm3-3)<1e-12 && s.touchesRegionBoundary,"nonzero extents, reflection/shear and affine cell volume match analytic selection");
    Check(owner==f.source.image->GetValues(),"voxel volume preserves scalar storage identity");
    f.request.requireEnclosedSelection=true;const auto truncated=f.Run();
    Check(truncated.statistics.validity==VoxelVolumeValidity::TouchesBoundary && !truncated.statistics.volumeMm3,"enclosed selection refuses ROI truncation");
    f.request.storedValueRange=std::array<double,2>{100,200};Check(f.Run().statistics.validity==VoxelVolumeValidity::EmptySelection,"empty object selection is explicit");
    f.request.extent=std::array<int,6>{0,1,0,1,0,1};Check(f.Run().status==AlignmentStatus::InvalidInput,"out-of-source ROI rejected");
    f.request.extent.reset();g.direction={1,0,0,0,0,0,0,0,1};f.source.image=Image(g,ImageValueType::Int32,values);
    Check(f.Run().status==AlignmentStatus::InvalidInput,"singular physical geometry rejected");
}
void QualityAndIntegers() {
    Fixture f;ImageMetadata metadata;metadata.scalar.noData=-999;
    auto mask=std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{1,0,1,1,1,1});
    f.source.image=Image(Grid(6),ImageValueType::Float32,std::vector<float>{.1f,5,NAN,INFINITY,-999,7},mask,metadata);
    const auto r=f.Run();
    Check(r.status==AlignmentStatus::FullyDetermined && r.statistics.selected==2 && r.statistics.masked==1 && r.statistics.noData==1 && r.statistics.nonFinite==2 &&
          r.statistics.observedVolumeMm3==2 && !r.statistics.volumeMm3,"masked, NoData and nonfinite cells remain unknown rather than becoming zero material");
    metadata.scalar.noData=.1;f.source.image=Image(Grid(2),ImageValueType::Float32,std::vector<float>{.1f,.2f},{},metadata);
    Check(f.Run().statistics.noData==1,"Float32 NoData uses stored scalar representation");
    const std::uint64_t exact=9007199254740992ULL;f.source.image=Image(Grid(3),ImageValueType::UInt64,std::vector<std::uint64_t>{exact,exact+1,UINT64_MAX});
    f.request.storedValueRange=std::array<double,2>{static_cast<double>(exact),static_cast<double>(exact)};
    Check(f.Run().statistics.selected==1,"UInt64 thresholds do not round adjacent integer values through double");
    f.request.storedValueRange=std::array<double,2>{0,static_cast<double>(UINT64_MAX)};
    Check(f.Run().statistics.selected==3,"UInt64 upper endpoint clips safely without overflow cast");
    f.source.image=Image(Grid(3),ImageValueType::Int64,std::vector<std::int64_t>{INT64_MIN,0,INT64_MAX});
    f.request.storedValueRange=std::array<double,2>{static_cast<double>(INT64_MIN),-1};Check(f.Run().statistics.selected==1,"Int64 lower endpoint is exact");
}
void LabelsAndLimits() {
    Fixture f;f.request.method=VoxelVolumeMethod::Labels;f.request.storedValueRange.reset();f.request.labels={9007199254740993ULL,UINT64_MAX};
    auto labels=std::make_shared<const std::vector<std::uint64_t>>(std::vector<std::uint64_t>{9007199254740992ULL,9007199254740993ULL,UINT64_MAX});
    f.source.labels=std::make_shared<const LabelMap3DPayload>(Grid(3),LabelMapValues{labels});
    Check(f.Run().statistics.selected==2,"64-bit label identities are matched exactly");
    auto signedLabels=std::make_shared<const std::vector<std::int32_t>>(std::vector<std::int32_t>{-1,0,1,2,2});
    f.source.labels=std::make_shared<const LabelMap3DPayload>(Grid(5),LabelMapValues{signedLabels});f.request.labels={2};
    f.request.validityImage=Ref(2);auto mask=std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{1,1,1,0,1});
    f.source.validityImage=Image(Grid(5),ImageValueType::UInt8,std::vector<std::uint8_t>(5,1),mask);
    const auto r=f.Run();Check(r.statistics.selected==1 && r.statistics.invalidLabels==1 && r.statistics.masked==1 && !r.statistics.volumeMm3,"label quality source and negative labels are explicit");
    f.work.cancelled->store(true);Check(f.Run().status==AlignmentStatus::Cancelled,"voxel scan cancels");f.work.cancelled->store(false);
}
}
int main() { try { GeometryAndRoi();QualityAndIntegers();LabelsAndLimits(); }catch(const std::exception &e){++failures;std::cerr<<e.what()<<'\n';}return failures ? 1 : 0; }
