#include "AlignmentVoxelVolume.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {
void Require(bool v,const char *message) { if(!v)throw std::invalid_argument(message); }
enum class Quality { Valid, Masked, NoData, NonFinite };
template<class T> struct IntegerRange {
    T low{},high{};bool empty=false;
    IntegerRange(double a,double b) {
        a=std::ceil(a);b=std::floor(b);
        const double upper=std::ldexp(1.0,std::numeric_limits<T>::digits),lower=std::is_signed_v<T> ? -upper : 0;
        empty=a>b || a>=upper || b<lower;
        if(!empty) { low=a<=lower ? std::numeric_limits<T>::lowest() : static_cast<T>(a);
            high=b>=upper ? std::numeric_limits<T>::max() : static_cast<T>(b); }
    }
    bool Contains(T value) const { return !empty && value>=low && value<=high; }
};
// 类型分派、mask 指针和 NoData 转换只准备一次；逐体素扫描不分配也不反复转换阈值。
template<class T> struct ImageReader {
    const std::uint8_t *bytes=nullptr,*mask=nullptr;T noData{};bool hasNoData=false,nanNoData=false;
    explicit ImageReader(const ImageGrid3DPayload &image):bytes(image.GetValues()->data()),mask(image.GetValidityMask() ? image.GetValidityMask()->data() : nullptr) {
        const auto &value=image.GetMetadata().scalar.noData;if(!value)return;
        if constexpr(std::is_integral_v<T>) {
            if(!std::isfinite(*value) || std::floor(*value)!=*value)return;
            const IntegerRange<T> range(*value,*value);hasNoData=!range.empty;if(hasNoData)noData=range.low;
        } else {
            nanNoData=std::isnan(*value);if(nanNoData)return;
            if(std::isfinite(*value) && std::abs(*value)>std::numeric_limits<T>::max())return;
            noData=static_cast<T>(*value);hasNoData=true;
        }
    }
    T Load(std::size_t index) const { T value;std::memcpy(&value,bytes+index*sizeof(T),sizeof(T));return value; }
    Quality GetQuality(std::size_t index,T value) const {
        if(mask && !mask[index])return Quality::Masked;
        if(hasNoData && value==noData)return Quality::NoData;
        if constexpr(std::is_floating_point_v<T>) { if(nanNoData && std::isnan(value))return Quality::NoData;if(!std::isfinite(value))return Quality::NonFinite; }
        return Quality::Valid;
    }
};
template<class F> auto Dispatch(ImageValueType type,F &&f) {
    switch(type) {
    case ImageValueType::Int8:return f(std::int8_t{});case ImageValueType::UInt8:return f(std::uint8_t{});
    case ImageValueType::Int16:return f(std::int16_t{});case ImageValueType::UInt16:return f(std::uint16_t{});
    case ImageValueType::Int32:return f(std::int32_t{});case ImageValueType::UInt32:return f(std::uint32_t{});
    case ImageValueType::Int64:return f(std::int64_t{});case ImageValueType::UInt64:return f(std::uint64_t{});
    case ImageValueType::Float32:return f(float{});case ImageValueType::Float64:return f(double{});
    default:throw std::invalid_argument("Unsupported voxel scalar type.");
    }
}
double CellVolume(const GridGeometry3D &g) {
    std::array<double,9> basis{};double scale=0;
    for(std::size_t r=0;r<3;++r)for(std::size_t c=0;c<3;++c) {
        const auto i=3*r+c;basis[i]=g.direction[i]*g.spacing[c];
        Require(std::isfinite(basis[i]),"Voxel basis overflows.");scale=std::max(scale,std::abs(basis[i]));
    }
    Require(scale>0,"Singular voxel geometry.");for(auto &v:basis)v/=scale;
    const double det=basis[0]*(basis[4]*basis[8]-basis[5]*basis[7])-basis[1]*(basis[3]*basis[8]-basis[5]*basis[6])+basis[2]*(basis[3]*basis[7]-basis[4]*basis[6]);
    Require(std::isfinite(det) && std::abs(det)>32*std::numeric_limits<double>::epsilon(),"Singular or numerically ill-conditioned voxel geometry.");
    const double volume=std::exp(std::log(std::abs(det))+3*std::log(scale));
    Require(std::isfinite(volume) && volume>0,"Voxel volume overflows or underflows.");return volume;
}
template<class Visit> void Scan(const AlignmentWork &work,const GridGeometry3D &g,const std::array<int,6> &extent,VoxelVolumeStatistics &s,Visit visit) {
    std::size_t count=0;
    for(std::int64_t z=extent[4];z<=extent[5];++z)for(std::int64_t y=extent[2];y<=extent[3];++y) {
        auto index=(static_cast<std::size_t>(z-g.extent[4])*g.dimensions[1]+static_cast<std::size_t>(y-g.extent[2]))*g.dimensions[0]+static_cast<std::size_t>(extent[0]-static_cast<std::int64_t>(g.extent[0]));
        for(std::int64_t x=extent[0];x<=extent[1];++x,++index) {
            if(count++%4096==0 && AlignmentGeometryFit::GetWorkStatus(work)!=AlignmentStatus::FullyDetermined)throw std::runtime_error("Voxel volume cancelled or deadline reached.");
            ++s.total;const auto outcome=visit(index);
            switch(outcome.first) {
            case Quality::Masked:++s.masked;continue;case Quality::NoData:++s.noData;continue;case Quality::NonFinite:++s.nonFinite;continue;default:break;
            }
            if(outcome.second<0)++s.invalidLabels;
            else if(!outcome.second)++s.unselected;
            else { ++s.selected;s.touchesRegionBoundary=s.touchesRegionBoundary || x==extent[0] || x==extent[1] || y==extent[2] || y==extent[3] || z==extent[4] || z==extent[5]; }
        }
    }
}
}
bool AlignmentVoxelVolume::GetGeometrySame(const GridGeometry3D &a,const GridGeometry3D &b) {
    return a.extent==b.extent && a.dimensions==b.dimensions && a.spacing==b.spacing && a.origin==b.origin && a.direction==b.direction && a.coordinateFrame==b.coordinateFrame;
}
double AlignmentVoxelVolume::GetCellVolumeMm3(const GridGeometry3D &g) { return CellVolume(g); }
bool AlignmentVoxelVolume::GetRequestValid(const VoxelVolumeRequest &r,const AlignmentConfig &config) {
    if(r.id.empty() || r.id.size()>128 || !GetDataRevisionRefValid(r.source) || static_cast<unsigned>(r.method)>1 ||
        (r.validityImage && !GetDataRevisionRefValid(*r.validityImage)))return false;
    if(r.extent)for(int a=0;a<3;++a)if((*r.extent)[2*a]>(*r.extent)[2*a+1])return false;
    if(config.workingBytes && (*config.workingBytes<8192 || r.labels.size()>(*config.workingBytes-8192)/16))return false;
    if(r.method==VoxelVolumeMethod::StoredValueRange)return r.storedValueRange && r.labels.empty() && !r.validityImage &&
        std::isfinite((*r.storedValueRange)[0]) && std::isfinite((*r.storedValueRange)[1]) && (*r.storedValueRange)[0]<=(*r.storedValueRange)[1];
    return !r.storedValueRange && !r.labels.empty() && std::is_sorted(r.labels.begin(),r.labels.end()) &&
        std::adjacent_find(r.labels.begin(),r.labels.end())==r.labels.end();
}
VoxelVolumeData AlignmentVoxelVolume::Build(const AlignmentWork &work,const VoxelVolumeWork &source,const VoxelVolumeRequest &r) {
    VoxelVolumeData result;
    try {
        if(work.config.workingBytes && (*work.config.workingBytes<8192 || r.labels.size()>(*work.config.workingBytes-8192)/16))throw std::length_error("Voxel volume metadata budget exceeded.");
        Require(GetRequestValid(r,work.config),"Invalid voxel volume request.");
        const bool scalar=r.method==VoxelVolumeMethod::StoredValueRange;
        Require(scalar ? source.image && source.image->GetValid() && source.image->GetComponentCount()==1 : source.labels && source.labels->GetValid(),"Voxel volume source type or geometry is invalid.");
        const auto &g=scalar ? source.image->GetGeometry() : source.labels->GetGeometry();
        if(work.config.workingBytes && g.coordinateFrame.size()>(*work.config.workingBytes-8192-r.labels.size()*16)/4)throw std::length_error("Voxel geometry metadata budget exceeded.");
        Require(!source.validityImage || (source.validityImage->GetValid() && source.validityImage->GetComponentCount()==1 && GetGeometrySame(g,source.validityImage->GetGeometry())),"Validity image must use the exact source geometry.");
        Require(bool(source.validityImage)==r.validityImage.has_value(),"Validity source is unavailable.");
        auto &s=result.statistics;s.evaluatedExtent=r.extent.value_or(g.extent);
        for(int a=0;a<3;++a)Require(s.evaluatedExtent[2*a]>=g.extent[2*a] && s.evaluatedExtent[2*a+1]<=g.extent[2*a+1],"Voxel ROI lies outside source extent.");
        s.cellVolumeMm3=CellVolume(g);
        if(scalar) Dispatch(source.image->GetValueType(),[&](auto type) {
            using T=decltype(type);const ImageReader<T> reader(*source.image);const auto range=*r.storedValueRange;
            if constexpr(std::is_integral_v<T>) {
                const IntegerRange<T> interval(range[0],range[1]);
                Scan(work,g,s.evaluatedExtent,s,[&](std::size_t i) {const auto v=reader.Load(i);return std::make_pair(reader.GetQuality(i,v),interval.Contains(v) ? 1 : 0);});
            } else Scan(work,g,s.evaluatedExtent,s,[&](std::size_t i) {const auto v=reader.Load(i);return std::make_pair(reader.GetQuality(i,v),v>=range[0] && v<=range[1] ? 1 : 0);});
        });
        else {
            std::function<Quality(std::size_t)> quality;
            if(source.validityImage)Dispatch(source.validityImage->GetValueType(),[&](auto type) {
                using T=decltype(type);const ImageReader<T> reader(*source.validityImage);quality=[reader](std::size_t i) {return reader.GetQuality(i,reader.Load(i));};
            });
            std::visit([&](const auto &values) {
                Scan(work,g,s.evaluatedExtent,s,[&](std::size_t i) {
                    const auto q=quality ? quality(i) : Quality::Valid;const auto value=(*values)[i];
                    if constexpr(std::is_signed_v<std::decay_t<decltype(value)>>)if(value<0)return std::make_pair(q,-1);
                    return std::make_pair(q,std::binary_search(r.labels.begin(),r.labels.end(),static_cast<std::uint64_t>(value)) ? 1 : 0);
                });
            },source.labels->GetValues());
        }
        s.observedVolumeMm3=static_cast<double>(s.selected)*s.cellVolumeMm3;
        Require(std::isfinite(s.observedVolumeMm3),"Selected cell volume overflows.");
        s.validity=s.masked || s.noData || s.nonFinite || s.invalidLabels ? VoxelVolumeValidity::IncompleteSamples
            : r.requireEnclosedSelection && !s.selected ? VoxelVolumeValidity::EmptySelection
            : r.requireEnclosedSelection && s.touchesRegionBoundary ? VoxelVolumeValidity::TouchesBoundary : VoxelVolumeValidity::Complete;
        if(s.validity==VoxelVolumeValidity::Complete)s.volumeMm3=s.observedVolumeMm3;
        result.status=AlignmentGeometryFit::GetWorkStatus(work);
    } catch(const std::length_error &e) { result.status=AlignmentStatus::BudgetExceeded;result.message=e.what(); }
      catch(const std::bad_alloc &) { result.status=AlignmentStatus::BudgetExceeded; }
      catch(const std::invalid_argument &e) { result.status=AlignmentStatus::InvalidInput;result.message=e.what(); }
      catch(const std::exception &e) { const auto state=AlignmentGeometryFit::GetWorkStatus(work);result.status=state==AlignmentStatus::FullyDetermined ? AlignmentStatus::InternalError : state;result.message=e.what(); }
    return result;
}
