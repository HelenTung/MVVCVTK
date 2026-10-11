#include "ThicknessCudaPath.h"
#include "ThicknessPositivePath.h"
#include "ThicknessCudaExactSign.h"
#include "ThicknessCudaMath.h"
#include "ThicknessCudaMaterialPath.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace ThicknessCudaPath {
namespace {
struct DeviceField {
    struct Region {std::array<int,6> extent{};int sign=0;};
    std::array<int,6> extent{};
    std::array<std::size_t,3> blocks{};
    std::array<std::size_t,3> strides{};
    double threshold=0;ScalarType scalarType=ScalarType::Unknown;
    const std::uint8_t *values=nullptr,*mask=nullptr;
    const std::int8_t *signs=nullptr,*children=nullptr;
    const std::uint32_t* offsets=nullptr;
    __device__ void check() const {}
    __device__ bool node(const std::array<std::int64_t,3>& index,double& value) const {
        std::size_t offset=0;
        for (unsigned axis=0;axis<3;++axis) {
            if (index[axis]<extent[2*axis] || index[axis]>extent[2*axis+1]) return false;
            offset+=std::size_t(index[axis]-extent[2*axis])*strides[axis];
        }
        return Read(offset,value);
    }
    __device__ bool nodes(const std::array<std::int64_t,3>& cell,std::array<double,8>& valuesOut) const {
        std::size_t base=0;
        for(unsigned axis=0;axis<3;++axis){
            if(cell[axis]<extent[2*axis]||cell[axis]>=extent[2*axis+1])return false;
            base+=std::size_t(cell[axis]-extent[2*axis])*strides[axis];
        }
        for(unsigned corner=0;corner<8;++corner){
            const auto offset=base+((corner&1U)?strides[0]:0)+((corner&2U)?strides[1]:0)+((corner&4U)?strides[2]:0);
            if(!Read(offset,valuesOut[corner]))return false;
        }
        return true;
    }
    __device__ bool Read(std::size_t offset,double& value) const {
        if (mask && !mask[offset]) return false;
        switch (scalarType) {
        case ScalarType::Int8:value=reinterpret_cast<const std::int8_t*>(values)[offset];break;
        case ScalarType::UInt8:value=values[offset];break;
        case ScalarType::Int16:value=reinterpret_cast<const std::int16_t*>(values)[offset];break;
        case ScalarType::UInt16:value=reinterpret_cast<const std::uint16_t*>(values)[offset];break;
        case ScalarType::Int32:value=reinterpret_cast<const std::int32_t*>(values)[offset];break;
        case ScalarType::UInt32:value=reinterpret_cast<const std::uint32_t*>(values)[offset];break;
        case ScalarType::Float32:value=reinterpret_cast<const float*>(values)[offset];break;
        case ScalarType::Float64:value=reinterpret_cast<const double*>(values)[offset];break;
        default:return false;
        }
        return isfinite(value);
    }
    __device__ Region GetUniformRegion(const std::array<std::int64_t,3>& cell) const {
        Region region;if (!signs) return region;
        std::size_t offset=0,stride=1;
        for (unsigned axis=0;axis<3;++axis) {
            if (cell[axis]<extent[2*axis] || cell[axis]>=extent[2*axis+1]) return region;
            const auto block=std::size_t(cell[axis]-extent[2*axis])/8;
            offset+=block*stride;stride*=blocks[axis];
            const auto low=std::int64_t(extent[2*axis])+8*std::int64_t(block);
            region.extent[2*axis]=int(low);
            region.extent[2*axis+1]=int(ThicknessMaterialField::Minimum(low+8,std::int64_t(extent[2*axis+1])));
        }
        region.sign=signs[offset];if (region.sign || !children) return region;
        const auto child=offsets[offset];if (child==0xffffffffU) return region;
        std::array<unsigned,3> keys{};
        for (unsigned level=0,size=4;level<3;++level,size/=2) {
            unsigned key=0;
            for (unsigned axis=0;axis<3;++axis) {
                const auto half=unsigned(cell[axis]-region.extent[2*axis])/size;
                key|=half<<axis;region.extent[2*axis]+=int(half*size);
                region.extent[2*axis+1]=ThicknessMaterialField::Minimum(region.extent[2*axis]+int(size),region.extent[2*axis+1]);
            }
            keys[level]=key;
            const auto node=level==0?keys[0]:level==1?8+keys[0]*8+keys[1]:72+keys[0]*64+keys[1]*8+keys[2];
            region.sign=children[std::size_t(child)*584+node];if (region.sign) break;
        }
        return region;
    }
    __device__ int GetMaterialAt(const std::array<double,3>& point) const {
        using namespace ThicknessMaterialField;
        std::array<std::int64_t,3> cell{};std::array<Interval,3> local{};
        for(unsigned axis=0;axis<3;++axis){
            if(!IsFinite(point[axis])||point[axis]<extent[2*axis]||point[axis]>extent[2*axis+1])return -2;
            cell[axis]=Minimum<std::int64_t>(std::int64_t(Floor(point[axis])),extent[2*axis+1]-1);
            local[axis]=Interval(point[axis])-Interval(double(cell[axis]));
        }
        Interval bounded(-threshold);
        for(unsigned corner=0;corner<8;++corner){auto index=cell;for(unsigned axis=0;axis<3;++axis)index[axis]+=(corner>>axis)&1U;
            double value=0;if(!node(index,value))return -2;Interval weight(1);
            for(unsigned axis=0;axis<3;++axis)weight=weight*(((corner>>axis)&1U)?local[axis]:Interval(1)-local[axis]);
            bounded+=Interval(value)*weight;
        }
        const int sign=GetSign(bounded);if(sign)return sign>0?1:0;
        return ThicknessCudaExactSign::GetSign(*this,point,cell);
    }
};
using Point=std::array<double,3>;
__device__ Point Add(Point a,Point b){return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
__device__ Point Sub(Point a,Point b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
__device__ Point Scale(Point a,double b){return {a[0]*b,a[1]*b,a[2]*b};}
__device__ double Dot(Point a,Point b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
__device__ Point Cross(Point a,Point b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
__device__ double Length(Point a){return ThicknessCudaMath::Length(a);}
__device__ Point Unit(Point a){const auto length=Length(a);return length>0&&isfinite(length)?Scale(a,1/length):Point{};}
__device__ Point Index(Point p,const Params& params){Point r{};for(unsigned a=0;a<3;++a)for(unsigned k=0;k<3;++k)r[a]+=params.direction[k*3+a]*p[k]/params.spacing[a];return r;}
__device__ bool Normal(const DeviceField& field,Point p,const Params& params,Point& normal,bool boundary=false){
    ThicknessCudaMath::Sample sample;const auto index=Index(p,params);
    if(!ThicknessCudaMath::GetSample(field,index,sample))return false;
    normal={};for(unsigned a=0;a<3;++a)for(unsigned k=0;k<3;++k)normal[a]+=params.direction[a*3+k]*sample.gradient[k]/params.spacing[k];
    const auto magnitude=Length(normal);if(!isfinite(magnitude)||!(magnitude>0))return false;
    if(boundary&&fabs(sample.value-field.threshold)>8*params.epsilon*magnitude)return false;
    Point gradient{};
    for(unsigned a=0;a<3;++a){auto before=index,after=index;before[a]-=.5;after[a]+=.5;ThicknessCudaMath::Sample left,right;
        if(!ThicknessCudaMath::GetSample<false>(field,before,left)||!ThicknessCudaMath::GetSample<false>(field,after,right))return false;gradient[a]=right.value-left.value;}
    normal={};for(unsigned a=0;a<3;++a)for(unsigned k=0;k<3;++k)normal[a]+=params.direction[a*3+k]*gradient[k]/params.spacing[k];
    normal=Unit(normal);return Length(normal)>.5;
}
struct Prepared {Point normal{},u{},v{};bool valid=false;};
struct RayValue {Value value{};double missing=ThicknessMaterialField::Infinity();bool fatal=false;};
__device__ RayValue Fatal(){RayValue r;r.fatal=true;return r;}
__global__ void PrepareKernel(DeviceField field,const Measurement* requests,Prepared* prepared,Params params,std::size_t count){
    const auto index=std::size_t(blockIdx.x)*blockDim.x+threadIdx.x;if(index>=count)return;
    Prepared r;if(!Normal(field,requests[index].source,params,r.normal)){prepared[index]=r;return;}
    unsigned axis=0;for(unsigned k=1;k<3;++k)if(fabs(r.normal[k])<fabs(r.normal[axis]))axis=k;
    Point ref{};ref[axis]=1;r.u=Unit(Cross(r.normal,ref));r.v=Cross(r.normal,r.u);r.valid=true;prepared[index]=r;
}
__device__ RayValue MeasureRay(const DeviceField& field,Point p,const Prepared& seed,unsigned di,const Params& params){
    auto direction=seed.normal;
    if(di)direction=Add(Scale(seed.normal,params.coneCos),Scale(Add(Scale(seed.u,params.phiCos[di]),Scale(seed.v,params.phiSin[di])),params.coneSin));
    const auto ray=ThicknessCudaMath::Trace(field,Index(p,params),Index(direction,params),params.maxDistance,
        params.maxBoundaryError+8*params.epsilon,params.epsilon*.25,params.sourceLocal);
    if(ray.status==ThicknessCudaMath::Status::Ambiguous)return Fatal();
    if(ray.status==ThicknessCudaMath::Status::MissingSupport){
        if(!params.sourceLocal||!ray.hasEntry)return Fatal();
        const auto entry=Add(p,Scale(direction,ray.entry)),observed=Add(p,Scale(direction,ray.observedUntil));
        const auto length=Length(Sub(observed,entry));if(!(length>4*params.epsilon))return Fatal();
        const auto support=ThicknessCudaMaterialPath::Get(field,Index(entry,params),Sub(Index(observed,params),Index(entry,params)),length,params.epsilon,0,params.tolerance);
        Point normal{};if(!support.valid||!Normal(field,entry,params,normal,true)||!(Dot(direction,normal)>0))return Fatal();
        const auto certified=fmax(0.,length-support.front-support.back-64*params.epsilon);
        RayValue result;result.missing=certified*Dot(direction,normal)*.5;return result;
    }
    if(ray.status!=ThicknessCudaMath::Status::Found)return {};
    const auto from=Add(p,Scale(direction,ray.entry)),to=Add(p,Scale(direction,ray.exit));Point normal{},opposite{};
    if(!Normal(field,from,params,normal,true)||!Normal(field,to,params,opposite,true)||Dot(direction,normal)<=0||Dot(direction,opposite)>=0)return {};
    const auto delta=Sub(to,from);const auto length=Length(delta);
    const auto support=ThicknessCudaMaterialPath::Get(field,Index(from,params),Sub(Index(to,params),Index(from,params)),length,params.epsilon,params.maxBoundaryError,params.tolerance);
    if(!support.valid)return {};
    const auto difference=Sub(Unit(normal),Unit(opposite));const auto denominator=Dot(difference,difference);
    if(!(denominator>0))return {};const auto value=2*Dot(delta,difference)/denominator;
    RayValue result;if(isfinite(value)&&value>0)result.value={value,true};return result;
}
__global__ void RayKernel(DeviceField field,const Measurement* requests,const Prepared* prepared,RayValue* rays,Params params,std::size_t count){
    const auto index=std::size_t(blockIdx.x)*blockDim.x+threadIdx.x;if(index>=count)return;
    const auto di=unsigned(blockIdx.y)+1;rays[std::size_t(di)*count+index]=prepared[index].valid
        ?MeasureRay(field,requests[index].source,prepared[index],di,params):RayValue{};
}
__global__ void NormalRayKernel(DeviceField field,const Measurement* requests,Prepared* prepared,RayValue* rays,Params params,std::size_t count){
    const auto index=std::size_t(blockIdx.x)*blockDim.x+threadIdx.x;if(index>=count)return;
    const auto result=prepared[index].valid?MeasureRay(field,requests[index].source,prepared[index],0,params):RayValue{};
    rays[index]=result;if(result.fatal)prepared[index].valid=false;
}
__global__ void FinishKernel(const Prepared* prepared,const RayValue* rays,Value* results,Params params,std::size_t count){
    const auto index=std::size_t(blockIdx.x)*blockDim.x+threadIdx.x;if(index>=count)return;
    Value result;if(!prepared[index].valid){results[index]=result;return;}
    double missing=ThicknessMaterialField::Infinity();
    // 保持原方向顺序、失败条件和最小值比较，禁止原子浮点归并。
    for(unsigned di=0;di<params.directionCount;++di){const auto& ray=rays[std::size_t(di)*count+index];
        if(ray.fatal){results[index]={};return;}
        missing=fmin(missing,ray.missing);
        if(ray.value.valid&&(!result.valid||ray.value.thickness<result.thickness))result=ray.value;
    }
    if(result.valid&&result.thickness>missing-64*params.epsilon)result={};results[index]=result;
}
void Check(cudaError_t status) {if (status!=cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));}
class Implementation final : public Client {
    DeviceField m_field;
    std::vector<void*> m_allocations;
    static constexpr std::size_t MeasurementBatch=4096*9;
    Measurement* m_measurements=nullptr;Value* m_values=nullptr;
    Prepared* m_prepared=nullptr;RayValue* m_rays=nullptr;
    cudaStream_t m_stream=nullptr;
    cudaEvent_t m_start=nullptr,m_finish=nullptr;
    bool m_profile=false;std::size_t m_profileRequests=0,m_profileBatches=0;
    double m_kernelMs=0,m_measureMs=0;
    std::chrono::steady_clock::time_point m_lastReport=std::chrono::steady_clock::now();
    void Report()const {
        std::fprintf(stderr,"WallCuda requests=%zu batches=%zu kernelMs=%.3f hostMeasureMs=%.3f\n",
            m_profileRequests,m_profileBatches,m_kernelMs,m_measureMs);
    }
    void* Upload(const void* source,std::size_t bytes) {
        if (!bytes) return nullptr;void* pointer=nullptr;
        Check(cudaMalloc(&pointer,bytes));m_allocations.push_back(pointer);
        Check(cudaMemcpy(pointer,source,bytes,cudaMemcpyHostToDevice));return pointer;
    }
public:
    void Initialize(const Input& input) {
        m_allocations.reserve(10);
        std::size_t free=0,total=0;Check(cudaMemGetInfo(&free,&total));
        const auto bytes=input.valueBytes+input.maskBytes+input.signCount+input.offsetCount*sizeof(std::uint32_t)+input.childBytes;
        if (free<bytes || free-bytes<512U*1024U*1024U) throw std::runtime_error("Insufficient CUDA memory for immutable wall input.");
        m_field.extent=input.extent;m_field.blocks=input.blocks;m_field.threshold=input.threshold;m_field.scalarType=input.scalarType;
        m_field.strides={1,std::size_t(std::int64_t(input.extent[1])-input.extent[0]+1),0};
        m_field.strides[2]=m_field.strides[1]*std::size_t(std::int64_t(input.extent[3])-input.extent[2]+1);
        m_field.values=static_cast<const std::uint8_t*>(Upload(input.values,input.valueBytes));
        m_field.mask=static_cast<const std::uint8_t*>(Upload(input.mask,input.maskBytes));
        m_field.signs=static_cast<const std::int8_t*>(Upload(input.signs,input.signCount));
        m_field.offsets=static_cast<const std::uint32_t*>(Upload(input.offsets,input.offsetCount*sizeof(std::uint32_t)));
        m_field.children=static_cast<const std::int8_t*>(Upload(input.children,input.childBytes));
        Check(cudaStreamCreateWithFlags(&m_stream,cudaStreamNonBlocking));
        const auto profile=std::getenv("MVVCVTK_WALL_PROFILE");m_profile=profile&&profile[0]=='1'&&!profile[1];
        if(m_profile){Check(cudaEventCreate(&m_start));Check(cudaEventCreate(&m_finish));}
        Check(cudaMalloc(&m_measurements,MeasurementBatch*sizeof(Measurement)));m_allocations.push_back(m_measurements);
        Check(cudaMalloc(&m_values,MeasurementBatch*sizeof(Value)));m_allocations.push_back(m_values);
        Check(cudaMalloc(&m_prepared,MeasurementBatch*sizeof(Prepared)));m_allocations.push_back(m_prepared);
        Check(cudaMalloc(&m_rays,MeasurementBatch*17*sizeof(RayValue)));m_allocations.push_back(m_rays);
    }
    ~Implementation() override {
        if(m_profile&&m_profileRequests)Report();
        if(m_start)cudaEventDestroy(m_start);if(m_finish)cudaEventDestroy(m_finish);
        if (m_stream) cudaStreamDestroy(m_stream);for (auto p:m_allocations) cudaFree(p);
    }
    std::vector<Value> Measure(const std::vector<Measurement>& requests,const Params& params,const std::function<void()>& check) override {
        if(!params.directionCount||params.directionCount>17)throw std::runtime_error("Invalid CUDA wall direction count.");
        const auto started=std::chrono::steady_clock::now();
        std::vector<Value> results(requests.size());
        for(std::size_t begin=0;begin<requests.size();begin+=MeasurementBatch){
            check();const auto count=std::min(MeasurementBatch,requests.size()-begin);
            Check(cudaMemcpyAsync(m_measurements,requests.data()+begin,count*sizeof(Measurement),cudaMemcpyHostToDevice,m_stream));
            if(m_profile)Check(cudaEventRecord(m_start,m_stream));
            const unsigned blocks=unsigned((count+63)/64);
            PrepareKernel<<<blocks,64,0,m_stream>>>(m_field,m_measurements,m_prepared,params,count);Check(cudaGetLastError());
            NormalRayKernel<<<blocks,64,0,m_stream>>>(m_field,m_measurements,m_prepared,m_rays,params,count);Check(cudaGetLastError());
            if(params.directionCount>1){
                RayKernel<<<dim3(blocks,params.directionCount-1),64,0,m_stream>>>(m_field,m_measurements,m_prepared,m_rays,params,count);Check(cudaGetLastError());
            }
            FinishKernel<<<blocks,64,0,m_stream>>>(m_prepared,m_rays,m_values,params,count);
            Check(cudaGetLastError());if(m_profile)Check(cudaEventRecord(m_finish,m_stream));
            Check(cudaMemcpyAsync(results.data()+begin,m_values,count*sizeof(Value),cudaMemcpyDeviceToHost,m_stream));
            Check(cudaStreamSynchronize(m_stream));
            if(m_profile){float ms=0;Check(cudaEventElapsedTime(&ms,m_start,m_finish));m_kernelMs+=ms;++m_profileBatches;}
        }
        if(m_profile){m_profileRequests+=requests.size();const auto now=std::chrono::steady_clock::now();
            m_measureMs+=std::chrono::duration<double,std::milli>(now-started).count();
            if(now-m_lastReport>std::chrono::seconds(10)){Report();m_lastReport=now;}}
        return results;
    }
};
}
std::unique_ptr<Client> Create(const Input& input) noexcept {
    try {auto client=std::make_unique<Implementation>();client->Initialize(input);return client;}
    catch (...) {return {};}
}
}
