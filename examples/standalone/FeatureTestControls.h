#pragma once

#include "Host/HostFeature.h"
#include <array>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class VtkAppHostSession;
struct HostLoadRequest;
struct ImageDescriptor;
class CropHostFeature;
class PartSegmentationHostFeature;
class SurfaceDeterminationHostFeature;
class MetrologyAlignmentHostFeature;
class ArtifactReductionHostFeature;

// Application choices for manual experiments, separate from Feature/SDK defaults.
struct FeatureTestOptions final {
    // 0 由应用按当前可用内存决定；显式命令行预算始终优先。
    std::size_t budgetBytes = 0;
    std::uint32_t timeoutMs = 300000;
    std::string inputPath = "F:\\data\\ct\\1536x1536x1536_1440.raw";
    std::array<int, 3> dimensions{1536, 1536, 1536};
    bool hasDimensions = false;
    std::optional<std::array<float, 3>> spacing;
    std::optional<std::array<float, 3>> origin;
    std::optional<std::array<double, 9>> direction;
    std::string inputFrame;
    std::string inputUnit;
    std::string inputFormat;
    std::string datasetId;
    std::string inputDigest;
    double editRadius = 0.0; // 0 selects 1.5 times the largest voxel spacing.
    std::uint64_t islandVoxels = 2;
    int ringAxis = 2;
    int ringWidth = 1;
    double ringStrength = 0.5;
    int diffusionIterations = 1;
    std::optional<std::array<double, 2>> ringCenter;
};

FeatureTestOptions GetFeatureTestOptions(int argc, char* argv[]);
void PrintFeatureTestHelp();
HostLoadRequest GetFeatureLoadRequest(const FeatureTestOptions& options, bool isAudit);
bool GetFeatureInputValid(const HostLoadRequest& request, const ImageDescriptor& descriptor);

struct FeatureTestBindings final {
    std::weak_ptr<CropHostFeature> crop;
    std::weak_ptr<PartSegmentationHostFeature> parts;
    std::weak_ptr<SurfaceDeterminationHostFeature> surface;
    std::weak_ptr<MetrologyAlignmentHostFeature> alignment;
    std::weak_ptr<ArtifactReductionHostFeature> artifact;
};

struct FeatureTestStep final {
    std::string name;
    HostKeyChord key;
    std::function<bool()> ready;
};

class FeatureTestControls final : public HostFeature,
    public std::enable_shared_from_this<FeatureTestControls> {
public:
    FeatureTestControls(VtkAppHostSession& session, FeatureTestBindings bindings,
        FeatureTestOptions options, HostViewTargets views);
    ~FeatureTestControls() override;
    std::string_view GetFeatureId() const noexcept override;
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;
    std::string GetFailure() const;
    std::vector<FeatureTestStep> GetAuditSteps(bool isReal = false);
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
