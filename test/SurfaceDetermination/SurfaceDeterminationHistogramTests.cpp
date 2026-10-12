// 解析两级灰度体独立验证原估计桶的次数、最近中心边界和手工阈值语义。
#include "SurfaceDeterminationTestCases.h"
#include "SurfaceDeterminationTestSupport.h"
#include "SurfaceDeterminationAlgorithm.h"
#include <numeric>
#include <cmath>
int GetSurfaceHistogramFailCount()
{
    using namespace SurfaceTest;
    Checks checks;
    const auto source=BuildSnapshot({16,16,16},{1,1,1},{0,0,0},{1,0,0,0,1,0,0,0,1},VTK_FLOAT,
        [](const Point3& point){return point[0]<8 ? 0.0 : 1000.0;});
    auto params=GetParams();params.initialIsoValue.reset();
    const auto result=SurfaceDeterminationAlgorithm::BuildSurface(source,params,64U*1024U*1024U,[]{return false;},{});
    checks.Get(result.isoEstimate.has_value(),"two-level volume publishes its original threshold histogram");
    if(result.isoEstimate){
        const auto& estimate=*result.isoEstimate;
        checks.Get(estimate.histogramCounts.front()==2048 && estimate.histogramCounts.back()==2048,"each analytic half volume enters exactly its endpoint center bin");
        checks.Get(std::accumulate(estimate.histogramCounts.begin(),estimate.histogramCounts.end(),std::uint64_t{})==4096 && estimate.sampleCount==4096,"histogram conserves all independently known voxel samples");
        checks.Get(estimate.histogramEdges.front()==0 && estimate.histogramEdges.back()==1000,"diagnostic histogram retains its actual scalar limits");
        const auto step=1000.0/511.0;
        checks.Get(std::abs(estimate.histogramEdges[1]-0.5*step)<1e-12 && std::abs(estimate.histogramEdges[512]-estimate.histogramEdges[511]-0.5*step)<1e-12,"nearest-center endpoint bins retain half width");
        for(std::size_t i=1;i<estimate.histogramEdges.size();++i)checks.Get(estimate.histogramEdges[i]>estimate.histogramEdges[i-1],"histogram boundaries are strictly increasing");
    }
    params.initialIsoValue=500;
    const auto manual=SurfaceDeterminationAlgorithm::BuildSurface(source,params,64U*1024U*1024U,[]{return false;},{});
    checks.Get(!manual.isoEstimate || (manual.isoEstimate->sampleCount==0 && std::accumulate(manual.isoEstimate->histogramCounts.begin(),manual.isoEstimate->histogramCounts.end(),std::uint64_t{})==0),"explicit ISO retains resolved range without fabricating automatic histogram samples");
    return checks.failureCount;
}
