#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceContracts.h"
#include "Data/VtkDataBridge.h"

#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkDataArray.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <numeric>
#include <stdexcept>

namespace
{
struct Manifest final
{
    std::filesystem::path rawPath;
    std::string identity, unit, reference, scalarType;
    std::uint64_t checksum = 0;
    GridGeometry3D geometry;
    std::array<double, 7> referenceValues{};
    double meanLimit = 0, p95Limit = 0, maximumLimit = 0, minimumCoverage = 0;
    std::size_t minimumAccepted = 0, inputLimit = 0, workingLimit = 0;
    SurfaceRecipe recipe;
};
void Require(bool valid, const char *message)
{
    if (!valid)
        throw std::runtime_error(message);
}
void ReadKey(std::istream &in, const char *expected)
{
    std::string key;
    Require(bool(in >> key) && key == expected, "Unexpected manifest field.");
}
bool FinitePositive(double x)
{
    return std::isfinite(x) && x > 0;
}
Manifest ReadManifest(const std::filesystem::path &path)
{
    Require(std::filesystem::file_size(path) <= 1024 * 1024, "Manifest exceeds 1 MiB.");
    std::ifstream in(path);
    in.imbue(std::locale::classic());
    Manifest m;
    std::string raw, order, text;
    unsigned version = 0;
    ReadKey(in, "surface-acceptance");
    Require(bool(in >> version) && version == 1, "Unsupported acceptance schema.");
    ReadKey(in, "raw");
    Require(bool(in >> std::quoted(raw) >> m.scalarType >> order) && order == "little",
            "RAW requires an explicit little-endian scalar layout.");
    m.rawPath = std::filesystem::u8path(raw);
    if (m.rawPath.is_relative())
        m.rawPath = path.parent_path() / m.rawPath;
    ReadKey(in, "identity");
    Require(bool(in >> std::quoted(m.identity) >> std::hex >> m.checksum >> std::dec) && !m.identity.empty(),
            "Missing dataset identity/checksum.");
    ReadKey(in, "geometry");
    for (auto &v : m.geometry.extent)
        Require(bool(in >> v), "Missing extent.");
    for (auto &v : m.geometry.origin)
        Require(bool(in >> v), "Missing origin.");
    for (auto &v : m.geometry.spacing)
        Require(bool(in >> v), "Missing spacing.");
    for (auto &v : m.geometry.direction)
        Require(bool(in >> v), "Missing direction.");
    Require(bool(in >> std::quoted(m.geometry.coordinateFrame) >> std::quoted(m.unit)) &&
                m.geometry.coordinateFrame == "RAS" &&
                (m.unit == "mm" || m.unit == "cm" || m.unit == "m" || m.unit == "um"),
            "Acceptance input requires canonical RAS and a physical length unit.");
    for (unsigned a = 0; a < 3; ++a)
    {
        const auto n = std::int64_t(m.geometry.extent[2 * a + 1]) - m.geometry.extent[2 * a] + 1;
        Require(n >= 2 && n <= INT_MAX, "Invalid volume dimensions.");
        m.geometry.dimensions[a] = static_cast<int>(n);
    }
    Require(GetGridGeometryValid(m.geometry), "Invalid input geometry.");
    ReadKey(in, "limits");
    Require(bool(in >> m.inputLimit >> m.workingLimit) && m.inputLimit && m.workingLimit,
            "Missing explicit reader and algorithm memory limits.");
    ReadKey(in, "reference");
    Require(bool(in >> m.reference) &&
                (m.reference == "plane" || m.reference == "sphere" || m.reference == "cylinder"),
            "Unsupported reference geometry.");
    const unsigned count = m.reference == "cylinder" ? 7 : 4;
    for (unsigned i = 0; i < count; ++i)
        Require(bool(in >> m.referenceValues[i]) && std::isfinite(m.referenceValues[i]),
                "Invalid reference values.");
    if (m.reference == "plane")
        Require(std::abs(std::hypot(m.referenceValues[0], m.referenceValues[1], m.referenceValues[2]) - 1) <
                    1e-8,
                "Reference plane normal must be unit length.");
    if (m.reference == "sphere")
        Require(FinitePositive(m.referenceValues[3]), "Reference sphere radius must be positive.");
    if (m.reference == "cylinder")
        Require(FinitePositive(m.referenceValues[6]) &&
                    std::abs(std::hypot(m.referenceValues[3], m.referenceValues[4], m.referenceValues[5]) -
                             1) < 1e-8,
                "Reference cylinder needs a unit axis and positive radius.");
    ReadKey(in, "tolerances");
    Require(
        bool(in >> m.meanLimit >> m.p95Limit >> m.maximumLimit >> m.minimumAccepted >> m.minimumCoverage) &&
            FinitePositive(m.meanLimit) && FinitePositive(m.p95Limit) && FinitePositive(m.maximumLimit) &&
            m.minimumAccepted && FinitePositive(m.minimumCoverage) && m.minimumCoverage <= 1,
        "Explicit distance tolerances and minimum support are required.");
    ReadKey(in, "recipe");
    Require(bool(in >> std::quoted(text)), "Missing business recipe.");
    const auto recipe = SurfaceRecipeCodec::GetRecipe(text);
    Require(bool(recipe.recipe), "Invalid business recipe.");
    m.recipe = *recipe.recipe;
    Require(m.recipe.materialPairs.empty() && m.recipe.method != SurfaceDeterminationMethod::AutomaticIso50 &&
                m.recipe.method != SurfaceDeterminationMethod::GlobalIsoPreview,
            "This RAW acceptance entry requires a gray-volume localization recipe.");
    in >> std::ws;
    Require(in.eof(), "Trailing manifest fields.");
    return m;
}
double Distance(const Manifest &m, const std::array<double, 3> &p)
{
    const auto &r = m.referenceValues;
    if (m.reference == "plane")
        return std::abs(p[0] * r[0] + p[1] * r[1] + p[2] * r[2] - r[3]);
    const std::array<double, 3> delta{p[0] - r[0], p[1] - r[1], p[2] - r[2]};
    if (m.reference == "sphere")
        return std::abs(std::hypot(delta[0], delta[1], delta[2]) - r[3]);
    const double axial = delta[0] * r[3] + delta[1] * r[4] + delta[2] * r[5];
    return std::abs(std::hypot(delta[0] - axial * r[3], delta[1] - axial * r[4], delta[2] - axial * r[5]) -
                    r[6]);
}
int Run(const Manifest &m)
{
    const std::uint16_t endian = 1;
    Require(*reinterpret_cast<const unsigned char *>(&endian) == 1,
            "RAW entry currently supports little-endian hosts.");
    int type = VTK_VOID;
    std::size_t scalarBytes = 0;
    if (m.scalarType == "uint8")
    {
        type = VTK_UNSIGNED_CHAR;
        scalarBytes = 1;
    }
    if (m.scalarType == "uint16")
    {
        type = VTK_UNSIGNED_SHORT;
        scalarBytes = 2;
    }
    if (m.scalarType == "int16")
    {
        type = VTK_SHORT;
        scalarBytes = 2;
    }
    if (m.scalarType == "float32")
    {
        type = VTK_FLOAT;
        scalarBytes = 4;
    }
    if (m.scalarType == "float64")
    {
        type = VTK_DOUBLE;
        scalarBytes = 8;
    }
    Require(type != VTK_VOID, "Unsupported RAW scalar type.");
    const auto count = GetGridVoxelCount(m.geometry);
    Require(count && *count <= m.inputLimit / scalarBytes, "RAW input exceeds the reader limit.");
    const auto bytes = *count * scalarBytes;
    Require(std::filesystem::file_size(m.rawPath) == bytes, "RAW length does not match its declared layout.");
    vtkNew<vtkImageData> image;
    image->SetExtent(m.geometry.extent[0], m.geometry.extent[1], m.geometry.extent[2], m.geometry.extent[3],
                     m.geometry.extent[4], m.geometry.extent[5]);
    image->SetOrigin(m.geometry.origin.data());
    image->SetSpacing(m.geometry.spacing.data());
    vtkNew<vtkMatrix3x3> direction;
    direction->DeepCopy(m.geometry.direction.data());
    image->SetDirectionMatrix(direction);
    image->AllocateScalars(type, 1);
    auto *buffer = static_cast<unsigned char *>(image->GetScalarPointer());
    Require(buffer, "RAW allocation failed.");
    std::ifstream raw(m.rawPath, std::ios::binary);
    std::uint64_t checksum = 14695981039346656037ULL;
    for (std::size_t offset = 0; offset < bytes;)
    {
        const auto amount = std::min<std::size_t>(1024 * 1024, bytes - offset);
        Require(
            bool(raw.read(reinterpret_cast<char *>(buffer + offset), static_cast<std::streamsize>(amount))),
            "RAW read failed.");
        for (std::size_t i = 0; i < amount; ++i)
        {
            checksum ^= buffer[offset + i];
            checksum *= 1099511628211ULL;
        }
        offset += amount;
    }
    Require(checksum == m.checksum, "Dataset FNV-1a-64 identity mismatch.");
    VtkDataBridge bridge;
    const auto payload = bridge.CreateImagePayload(image);
    Require(bool(payload), "RAW source adapter rejected the volume.");
    DataEntityId entity;
    for (unsigned i = 0; i < 8; ++i)
        entity.bytes[i] = static_cast<std::uint8_t>(checksum >> (8 * i));
    entity.bytes[15] = 1;
    const auto data = std::make_shared<const DataRevision>(
        DataRevision{{entity, 1}, DataTypes::imageGrid3D, {}, payload, {}});
    const auto source = bridge.GetImageGrid(data);
    SurfaceDeterminationStartParams params;
    static_cast<SurfaceRecipe &>(params) = m.recipe;
    params.targetViews = {};
    params.sourceVolume = data->self;
    params.modelUnit = m.unit;
    const auto begin = std::chrono::steady_clock::now();
    const auto result =
        SurfaceDeterminationAlgorithm::BuildSurface(source, params, m.workingLimit, [] { return false; }, {});
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    Require(result.status == SurfaceResultStatus::Succeeded, result.message.c_str());
    std::vector<double> errors;
    errors.reserve(result.acceptedPointCount);
    for (const auto &point : result.points)
        if (SurfaceContract::GetPointValid(point, result.method))
            errors.push_back(Distance(m, point.positionModel));
    Require(!errors.empty(), "No measurement-valid points.");
    std::sort(errors.begin(), errors.end());
    const double mean = std::accumulate(errors.begin(), errors.end(), 0.0) / errors.size();
    const double p95 = errors[static_cast<std::size_t>(std::ceil(.95 * errors.size())) - 1];
    const double coverage = double(errors.size()) / result.points.size();
    const bool passed = errors.size() >= m.minimumAccepted && coverage >= m.minimumCoverage &&
                        mean <= m.meanLimit && p95 <= m.p95Limit && errors.back() <= m.maximumLimit;
    std::cout << std::setprecision(17) << "dataset=" << std::quoted(m.identity) << " fnv1a64=" << std::hex
              << checksum << std::dec << " unit=" << m.unit << " algorithm=" << result.algorithmRevision
              << " fingerprint=" << result.parameterFingerprint << '\n'
              << "input_bytes=" << bytes << " input_limit=" << m.inputLimit
              << " working_limit=" << m.workingLimit << " estimated_working_bytes=" << result.requiredBytes
              << " retained_bytes=" << result.execution.retainedBytes << '\n'
              << "points=" << result.points.size() << " triangles=" << result.triangleIndices.size() / 3
              << " accepted=" << errors.size() << " coverage=" << coverage << '\n'
              << "mean=" << mean << " p95=" << p95 << " maximum=" << errors.back()
              << " seed_ms=" << result.execution.seedMs << " refinement_ms=" << result.execution.refinementMs
              << " total_ms=" << elapsed << " status=" << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
} // namespace
int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--describe")
    {
        std::cout
            << "surface-acceptance 1\nraw \"path.raw\" float32 little\nidentity \"traceable-dataset-id\" "
               "HEX_FNV1A64\n"
            << "geometry xmin xmax ymin ymax zmin zmax ox oy oz sx sy sz d00 d01 d02 d10 d11 d12 d20 d21 d22 "
               "\"RAS\" \"mm\"\n"
            << "limits MAX_INPUT_BYTES MAX_WORKING_BYTES\nreference plane nx ny nz d\n"
            << "# or reference sphere cx cy cz radius; cylinder cx cy cz ax ay az radius (unit normal/axis)\n"
            << "tolerances MEAN P95 MAXIMUM MIN_ACCEPTED MIN_VALID_FRACTION\nrecipe \"escaped "
               "SurfaceRecipeCodec text\"\n"
            << "FNV-1a-64 uses offset 14695981039346656037 and prime 1099511628211. RAW is x-fastest; no "
               "comments or omitted fields.\n";
        return 0;
    }
    try
    {
        Require(argc == 2, "Usage: SurfaceBusinessAcceptance MANIFEST or --describe");
        return Run(ReadManifest(std::filesystem::u8path(argv[1])));
    }
    catch (const std::exception &error)
    {
        std::cerr << "INVALID: " << error.what() << '\n';
        return 2;
    }
}
