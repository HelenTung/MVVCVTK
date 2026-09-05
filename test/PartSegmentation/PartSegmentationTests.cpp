#include "PartSegmentationTestCases.h"

#include <iostream>
#include <string_view>

namespace {

int GetSuiteFailCount(const std::string_view suite)
{
    if (suite == "algorithm") return GetPartAlgorithmFailCount();
    if (suite == "catalog") return GetPartCatalogFailCount();
    if (suite == "lifecycle") return GetPartLifecycleFailCount();
    if (suite == "lineage") return GetPartLineageFailCount();
    if (suite == "display") return GetPartDisplayFailCount();
    if (suite == "scale") return GetPartScaleFailCount();
    if (suite == "all") {
        return GetPartAlgorithmFailCount()
            + GetPartCatalogFailCount()
            + GetPartLifecycleFailCount()
            + GetPartLineageFailCount()
            + GetPartDisplayFailCount()
            + GetPartScaleFailCount();
    }
    return 1;
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string_view suite = argc > 1 && argv[1]
        ? std::string_view{ argv[1] }
        : std::string_view{ "all" };
    const int failureCount = GetSuiteFailCount(suite);
    std::cout << "PartSegmentation suite=" << suite
        << " failures=" << failureCount << '\n';
    return failureCount == 0 ? 0 : 1;
}
