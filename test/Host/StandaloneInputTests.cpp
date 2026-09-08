#include "FeatureInput.h"
#include "Host/Types/HostRequestTypes.h"
#include "Data/ImageReadTypes.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
FeatureTestOptions Parse(std::vector<std::string> arguments)
{
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    return GetFeatureTestOptions(static_cast<int>(pointers.size()), pointers.data());
}
} // namespace

int main()
{
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::cout << (passed ? "PASS: " : "FAIL: ") << name << '\n';
        if (!passed) ++failures;
    };
    const std::vector<std::string> valid{
        "test", "--input=scan.raw", "--dimensions=3,2,2", "--spacing=2,3,4",
        "--origin=10,20,30", "--direction=0,-1,0,1,0,0,0,0,1",
        "--input-frame=LPS", "--input-unit=mm", "--input-format=float32-le-xfastest",
        "--dataset-id=scan-1", "--input-digest=" + std::string(64, 'a')
    };
    const auto parsed = Parse(valid);
    const auto request = GetFeatureLoadRequest(parsed, true);
    check(request.geometry.spacing == std::array<float, 3>{2, 3, 4}
        && request.geometry.origin == std::array<float, 3>{10, 20, 30}
        && request.metadata.identity.datasetId == "scan-1"
        && request.metadata.source.digest == "sha256:" + std::string(64, 'a'),
        "Explicit input metadata is preserved in the Host request");
    const auto rejects = [&](std::vector<std::string> arguments) {
        try { (void)GetFeatureLoadRequest(Parse(std::move(arguments)), true); }
        catch (const std::exception&) { return true; }
        return false;
    };
    check(rejects({"test"}), "Missing geometry does not silently use the previous sample");
    for (std::size_t index : std::array<std::size_t, 9>{2, 3, 4, 5, 6, 7, 8, 9, 10}) {
        auto missing = valid;
        missing.erase(missing.begin() + static_cast<std::ptrdiff_t>(index));
        check(rejects(std::move(missing)), "Every required real-audit input field is mandatory");
    }
    for (const std::string bad : {"--spacing=1,0,1", "--spacing=1,nan,1",
        "--spacing=1,1,1,2", "--origin=1e100,0,0", "--direction=1,0",
        "--input-format=uint16", "--input-frame=RAS", "--input-unit=inch",
        "--input-digest=1234", "--dimensions=3,2,2.5"}) {
        auto invalid = valid;
        const auto key = bad.substr(0, bad.find('=') + 1);
        for (auto& argument : invalid) if (argument.compare(0, key.size(), key) == 0) argument = bad;
        check(rejects(std::move(invalid)), "Invalid geometry/storage/provenance is rejected");
    }
    auto duplicate = valid;
    duplicate.push_back("--spacing=1,1,1");
    check(rejects(std::move(duplicate)), "Conflicting duplicate input fields are rejected");
    ImageDescriptor descriptor;
    descriptor.metadata = request.metadata;
    descriptor.metadata.source.byteSize = 3 * 2 * 2 * sizeof(float);
    descriptor.dims = {3, 2, 2};
    descriptor.extent = {0, 2, 0, 1, 0, 1};
    descriptor.spacing = {2, 3, 4};
    descriptor.origin = {-7, -24, 30};
    descriptor.direction = {0, -1, 0, 1, 0, 0, 0, 0, 1};
    descriptor.valueType = ImageValueType::Float32;
    descriptor.componentBytes = 4;
    descriptor.componentCount = 1;
    descriptor.dataRevision = {{1}, 1};
    check(GetFeatureInputValid(request, descriptor), "LPS/RAS corner correspondence respects XY storage reversal");
    descriptor.metadata.source.uri = "another-scan.raw";
    check(!GetFeatureInputValid(request, descriptor), "Wrong source URI cannot pass provenance audit");
    descriptor.metadata.source.uri = request.metadata.source.uri;
    descriptor.metadata.attributes.pop_back();
    check(!GetFeatureInputValid(request, descriptor), "Missing storage provenance cannot pass input audit");
    descriptor.metadata.attributes = request.metadata.attributes;
    --descriptor.metadata.source.byteSize;
    check(!GetFeatureInputValid(request, descriptor), "Wrong input byte size is detected");
    ++descriptor.metadata.source.byteSize;
    descriptor.origin[0] += 1;
    check(!GetFeatureInputValid(request, descriptor), "Wrong physical origin cannot pass real input audit");
    descriptor.origin[0] -= 1;
    descriptor.valueType = ImageValueType::UInt32;
    check(!GetFeatureInputValid(request, descriptor), "Same byte width does not substitute for the scalar type");
    return failures == 0 ? 0 : 1;
}
