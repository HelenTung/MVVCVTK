#include "QtHostMethodCases.h"

#include <array>
#include <iostream>
#include <string_view>

namespace {

struct MethodCase {
    const char* name;
    int (*getFailCount)();
};

constexpr std::array<MethodCase, 6> methodCases{{
    { "load", &GetLoadFailCount },
    { "view", &GetViewFailCount },
    { "crop", &GetCropFailCount },
    { "gap", &GetGapFailCount },
    { "export", &GetExportFailCount },
    { "lifecycle", &GetLifecycleFailCount }
}};

} // namespace

bool GetCaseResult(bool isExpected, const char* caseName)
{
    std::cout << (isExpected ? "PASS: " : "FAIL: ") << caseName << '\n';
    return isExpected;
}

int main(int argc, char* argv[])
{
    const bool isAllCases = argc == 1;
    const bool isSingleCase =
        argc == 3 && std::string_view(argv[1]) == "--case";
    if (!isAllCases && !isSingleCase) {
        std::cerr
            << "ERROR: use --case "
            << "load|view|crop|gap|export|lifecycle\n";
        return 2;
    }

    const std::string_view selectedCase =
        isSingleCase ? std::string_view(argv[2]) : std::string_view{};
    int failureCount{0};
    bool hasSelectedCase{isAllCases};
    for (const auto& methodCase : methodCases) {
        if (isSingleCase && selectedCase != methodCase.name) {
            continue;
        }
        hasSelectedCase = true;
        failureCount += methodCase.getFailCount();
    }

    if (!hasSelectedCase) {
        std::cerr
            << "ERROR: unknown case '" << selectedCase << "'; use "
            << "load|view|crop|gap|export|lifecycle\n";
        return 2;
    }
    if (failureCount != 0) {
        std::cerr << "Qt Host method cases failed: " << failureCount << '\n';
        return 1;
    }
    std::cout << "Qt Host method cases passed.\n";
    return 0;
}
