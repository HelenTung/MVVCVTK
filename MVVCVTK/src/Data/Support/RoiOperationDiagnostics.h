#pragma once
#include <cstddef>
// 仓内定向验证使用的当前线程计数，不属于 API/SPI 或正式 ROI 存储。
namespace RoiOperationDiagnostics {
struct Counters final {
    std::size_t componentComparisons=0,componentSuccessorSteps=0,componentConsumedRuns=0;
    std::size_t clipRows=0,clipRuns=0,clipMaskValues=0,clipCheckpoints=0;
};
#if defined(MVVCVTK_ROI_DIAGNOSTICS)
inline thread_local Counters counters;
inline void Reset(){counters={};}
inline Counters Get(){return counters;}
#endif
}

#if defined(MVVCVTK_ROI_DIAGNOSTICS)
#define MVVCVTK_ROI_COUNT(field, amount) (RoiOperationDiagnostics::counters.field += (amount))
#else
#define MVVCVTK_ROI_COUNT(field, amount) ((void)0)
#endif
