#include "AlignmentCudaQuery.h"
#include <cuda_runtime.h>
#include <math_constants.h>
#include <cmath>
#include <cfloat>
#include <limits>
#include <new>

namespace {
void CheckCuda(cudaError_t error) {
    if (error == cudaSuccess) return;
    if (error == cudaErrorMemoryAllocation) throw std::bad_alloc();
    throw std::runtime_error(std::string("CUDA triangle query: ") + cudaGetErrorString(error));
}
template<class T> class Buffer final {
public:
    T *data = nullptr;
    ~Buffer() { if (data) cudaFree(data); }
    void Allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::length_error("CUDA buffer byte count overflows.");
        CheckCuda(cudaMalloc(reinterpret_cast<void **>(&data), count * sizeof(T)));
    }
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
};
class Stream final {
public:
    cudaStream_t handle{};
    Stream() { CheckCuda(cudaStreamCreateWithFlags(&handle, cudaStreamNonBlocking)); }
    ~Stream() { cudaStreamDestroy(handle); }
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;
};
struct Vector final { double x,y,z; };
__device__ Vector Add(Vector a,Vector b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
__device__ Vector Sub(Vector a,Vector b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
__device__ Vector Scale(double s,Vector a) { return {s*a.x,s*a.y,s*a.z}; }
__device__ double Dot(Vector a,Vector b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
__device__ Vector Vertex(const double *v,std::uint64_t id) { return {v[3*id],v[3*id+1],v[3*id+2]}; }
__device__ Vector Closest(Vector p,Vector a,Vector b,Vector c,int &error) {
    const auto ab=Sub(b,a),ac=Sub(c,a),ap=Sub(p,a);
    const double d1=Dot(ab,ap),d2=Dot(ac,ap);
    if(d1<=0 && d2<=0)return a;
    const auto bp=Sub(p,b);const double d3=Dot(ab,bp),d4=Dot(ac,bp);
    if(d3>=0 && d4<=d3)return b;
    const double vc=d1*d4-d3*d2;
    if(vc<=0 && d1>=0 && d3<=0)return Add(a,Scale(d1/(d1-d3),ab));
    const auto cp=Sub(p,c);const double d5=Dot(ab,cp),d6=Dot(ac,cp);
    if(d6>=0 && d5<=d6)return c;
    const double vb=d5*d2-d1*d6;
    if(vb<=0 && d2>=0 && d6<=0)return Add(a,Scale(d2/(d2-d6),ac));
    const double va=d3*d6-d5*d4;
    if(va<=0 && d4>=d3 && d5>=d6)
        return Add(b,Scale((d4-d3)/((d4-d3)+(d5-d6)),Sub(c,b)));
    const double denominator=va+vb+vc;
    if(!isfinite(denominator) || denominator<=0) { error=2;return {}; }
    return Add(Add(a,Scale(vb/denominator,ab)),Scale(vc/denominator,ac));
}
__device__ double Bounds(Vector p,const AlignmentCudaNode &node) {
    const double v[3]{p.x,p.y,p.z};double squared=0;
    for(int axis=0;axis<3;++axis) {
        const double d=fmax(fmax(node.bounds[2*axis]-v[axis],v[axis]-node.bounds[2*axis+1]),0.0);
        squared+=d*d;
    }
    return squared;
}
__global__ void QueryKernel(const AlignmentCudaNode *nodes,const std::uint64_t *order,
    const double *vertices,const std::uint64_t *triangles,const double *points,
    std::size_t count,AlignmentCudaHit *hits) {
    const auto index=static_cast<std::size_t>(blockIdx.x)*blockDim.x+threadIdx.x;
    if(index>=count)return;
    const Vector p=Vertex(points,index);
    AlignmentCudaHit hit{};hit.distanceSquared=CUDART_INF;hit.error=1;
    double second=CUDART_INF,scale=fmax(fmax(fabs(p.x),fabs(p.y)),fabs(p.z));
    for(int i=0;i<6;++i)scale=fmax(scale,fabs(nodes[0].bounds[i]));
    // 双精度距离运算的保守舍入邻域，仅用于触发 CPU 验证，不改变业务距离/公差。
    const double roundoff=128*DBL_EPSILON*scale*scale;
    std::uint64_t stack[65]{};unsigned pending=1;
    while(pending) {
        const auto &node=nodes[stack[--pending]];
        if(Bounds(p,node)>hit.distanceSquared+roundoff)continue;
        if(node.count) {
            for(std::uint64_t i=node.first;i<node.first+node.count;++i) {
                const auto id=order[i];
                const auto a=Vertex(vertices,triangles[3*id]),b=Vertex(vertices,triangles[3*id+1]),c=Vertex(vertices,triangles[3*id+2]);
                int error=0;const auto closest=Closest(p,a,b,c,error);
                const auto delta=Sub(p,closest);const double squared=Dot(delta,delta);
                if(error || !isfinite(squared)) { hit.error=2;hits[index]=hit;return; }
                if(hit.error==1 || squared<hit.distanceSquared || (squared==hit.distanceSquared && id<hit.triangle)) {
                    second=fmin(second,hit.distanceSquared);
                    hit={squared,id,0,false};
                } else second=fmin(second,squared);
            }
        } else {
            auto near=node.left,far=node.right;
            if(Bounds(p,nodes[near])>Bounds(p,nodes[far])) { const auto old=near;near=far;far=old; }
            if(pending+2>65) { hit.error=3;hits[index]=hit;return; }
            stack[pending++]=far;stack[pending++]=near;
        }
    }
    hit.requiresVerification=!isfinite(roundoff) || second-hit.distanceSquared<=roundoff;
    hits[index]=hit;
}
}

class AlignmentCudaQuery::Impl final {
public:
    Stream stream;
    Buffer<AlignmentCudaNode> nodes;
    Buffer<std::uint64_t> order,triangles;
    Buffer<double> vertices,points;
    Buffer<AlignmentCudaHit> hits;
    std::size_t capacity;
    template<class T> void Upload(Buffer<T>& destination,const MeshReadArray<T>& source,const std::function<void()>& checkpoint){
        destination.Allocate(source.size());
        for(std::size_t offset=0;offset<source.size();){
            if(checkpoint)checkpoint();auto chunk=source.GetChunk(offset,std::max<std::size_t>(1,1024*1024/sizeof(T)));
            if(chunk.error!=MeshReadError::None)throw std::runtime_error("CUDA mesh source block read failed.");
            CheckCuda(cudaMemcpyAsync(destination.data+offset,chunk.values.data(),chunk.values.size()*sizeof(T),cudaMemcpyHostToDevice,stream.handle));
            // 本批缓冲必须活到上传完成；不再取得全网格连续主机指针。
            CheckCuda(cudaStreamSynchronize(stream.handle));offset=chunk.nextOffset;
        }
        if(checkpoint)checkpoint();
    }
    Impl(const MeshReadArray<AlignmentCudaNode>& nodeData,const MeshReadArray<std::uint64_t>& orderData,
         const MeshReadArray<double>& vertexData,const MeshReadArray<std::uint64_t>& triangleData,
         std::size_t batchCapacity,const std::function<void()>& checkpoint):capacity(batchCapacity){
        if(nodeData.empty() || orderData.empty() || vertexData.empty() || triangleData.empty() || !capacity ||
            capacity>std::numeric_limits<std::size_t>::max()/3)throw std::invalid_argument("Invalid CUDA triangle index or batch capacity.");
        Upload(nodes,nodeData,checkpoint);Upload(order,orderData,checkpoint);Upload(triangles,triangleData,checkpoint);Upload(vertices,vertexData,checkpoint);
        points.Allocate(3*capacity);hits.Allocate(capacity);
    }

};
bool AlignmentCudaQuery::GetAvailable(std::string &reason) {
    int count=0;const auto status=cudaGetDeviceCount(&count);
    if(status!=cudaSuccess || count==0) {
        reason=status==cudaSuccess ? "No CUDA device available." : cudaGetErrorString(status);return false;
    }
    int device=0;const auto selected=cudaGetDevice(&device);
    if(selected!=cudaSuccess) { reason=cudaGetErrorString(selected);return false; }
    cudaDeviceProp properties{};const auto queried=cudaGetDeviceProperties(&properties,device);
    if(queried!=cudaSuccess) { reason=cudaGetErrorString(queried);return false; }
    int runtime=0,driver=0;cudaRuntimeGetVersion(&runtime);cudaDriverGetVersion(&driver);
    reason=std::string(properties.name)+"; runtime="+std::to_string(runtime)+"; driver="+std::to_string(driver);
    return true;
}
AlignmentCudaQuery::AlignmentCudaQuery(MeshReadArray<AlignmentCudaNode> nodes,MeshReadArray<std::uint64_t> order,
    MeshReadArray<double> vertices,MeshReadArray<std::uint64_t> triangles,std::size_t batchCapacity,const std::function<void()>& checkpoint){
    std::string reason;if(!GetAvailable(reason))throw AlignmentCudaUnavailable(reason);
    m_impl=std::make_unique<Impl>(nodes,order,vertices,triangles,batchCapacity,checkpoint);
}
AlignmentCudaQuery::~AlignmentCudaQuery() = default;
void AlignmentCudaQuery::Query(const double *points,std::size_t count,AlignmentCudaHit *hits) {
    if(count>m_impl->capacity || !points || !hits)throw std::invalid_argument("Invalid CUDA query batch.");
    if(!count)return;
    CheckCuda(cudaMemcpyAsync(m_impl->points.data,points,3*count*sizeof(double),cudaMemcpyHostToDevice,m_impl->stream.handle));
    // 线程块大小只组织执行，不限制业务点数；外层可连续提交任意数量的批次。
    QueryKernel<<<static_cast<unsigned>((count+127)/128),128,0,m_impl->stream.handle>>>(m_impl->nodes.data,
        m_impl->order.data,m_impl->vertices.data,m_impl->triangles.data,m_impl->points.data,count,m_impl->hits.data);
    CheckCuda(cudaGetLastError());
    CheckCuda(cudaMemcpyAsync(hits,m_impl->hits.data,count*sizeof(AlignmentCudaHit),cudaMemcpyDeviceToHost,m_impl->stream.handle));
    CheckCuda(cudaStreamSynchronize(m_impl->stream.handle));
}
