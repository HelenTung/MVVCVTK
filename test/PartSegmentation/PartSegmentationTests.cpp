#include "PartSegmentationTestCases.h"

#include <iostream>
#include <string_view>

namespace {

int GetSuiteFailCount(const std::string_view suite)
{
    if (suite == "algorithm") return GetPartAlgorithmFailCount();
    if (suite == "lifecycle") return GetPartLifecycleFailCount();
    if (suite == "display") return GetPartDisplayFailCount();
    if (suite == "all") {
        return GetPartAlgorithmFailCount()
            + GetPartLifecycleFailCount()
            + GetPartDisplayFailCount();
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
