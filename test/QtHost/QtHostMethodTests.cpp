#include "QtHostMethodCases.h"

#include <array>
#include <cstddef>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <crtdbg.h>
#include <windows.h>
#endif

namespace {

std::string methodExecutable;

struct MethodCase {
    const char* name;
    int (*getFailCount)();
};

constexpr std::size_t methodCaseCount =
    4
#if defined(MVVCVTK_HAS_ORTHOGONAL_CROP)
    + 1
#endif
#if defined(MVVCVTK_HAS_GAP_ANALYSIS)
    + 1
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    + 1
#endif
    ;

constexpr std::array<MethodCase, methodCaseCount> methodCases{{
    { "load", &GetLoadFailCount },
    { "view", &GetViewFailCount },
#if defined(MVVCVTK_HAS_ORTHOGONAL_CROP)
    { "crop", &GetCropFailCount },
#endif
#if defined(MVVCVTK_HAS_GAP_ANALYSIS)
    { "gap", &GetGapFailCount },
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    { "part-scene", &GetPartSceneFailCount },
#endif
    { "export", &GetExportFailCount },
    { "lifecycle", &GetLifecycleFailCount }
}};

constexpr std::string_view methodCaseNames =
    "load|view"
#if defined(MVVCVTK_HAS_ORTHOGONAL_CROP)
    "|crop"
#endif
#if defined(MVVCVTK_HAS_GAP_ANALYSIS)
    "|gap"
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    "|part-scene"
#endif
    "|export|lifecycle";

} // namespace

const std::string& GetMethodExecutable()
{
    return methodExecutable;
}

void SetMethodExecutable(std::string executable)
{
    methodExecutable = std::move(executable);
}

bool GetCaseResult(bool isExpected, const char* caseName)
{
    std::cout << (isExpected ? "PASS: " : "FAIL: ") << caseName << '\n';
    return isExpected;
}

int main(int argc, char* argv[])
{
    SetMethodExecutable(
        argc > 0 && argv[0] ? argv[0] : "");
    if (argc == 3
        && std::string_view(argv[1]) == "--death") {
#ifdef _WIN32
        SetErrorMode(
            SEM_FAILCRITICALERRORS
            | SEM_NOGPFAULTERRORBOX);
        _set_abort_behavior(
            0,
            _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _CrtSetReportMode(
            _CRT_WARN,
            _CRTDBG_MODE_FILE);
        _CrtSetReportMode(
            _CRT_ERROR,
            _CRTDBG_MODE_FILE);
        _CrtSetReportMode(
            _CRT_ASSERT,
            _CRTDBG_MODE_FILE);
#endif
        std::signal(SIGABRT, [](int) {
            std::_Exit(86);
        });
        std::set_terminate([]() {
            std::_Exit(86);
        });
        return StartLifecycleDeathCase(argv[2]);
    }

    const bool isAllCases = argc == 1;
    const bool isSingleCase =
        argc == 3 && std::string_view(argv[1]) == "--case";
    if (!isAllCases && !isSingleCase) {
        std::cerr
            << "ERROR: use --case "
            << methodCaseNames << '\n';
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
            << methodCaseNames << '\n';
        return 2;
    }
    if (failureCount != 0) {
        std::cerr << "Qt Host method cases failed: " << failureCount << '\n';
        return 1;
    }
    std::cout << "Qt Host method cases passed.\n";
    return 0;
}
