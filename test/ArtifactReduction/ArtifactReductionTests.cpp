#include "ArtifactTestSupport.h"
#include <iostream>

int main(int argc, char** argv)
{
    try {
        Require(argc == 2, "suite required");
        const std::string suite = argv[1];
        if (suite == "native") TestNative();
        else if (suite == "algorithm") TestAlgorithm();
        else if (suite == "lifecycle") TestLifecycle();
        else throw std::runtime_error("unknown suite");
        std::cout << "ArtifactReduction." << suite << " passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
