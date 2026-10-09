#include "ThicknessCudaProbe.h"
#include "ThicknessPositivePath.h"
#include "ThicknessCudaExactSign.h"
#include "ThicknessCudaMaterialPath.h"
#include "ThicknessCudaMath.h"
#include <cuda_runtime.h>
#include <stdexcept>
namespace {
struct Field {
    struct Region {std::array<int,6> extent{};int sign=0;};
    std::array<int,6> extent{};double threshold=0;const double* values=nullptr;
    __device__ void check()const{}
    __device__ Region GetUniformRegion(const std::array<std::int64_t,3>&)const{return {};}
    __device__ bool node(const std::array<std::int64_t,3>& p,double& value)const {
        std::size_t offset=0,stride=1;
        for(unsigned a=0;a<3;++a){if(p[a]<extent[2*a]||p[a]>extent[2*a+1])return false;
            offset+=std::size_t(p[a]-extent[2*a])*stride;stride*=std::size_t(extent[2*a+1]-extent[2*a]+1);}
        value=values[offset];return isfinite(value);
    }
};
__global__ void Probe(const CudaPathProbe* input,CudaPathProbeResult* output,std::size_t count) {
    const auto i=std::size_t(blockIdx.x)*blockDim.x+threadIdx.x;if(i>=count)return;
    const auto& p=input[i];Field field{p.extent,p.threshold,p.nodes.data()};
    const auto result=ThicknessCudaMaterialPath::Get(field,p.from,p.delta,p.length,p.epsilon,p.maximumTrim,p.tolerance);
    output[i]={result.valid,result.front,result.back};
    ThicknessCudaMath::Sample before,after;
    const auto original=ThicknessCudaMath::GetSample(field,p.from,before);
    const auto valueOnly=ThicknessCudaMath::GetSample<false>(field,p.from,after);
    output[i].valueSampleEquivalent=original==valueOnly && (!original || __double_as_longlong(before.value)==__double_as_longlong(after.value));
    output[i].length=ThicknessCudaMath::Length(p.delta);
}
void Check(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
}
std::vector<CudaPathProbeResult> RunCudaPathProbes(const std::vector<CudaPathProbe>& input) {
    std::vector<CudaPathProbeResult> output(input.size());CudaPathProbe* deviceInput=nullptr;CudaPathProbeResult* deviceOutput=nullptr;
    try{Check(cudaMalloc(&deviceInput,input.size()*sizeof(CudaPathProbe)));Check(cudaMalloc(&deviceOutput,input.size()*sizeof(CudaPathProbeResult)));
        Check(cudaMemcpy(deviceInput,input.data(),input.size()*sizeof(CudaPathProbe),cudaMemcpyHostToDevice));
        Probe<<<unsigned((input.size()+31)/32),32>>>(deviceInput,deviceOutput,input.size());Check(cudaGetLastError());
        Check(cudaMemcpy(output.data(),deviceOutput,output.size()*sizeof(CudaPathProbeResult),cudaMemcpyDeviceToHost));
    }catch(...){cudaFree(deviceInput);cudaFree(deviceOutput);throw;}
    cudaFree(deviceInput);cudaFree(deviceOutput);return output;
}
