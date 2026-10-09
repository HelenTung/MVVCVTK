#pragma once
#include <array>
#include <vector>
struct CudaPathProbe {
    std::array<double,27> nodes{};
    std::array<int,6> extent{0,1,0,1,0,1};
    std::array<double,3> from{0,.5,.5},delta{1,0,0};
    double threshold=0,length=1,epsilon=1e-8,maximumTrim=1,tolerance=1e-9;
};
struct CudaPathProbeResult {bool valid=false;double front=0,back=0;bool valueSampleEquivalent=true;double length=0;};
std::vector<CudaPathProbeResult> RunCudaPathProbes(const std::vector<CudaPathProbe>& probes);
