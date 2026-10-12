// 仓内读取接口只能通过公开值类型消费，不能依赖私有候选或算法头。
#include "Host/ArtifactReductionHostFeature.h"
std::optional<std::vector<std::array<double,2>>> ReadArtifactCandidate(const ArtifactReductionHostFeature& feature)
{
    return feature.GetCandidateSamples(1,0,1,16);
}
