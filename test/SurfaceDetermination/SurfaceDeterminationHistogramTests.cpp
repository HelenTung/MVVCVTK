// 独立解析分布验证直方图计数、峰位、有效域、工作空间和取消。
#include "SurfaceDeterminationTestCases.h"
#include "SurfaceDeterminationTestSupport.h"
#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceHistogramEstimator.h"
#include <numeric>
#include <cmath>
#include <limits>
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

    std::vector<double> maskedValues(4096);
    std::fill(maskedValues.begin()+2048,maskedValues.end(),1000);
    std::vector<unsigned char> validity(4096,1);
    std::fill(validity.begin(),validity.begin()+100,0);
    maskedValues[200]=std::numeric_limits<double>::quiet_NaN();
    maskedValues[300]=std::numeric_limits<double>::infinity();
    const auto readValue=[](const void* data,std::size_t index){return static_cast<const double*>(data)[index];};
    const SurfaceHistogramInput masked{{16,16,16},maskedValues.data(),readValue,validity.data()};
    std::optional<SurfaceIsoEstimate> maskedEstimate;
    std::string maskedMessage;
    const auto maskedFailure=SurfaceHistogramEstimator::BuildEstimate(masked,1024U*1024U,
        []{return false;},maskedEstimate,maskedMessage);
    checks.Get(maskedFailure==SurfaceFailureReason::None && maskedEstimate
        && maskedEstimate->histogramCounts.front()==1946 && maskedEstimate->histogramCounts.back()==2048
        && maskedEstimate->sampleCount==3994 && maskedEstimate->excludedSampleCount==2,
        "invalid mask samples are omitted while non-finite valid samples are explicitly excluded");

    std::vector<double> largeValues(4096,1.0e308);
    std::fill(largeValues.begin()+2048,largeValues.end(),1.1e308);
    const SurfaceHistogramInput large{{16,16,16},largeValues.data(),readValue,nullptr};
    std::optional<SurfaceIsoEstimate> largeEstimate;
    checks.Get(SurfaceHistogramEstimator::BuildEstimate(large,1024U*1024U,[]{return false;},largeEstimate,maskedMessage)
        ==SurfaceFailureReason::None && largeEstimate
        && std::abs(largeEstimate->isoValue-1.05e308)<=4*std::numeric_limits<double>::epsilon()*1.05e308,
        "a finite ISO midpoint does not overflow when the sum of the two finite peaks would overflow");

    // 独立解析密度构造等权、同方差且关于零对称的双高斯计数。
    // 均值间距为 2.56 倍标准差，有两个模式而谷很浅；ISO50 由对称性确定为零。
    constexpr std::size_t sampleCount = 128U*128U*128U;
    const auto buildValues=[](const double offset) {
        std::array<double,512> density{};
        std::array<std::size_t,512> population{};
        for (std::size_t i=0; i<density.size(); ++i) {
            const auto gray=-12.0+24.0*static_cast<double>(i)/(density.size()-1);
            const auto left=(gray+offset)/2.5, right=(gray-offset)/2.5;
            density[i]=std::exp(-0.5*left*left)+std::exp(-0.5*right*right);
        }
        const auto totalDensity=std::accumulate(density.begin(),density.end(),0.0);
        // 均匀底数保持固定范围，不改变峰数或对称性；余量按解析密度分配。
        const auto remaining=sampleCount-17*population.size();
        for (std::size_t i=0; i<population.size()/2; ++i)
            population[i]=population[population.size()-1-i]=17+
                static_cast<std::size_t>(remaining*density[i]/totalDensity);
        auto assigned=std::accumulate(population.begin(),population.end(),std::size_t{});
        for (std::size_t i=0; assigned<sampleCount; ++i,assigned+=2) {
            ++population[i%(population.size()/2)];
            ++population[population.size()-1-i%(population.size()/2)];
        }
        std::vector<double> values;
        values.reserve(sampleCount);
        for (std::size_t i=0; i<population.size(); ++i)
            values.insert(values.end(),population[i],-12.0+24.0*static_cast<double>(i)/(population.size()-1));
        return values;
    };
    const auto values=buildValues(3.2);
    const SurfaceHistogramInput overlapInput{{128,128,128},values.data(),
        [](const void* data,std::size_t index){return static_cast<const double*>(data)[index];},nullptr};
    std::optional<SurfaceIsoEstimate> overlapEstimate;
    std::string message;
    const auto overlapFailure=SurfaceHistogramEstimator::BuildEstimate(
        overlapInput,1024U*1024U,[]{return false;},overlapEstimate,message);
    checks.Get(overlapFailure==SurfaceFailureReason::None && overlapEstimate,
        "two resolved modes with a shallow valley are not rejected solely by a fixed valley ratio");
    if (overlapEstimate) {
        const auto& e=*overlapEstimate;
        const auto binWidth=(e.histogramEdges.back()-e.histogramEdges.front())/(e.histogramCounts.size()-1);
        checks.Get(e.backgroundValue<0 && e.materialValue>0 && std::abs(e.isoValue)<=0.5*binWidth,
            "equal symmetric populations retain the independently known ISO50 midpoint");
    }
    const auto unimodalValues=buildValues(0);
    const SurfaceHistogramInput unimodal{{128,128,128},unimodalValues.data(),overlapInput.readScalar,nullptr};
    auto unimodalEstimate=overlapEstimate;
    checks.Get(SurfaceHistogramEstimator::BuildEstimate(unimodal,1024U*1024U,
        []{return false;},unimodalEstimate,message)==SurfaceFailureReason::ThresholdUnreliable && !unimodalEstimate,
        "a smooth single population is not split merely because its sample count is large");
    auto rejectedEstimate=overlapEstimate;
    checks.Get(SurfaceHistogramEstimator::BuildEstimate(overlapInput,
        SurfaceHistogramEstimator::GetWorkingBytes()-1,[]{return false;},rejectedEstimate,message)
        ==SurfaceFailureReason::BudgetExceeded && !rejectedEstimate,
        "fine histogram workspace is admitted before allocation and cannot retain a stale estimate");
    struct ReadAudit { const std::vector<double>* values; mutable std::size_t reads=0; } readAudit{&values};
    const SurfaceHistogramInput interrupted{{128,128,128},&readAudit,
        [](const void* data,std::size_t index) {
            const auto* audit=static_cast<const ReadAudit*>(data);
            ++audit->reads;
            return (*audit->values)[index];
        },nullptr};
    // 只在读取统计后半段注入取消，覆盖细化扫描尚未完成时不发布半份估计。
    checks.Get(SurfaceHistogramEstimator::BuildEstimate(interrupted,1024U*1024U,
        [&]{return readAudit.reads>values.size()+values.size()/2;},rejectedEstimate,message)
        ==SurfaceFailureReason::Cancelled && !rejectedEstimate,
        "cancellation during full support collection publishes no partial or stale histogram estimate");
    return checks.failureCount;
}
