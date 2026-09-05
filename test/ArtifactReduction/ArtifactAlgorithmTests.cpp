#include "ArtifactTestSupport.h"
#include "TomoPyRingAdapter.h"

#include <vtkImageAnisotropicDiffusion3D.h>
#include <vtkImageData.h>
#include <vtkNew.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <random>

namespace {
void TestTypesAndMasks()
{
    auto grid = CreateGrid(7, 5, 3);
    grid.extent = { -4, 2, 8, 12, 11, 13 };
    grid.spacing = { 0.25, 0.5, 2.0 };
    grid.origin = { 7, -9, 11 };
    grid.direction = { 0, -1, 0, 1, 0, 0, 0, 0, 1 };
    ImageMetadata metadata;
    metadata.source = { ImageSourceKind::RawFile, "source.raw", 210, std::string("source-digest") };
    metadata.scalar.slope = 3.0;
    metadata.scalar.intercept = -20.0;
    metadata.scalar.noData = -99.0;
    std::vector<std::int16_t> values(105, -4);
    values[0] = -99; values[1] = 30000;
    auto validity = std::make_shared<std::vector<std::uint8_t>>(105, 1);
    (*validity)[2] = 0;
    ArtifactReduction::AlgorithmInput input;
    input.image = CreateImage(grid, ImageValueType::Int16, values, validity, metadata);
    const auto result = BuildCandidate(input);
    Require(result.error == ArtifactError::None && result.image, "identity conversion");
    const auto converted = GetValues(*result.image);
    for (std::size_t i = 0; i < values.size(); ++i) Require(converted[i] == values[i], "int16 exact float conversion");
    const auto& actualGrid = result.image->GetGeometry();
    Require(actualGrid.extent == grid.extent && actualGrid.dimensions == grid.dimensions && actualGrid.spacing == grid.spacing
        && actualGrid.origin == grid.origin && actualGrid.direction == grid.direction && actualGrid.coordinateFrame == grid.coordinateFrame, "grid unchanged");
    Require(*result.image->GetValidityMask() == *validity, "validity unchanged");
    const auto& outputMetadata = result.image->GetMetadata();
    Require(outputMetadata.source.kind == ImageSourceKind::Memory && outputMetadata.source.uri.empty()
        && !outputMetadata.source.digest && outputMetadata.source.byteSize == 420, "derived source metadata");
    Require(outputMetadata.scalar.slope == 3.0 && outputMetadata.scalar.intercept == -20.0, "no repeated calibration");
    Require(result.quality.validCount == 103 && !result.quality.fidelityVerified, "quality validity semantics");
    const auto snapshot = std::dynamic_pointer_cast<const ImageGrid3DPayload>(result.image->CreateSnapshot());
    Require(snapshot->GetValues() == result.image->GetValues(), "Store freeze shares immutable values");
    ArtifactRequest request;
    request.diffusion = ArtifactDiffusionParams{};
    Require(BuildCandidate(input, request).error == ArtifactError::UnsupportedValidity, "invalid data never filled for diffusion");
    auto wrongMaskGrid = grid; wrongMaskGrid.origin[0] += 1;
    input.processing = CreateMask(wrongMaskGrid, std::vector<std::uint8_t>(105, 1));
    Require(BuildCandidate(input).error == ArtifactError::InvalidData, "mask physical geometry mismatch");
    input.processing.reset();
    input.image = CreateImage(grid, ImageValueType::UInt8, std::vector<std::uint8_t>(105, 255));
    Require(GetValues(*BuildCandidate(input).image)[0] == 255, "uint8 conversion");
    input.image = CreateImage(grid, ImageValueType::UInt16, std::vector<std::uint16_t>(105, 65535));
    Require(GetValues(*BuildCandidate(input).image)[0] == 65535, "uint16 conversion");
    input.image = CreateImage(grid, ImageValueType::Float64, std::vector<double>(105, 1.0));
    Require(BuildCandidate(input).error == ArtifactError::UnsupportedType, "float64 precision rejection");
    input.image = CreateImage(grid, ImageValueType::UInt32, std::vector<std::uint32_t>(105, 1));
    Require(BuildCandidate(input).error == ArtifactError::UnsupportedType, "uint32 precision rejection");
    std::vector<float> floats(105, 3.5f);
    floats[4] = std::numeric_limits<float>::quiet_NaN();
    input.image = CreateImage(grid, ImageValueType::Float32, floats);
    const auto identity = BuildCandidate(input);
    Require(*identity.image->GetValues() == *input.image->GetValues(), "float no-op byte preservation including NaN");
    Require(BuildCandidate(input, request).error == ArtifactError::UnsupportedValidity, "NaN diffusion rejection");
    std::size_t required = 0;
    Require(ArtifactReduction::GetInputError(input, {}, {}, required) == ArtifactError::None, "budget estimate");
    ArtifactConfig budget;
    budget.memoryBudgetBytes = required;
    Require(BuildCandidate(input, {}, budget).error == ArtifactError::None, "exact budget acceptance");
    --budget.memoryBudgetBytes;
    Require(BuildCandidate(input, {}, budget).error == ArtifactError::TooLarge, "one-byte budget rejection");
    ArtifactReduction::TaskControl control;
    control.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    Require(ArtifactReduction::BuildArtifactCandidate(input, {}, {}, control).error == ArtifactError::TimedOut, "absolute deadline");
    control.isCancelled.store(true);
    Require(ArtifactReduction::BuildArtifactCandidate(input, {}, {}, control).error == ArtifactError::Cancelled, "cancel precedence");
}

void TestDiffusion()
{
    auto grid = CreateGrid(19, 17, 13);
    grid.extent = { -9, 9, 12, 28, -30, -18 };
    grid.origin = { 10, 20, -40 };
    grid.spacing = { 0.5, 1, 2 };
    grid.direction = { 0, -1, 0, 1, 0, 0, 0, 0, 1 };
    const auto count = *GetGridVoxelCount(grid);
    std::vector<float> values(count);
    std::vector<std::uint8_t> material(count);
    std::mt19937 random(1829);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (int z = 0; z < 13; ++z) for (int y = 0; y < 17; ++y) for (int x = 0; x < 19; ++x) {
        const auto index = (static_cast<std::size_t>(z) * 17 + y) * 19 + x;
        values[index] = (x < 9 ? 100.0f : 150.0f) + noise(random);
        material[index] = x < 7 ? 1 : 0;
    }
    ArtifactReduction::AlgorithmInput input;
    input.image = CreateImage(grid, ImageValueType::Float32, values);
    input.material = CreateMask(grid, material);
    ArtifactRequest request;
    request.diffusion = ArtifactDiffusionParams{ 3, 4.0, 0.5, 13 };
    const auto full = BuildCandidate(input, request);
    Require(full.error == ArtifactError::None, "VTK diffusion full");
    Require(full.quality.materialStdAfter < full.quality.materialStdBefore, "diffusion reduces homogeneous noise");
    for (const int slab : { 1, 3 }) {
        request.diffusion->slabDepth = slab;
        const auto blocked = BuildCandidate(input, request);
        Require(blocked.error == ArtifactError::None && *blocked.image->GetValues() == *full.image->GetValues(), "N-iteration halo exact parity");
    }
    vtkNew<vtkImageData> referenceInput;
    referenceInput->SetExtent(grid.extent.data());
    referenceInput->SetSpacing(grid.spacing.data());
    referenceInput->SetOrigin(grid.origin.data());
    referenceInput->SetDirectionMatrix(grid.direction.data());
    referenceInput->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(referenceInput->GetScalarPointer(), values.data(), values.size() * sizeof(float));
    vtkNew<vtkImageAnisotropicDiffusion3D> reference;
    reference->SetEnableSMP(false); reference->SetNumberOfThreads(1);
    reference->SetInputData(referenceInput); reference->SetNumberOfIterations(3);
    reference->SetDiffusionThreshold(4.0); reference->SetDiffusionFactor(0.5);
    reference->Update();
    Require(std::memcmp(reference->GetOutput()->GetScalarPointer(), full.image->GetValues()->data(), values.size() * sizeof(float)) == 0,
        "direct VTK full-volume reference");
    std::vector<std::uint8_t> protection(count, 0), processing(count, 1);
    for (std::size_t i = 0; i < count; ++i) { if (i % 7 == 0) protection[i] = 1; if (i % 11 == 0) processing[i] = 0; }
    input.protection = CreateMask(grid, protection);
    input.processing = CreateMask(grid, processing);
    const auto masked = BuildCandidate(input, request);
    const auto actual = GetValues(*masked.image);
    const auto expected = GetValues(*full.image);
    for (std::size_t i = 0; i < count; ++i)
        Require(actual[i] == ((protection[i] || !processing[i]) ? values[i] : expected[i]), "mask only restricts final writeback");
    std::cout << "diffusion material std: " << full.quality.materialStdBefore << " -> " << full.quality.materialStdAfter << '\n';
}

void TestRing()
{
    auto grid = CreateGrid(64, 64, 2);
    grid.extent = { -7, 56, 12, 75, 9, 10 };
    grid.direction = { 0, 0, 1, 0, 1, 0, -1, 0, 0 };
    std::vector<float> values(64 * 64 * 2);
    std::vector<std::uint8_t> protect(values.size()), material(values.size());
    for (int z = 0; z < 2; ++z) for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        const double radius = std::hypot(x - 32.0, y - 32.0);
        const auto i = (static_cast<std::size_t>(z) * 64 + y) * 64 + x;
        values[i] = 100.0f + static_cast<float>(5.0 * std::sin(radius * 1.5));
        // 真实同心边界及小孔/薄壁样本明确保护，不假装算法自动识别。
        if ((radius > 7 && radius < 9) || (x == 30 && y < 12) || (x == 10 && y == 10)) { values[i] = 160; protect[i] = 1; }
        material[i] = radius > 14 && radius < 26 ? 1 : 0;
    }
    ArtifactReduction::AlgorithmInput input;
    input.image = CreateImage(grid, ImageValueType::Float32, values);
    input.protection = CreateMask(grid, protect);
    input.material = CreateMask(grid, material);
    ArtifactRequest request;
    ArtifactRingParams ring;
    ring.centerIndex = { 25, 44 }; ring.threshMin = 0; ring.threshMax = 200; ring.threshold = 50; ring.ringWidth = 1; ring.maxCorrection = 20;
    request.ring = ring;
    const auto result = BuildCandidate(input, request);
    Require(result.error == ArtifactError::None, "ring adapter success");
    const auto output = GetValues(*result.image);
    for (std::size_t i = 0; i < values.size(); ++i) if (protect[i]) Require(output[i] == values[i], "protected concentric/thin structure preserved");
    Require(result.quality.materialStdAfter < result.quality.materialStdBefore, "synthetic ring residual decrease");
    ArtifactReduction::AlgorithmInput unmasked;
    unmasked.image = input.image;
    const auto baseline = GetValues(*BuildCandidate(unmasked, request).image);
    for (const int axis : { 0, 1 }) {
        std::array<int, 2> planeAxes{};
        int next = 0;
        for (int a = 0; a < 3; ++a) if (a != axis) planeAxes[next++] = a;
        std::array<int, 3> dims{ 64, 64, 64 }; dims[axis] = 2;
        const auto axisGrid = CreateGrid(dims[0], dims[1], dims[2]);
        const auto offset = [&](int x, int y, int z) {
            std::array<int, 3> point{};
            point[axis] = z; point[planeAxes[0]] = x; point[planeAxes[1]] = y;
            return (static_cast<std::size_t>(point[2]) * dims[1] + point[1]) * dims[0] + point[0];
        };
        std::vector<float> permuted(values.size());
        for (int z = 0; z < 2; ++z) for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
            permuted[offset(x, y, z)] = values[(static_cast<std::size_t>(z) * 64 + y) * 64 + x];
        unmasked.image = CreateImage(axisGrid, ImageValueType::Float32, permuted);
        auto axisRequest = request; axisRequest.ring->axis = axis; axisRequest.ring->centerIndex = { 32, 32 };
        const auto axisResult = BuildCandidate(unmasked, axisRequest);
        Require(axisResult.error == ArtifactError::None, "grid X/Y ring axis");
        const auto axisValues = GetValues(*axisResult.image);
        for (int z = 0; z < 2; ++z) for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
            Require(axisValues[offset(x, y, z)] == baseline[(static_cast<std::size_t>(z) * 64 + y) * 64 + x], "axis permutation exact parity");
    }
    auto bad = grid; bad.spacing[1] = 2;
    input.image = CreateImage(bad, ImageValueType::Float32, values); input.protection.reset(); input.material.reset();
    Require(BuildCandidate(input, request).error == ArtifactError::UnsupportedGeometry, "non-square ring pixel geometry rejected");
    input.image = CreateImage(grid, ImageValueType::Float32, values);
    ring.strength = 0; request.ring = ring;
    Require(*BuildCandidate(input, request).image->GetValues() == *input.image->GetValues(), "zero ring strength bypass");
    std::cout << "ring material std: " << result.quality.materialStdBefore << " -> " << result.quality.materialStdAfter << '\n';
}
}

void TestAlgorithm()
{
    TestTypesAndMasks();
    TestDiffusion();
    TestRing();
}
