#include "CropBridgeTests.h"
#include "PlanarTestSuites.h"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[])
{
    const std::string_view suite =
        argc > 1 && argv[1] ? argv[1] : "all";
    if (suite == "algorithm") {
        return CropAlgorithmSuite().GetFailCount() == 0
            ? 0 : 1;
    }
    if (suite == "bridge") {
        return CropBridgeSuite().GetFailCount() == 0
            ? 0 : 1;
    }
    if (suite == "shader") {
        return CropShaderPreviewSuite().GetFailCount() == 0
            ? 0 : 1;
    }
    if (suite == "app") {
        return AppTaskSuite().GetFailCount() == 0
            ? 0 : 1;
    }
    if (suite != "all") {
        std::cerr << "Unknown suite: " << suite << '\n';
        return 2;
    }

    int failureCount = CropAlgorithmSuite().GetFailCount();
    failureCount += CropBridgeSuite().GetFailCount();
    failureCount += CropShaderPreviewSuite().GetFailCount();
    failureCount += AppTaskSuite().GetFailCount();

    if (failureCount != 0) {
        std::cerr << "PlanarCropTests failed: " << failureCount << '\n';
        return 1;
    }

    std::cout << "PlanarCropTests passed.\n";
    return 0;
}
