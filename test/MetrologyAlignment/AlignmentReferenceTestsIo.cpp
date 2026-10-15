#include "AlignmentReference.h"
#include "AlignmentData.h"
#include "Host/MetrologyAlignmentHostFeature.h"
#include "../TestDataPort.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
int failures = 0;
void Check(bool value, const char *message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
// ASCII 几何为单位正方形，两三角形共享两个顶点；包含前导正号与负零。
const std::string ascii = R"stl(solid square
facet normal 0 0 1
outer loop
vertex -0 0 0
vertex +1 0 0
vertex 0 1 0
endloop
endfacet
facet normal 0 0 1
outer loop
vertex 1 0 0
vertex 1 1 0
vertex 0 1 0
endloop
endfacet
endsolid square
)stl";
void Write(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("Fixture write failed.");
}
void UInt32(std::string &bytes, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
}
std::string Binary() {
    std::string bytes(80, '\0');
    bytes.replace(0, 12, "solid binary");
    UInt32(bytes, 1);
    // IEEE754 的 0/1 位模式给出一个单位直角三角形，与解析真值独立对应。
    for (const auto value : {0U,0U,0x3f800000U, 0U,0U,0U,
                             0x3f800000U,0U,0U, 0U,0x3f800000U,0U}) UInt32(bytes, value);
    bytes.append(2, '\0');
    return bytes;
}
AlignmentWork Work() {
    AlignmentWork work;

    work.cancelled = std::make_shared<std::atomic<bool>>(false);
    return work;
}
AlignmentReferenceRequest Request(const std::filesystem::path &path) {
    return {path.u8string(), "nominal-part", "RAS", AlignmentUnit::Meter, AlignmentUnit::Millimeter};
}
void Loader(const std::filesystem::path &folder) {
    const auto path = folder / std::filesystem::u8path(u8"参考表面.stl");
    Write(path, ascii);
    auto work = Work();
    auto request = Request(path);
    auto result = AlignmentReferenceLoader::Load(request, work);
    Check(result.status == AlignmentStatus::FullyDetermined && result.mesh,
          "ASCII STL imports from Unicode path");
    if (result.mesh) {
        Check(result.metadata.pointCount == 4 && result.metadata.triangleCount == 2,
              "exact coordinate welding preserves square topology");
        Check(result.mesh->GetPointValues() == MeshReadArray<double>(std::vector<double>{0,0,0, 1000,0,0, 0,1000,0, 1000,1000,0}),
              "explicit meter to millimeter conversion");
        Check(result.mesh->GetTriangleIndices() == MeshReadArray<std::uint64_t>(std::vector<std::uint64_t>{0,1,2, 1,3,2}),
              "source facet winding retained");
        // 此固定摘要由 .NET SHA256 对上面的 UTF-8/LF 字面量独立计算。
        Check(result.metadata.sourceBytes == ascii.size() && result.metadata.sha256 ==
              "aeb59df876f6709d396b360351d4d09f25035b9d7ce6afe66846e2756c85be48",
              "all original bytes identified");
    }
    const auto originalDigest = result.metadata.sha256;
    request.unit = AlignmentUnit::Meter;
    result = AlignmentReferenceLoader::Load(request, work);
    Check(result.mesh && result.mesh->GetPointValues()[3] == 1 && result.metadata.sha256 == originalDigest &&
          result.metadata.unit == AlignmentUnit::Meter, "file digest independent of explicit unit interpretation");
    request.fileUnit = AlignmentUnit::ModelUnit;
    Check(!AlignmentReferenceLoader::GetRequestValid(request), "unknown units cannot be guessed as meters");
    request = Request(path);
    work.cancelled->store(true);
    Check(AlignmentReferenceLoader::Load(request, work).status == AlignmentStatus::Cancelled,
          "cancelled import returns no geometry");
    work.cancelled->store(false);
    work = Work();
    const auto binaryPath = folder / "binary.stl";
    Write(binaryPath, Binary());
    auto binaryRequest = Request(binaryPath);
    binaryRequest.fileUnit = AlignmentUnit::Millimeter;
    result = AlignmentReferenceLoader::Load(binaryRequest, work);
    Check(result.mesh && result.mesh->GetPointValues() == MeshReadArray<double>(std::vector<double>{0,0,0, 1,0,0, 0,1,0}),
          "binary STL with solid header recognized by exact length");
    // 50-byte facet 跨越 64 KiB 输入块时仍保留准确的坐标与连接关系。
    const std::uint32_t facetCount = 65536 / 50 + 2;
    auto crossed = Binary().substr(0, 80);
    UInt32(crossed, facetCount);
    const auto facet = Binary().substr(84);
    for (std::uint32_t i = 0; i < facetCount; ++i) crossed += facet;
    Write(folder / "cross-block.stl", crossed);
    result = AlignmentReferenceLoader::Load(Request(folder / "cross-block.stl"), work);
    Check(result.mesh && result.metadata.pointCount == 3 && result.metadata.triangleCount == facetCount &&
          result.mesh->GetPointValues() == MeshReadArray<double>(std::vector<double>{0,0,0, 1000,0,0, 0,1000,0}),
          "binary facet spanning an IO block retains geometry");
    if (result.mesh)
        for (std::size_t i = 0; i < result.mesh->GetTriangleIndices().size(); ++i)
            Check(result.mesh->GetTriangleIndices()[i] == i % 3, "cross-block triangle connectivity retained");
    Write(folder / "truncated.stl", Binary().substr(0, 130));
    Check(AlignmentReferenceLoader::Load(Request(folder / "truncated.stl"), work).status == AlignmentStatus::InvalidInput,
          "truncated binary rejected");
    Write(folder / "broken.stl", ascii.substr(0, ascii.find("endsolid")));
    Check(AlignmentReferenceLoader::Load(Request(folder / "broken.stl"), work).status == AlignmentStatus::InvalidInput,
          "incomplete ASCII rejected");
    Write(folder / "trailing.stl", ascii + "unexpected-data\n");
    Check(AlignmentReferenceLoader::Load(Request(folder / "trailing.stl"), work).status == AlignmentStatus::InvalidInput,
          "non-solid trailing ASCII data rejected");
    Write(folder / "multi-solid.stl", ascii + ascii);
    result = AlignmentReferenceLoader::Load(Request(folder / "multi-solid.stl"), work);
    Check(result.mesh && result.metadata.triangleCount == 4 && result.metadata.pointCount == 4,
          "multiple ASCII solids preserve every facet and exact shared vertices");
    auto degenerate = Binary();
    for (std::size_t i = 96; i < 132; ++i) degenerate[i] = 0;
    Write(folder / "degenerate.stl", degenerate);
    Check(AlignmentReferenceLoader::Load(Request(folder / "degenerate.stl"), work).status == AlignmentStatus::InvalidInput,
          "degenerate surface rejected before publication");
    const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    Check(writer != INVALID_HANDLE_VALUE, "fixture writer opened");
    if (writer != INVALID_HANDLE_VALUE) {
        Check(AlignmentReferenceLoader::Load(Request(path), work).status == AlignmentStatus::IoError,
              "import rejects concurrently writable file");
        CloseHandle(writer);
    }
    Check(AlignmentReferenceLoader::Load(Request(folder / "missing.stl"), work).status == AlignmentStatus::IoError,
          "missing file reported as IO error");
}
class Port final : public TestDataPort {
  public:
    bool rejects = false, throwsAfterCommit = false;
    DataCommitResult SetDataCommit(DataTransaction tx) override {
        if (rejects) return {};
        const auto result = TestDataPort::SetDataCommit(std::move(tx));
        if (throwsAfterCommit) throw std::runtime_error("observer fault");
        return result;
    }
};
void Drain(const std::shared_ptr<MetrologyAlignmentHostFeature> &feature) {
    // 沿用 AlignmentLifecycleTests 的五秒测试调度上界，不是产品性能承诺。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (feature->GetState().isBusy && std::chrono::steady_clock::now() < deadline) {
        feature->OnHostTick(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(!feature->GetState().isBusy, "import owner completion bounded");
}
void PublicImport(const std::filesystem::path &folder) {
    const auto path = folder / "public.stl";
    Write(path, ascii);
    auto data = std::make_shared<Port>();
    auto feature = std::make_shared<MetrologyAlignmentHostFeature>();
    HostFeatureContext context;
    context.data = data;
    Check(feature->AttachHost(context), "import feature attaches without view or source volume");
    AlignmentRequest request;
    request.action = AlignmentAction::ImportReference;
    request.reference = Request(path);
    AlignmentResult result;
    int calls = 0;
    const auto callback = [&](AlignmentResult value) { result = std::move(value); ++calls; };
    Check(feature->SendRequest(request, callback).status == AlignmentAdmissionStatus::Accepted, "import accepted");
    Check(feature->SendRequest(request).status == AlignmentAdmissionStatus::Busy, "import uses bounded task slot");
    Drain(feature);
    const auto reference = feature->GetReference(result.referenceMesh);
    Check(calls == 1 && reference && reference->triangleCount == 2 && !result.isActivated,
          "public import publishes traceable reference without activating alignment");
    const auto graph = data->GetDataGraph();
    const auto mesh = graph.view->GetData(result.referenceMesh);
    Check(mesh && mesh->inputs.size() == 1 && mesh->inputs.front().role == "reference-source" &&
          graph.view->GetData(mesh->inputs.front().source), "mesh and source metadata published together");
    AlignmentRecipe recipe;
    recipe.id = "reference-units"; recipe.nominalData = result.referenceMesh;
    recipe.method = AlignmentMethod::ConstrainedBestFit; recipe.datumCount = 0;
    recipe.surfaceFit = AlignmentSurfaceFit{}; recipe.unit = AlignmentUnit::Meter;
    recipe.targetFrameId = "nominal-part";
    AlignmentRequest save; save.action = AlignmentAction::SaveRecipe; save.recipe = recipe;
    DataRevisionRef recipeRef;
    feature->SendRequest(save, [&](AlignmentResult value) { recipeRef = value.recipe; });
    AlignmentRequest start;
    start.recipeRef = recipeRef;
    start.input = AlignmentInput{result.referenceMesh, result.referenceMesh, result.referenceMesh,
        "measured", "RAS", "scope", AlignmentUnit::Meter};
    Check(feature->SendRequest(start).status == AlignmentAdmissionStatus::InvalidRequest,
          "imported units checked against subsequent recipe");
    data->rejects = true; calls = 0;
    const auto commit = data->GetDataGraph().commitId;
    feature->SendRequest(request, callback); Drain(feature);
    Check(calls == 1 && result.status == AlignmentStatus::InternalError &&
          !GetDataRevisionRefValid(result.referenceMesh) && data->GetDataGraph().commitId == commit,
          "commit failure publishes neither reference node");
    data->rejects = false; data->throwsAfterCommit = true; calls = 0;
    feature->SendRequest(request, callback); Drain(feature);
    Check(calls == 1 && result.status == AlignmentStatus::FullyDetermined &&
          feature->GetReference(result.referenceMesh).has_value(), "post-commit fault retains published reference");
    data->throwsAfterCommit = false; calls = 0;
    const auto beforeCancel = data->GetDataGraph().commitId;
    feature->SendRequest(request, callback);
    AlignmentRequest cancel; cancel.action = AlignmentAction::Cancel;
    feature->SendRequest(cancel); Drain(feature);
    Check(calls == 1 && result.status == AlignmentStatus::Cancelled &&
          data->GetDataGraph().commitId == beforeCancel, "cancelled import cannot publish late output");
    request.action = AlignmentAction::SetVisibility; request.isVisible = true;
    Check(feature->SendRequest(request, callback).status == AlignmentAdmissionStatus::InvalidRequest && calls == 1,
          "reference fields rejected by other actions without callback retention");
    Check(feature->DetachHost(), "import feature detaches");
}

int Audit(const std::filesystem::path &path, std::size_t budgetMiB,
          std::size_t timeoutMs, const std::filesystem::path &output) {
    if (budgetMiB == 0 || budgetMiB > std::numeric_limits<std::size_t>::max() / (1024 * 1024) ||
        timeoutMs == 0 || timeoutMs > 3600000) throw std::invalid_argument("Invalid audit limits.");
    AlignmentConfig config;


    auto data = std::make_shared<TestDataPort>();
    auto feature = std::make_shared<MetrologyAlignmentHostFeature>(config);
    HostFeatureContext context; context.data = data;
    if (!feature->AttachHost(context)) throw std::runtime_error("Audit attach failed.");
    AlignmentRequest request; request.action = AlignmentAction::ImportReference;
    // 导入审计只检查文件几何/资源；不推测该实测导出文件的物理单位或名义身份。
    request.reference = AlignmentReferenceRequest{path.u8string(), "audit-reference", "STL",
        AlignmentUnit::ModelUnit, AlignmentUnit::ModelUnit};
    AlignmentResult result;
    std::size_t callbacks = 0;
    PROCESS_MEMORY_COUNTERS_EX before{}; before.cb = sizeof(before);
    const bool hasBefore = GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before)) != 0;
    const auto start = std::chrono::steady_clock::now();
    const auto admission = feature->SendRequest(request, [&](AlignmentResult value) {
        result = std::move(value); ++callbacks;
    });
    if (admission.status != AlignmentAdmissionStatus::Accepted) throw std::runtime_error("Audit request rejected.");
    while (feature->GetState().isBusy) {
        feature->OnHostTick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs) + std::chrono::seconds(5)) {
            std::ofstream failed(output);
            failed << "{\"scope\":\"public STL import\",\"status\":\"HARNESS_TIMEOUT\"}\n";
            failed.close();
            std::quick_exit(2); // 审计子进程上界；不让失去响应的 worker 阻塞测试退出。
        }
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    PROCESS_MEMORY_COUNTERS_EX after{}; after.cb = sizeof(after);
    const bool hasAfter = GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after)) != 0;
    const auto reference = feature->GetReference(result.referenceMesh);
    const bool passed = callbacks == 1 && reference && result.status == AlignmentStatus::FullyDetermined;
    std::ofstream report(output);
    if (!report) throw std::runtime_error("Audit output unavailable.");
    report << "{\n  \"scope\": \"public STL import only; no registration, rendering or physical accuracy claim\",\n"
           << "  \"status\": \"" << (passed ? "PASS" : "FAIL") << "\",\n"
           << "  \"resultStatus\": " << static_cast<unsigned>(result.status) << ",\n"
           << "  \"workingBudgetMiB\": " << budgetMiB << ",\n"
           << "  \"deadlineMs\": " << timeoutMs << ",\n"
           << "  \"elapsedMs\": " << elapsed << ",\n"
           << "  \"memoryAvailable\": " << (hasBefore && hasAfter ? "true" : "false") << ",\n"
           << "  \"initialPrivateBytes\": " << before.PrivateUsage << ",\n"
           << "  \"finalPrivateBytes\": " << after.PrivateUsage << ",\n"
           << "  \"processPeakWorkingSetBytes\": " << after.PeakWorkingSetSize << ",\n"
           << "  \"processPeakCommitBytes\": " << after.PeakPagefileUsage << ",\n"
           << "  \"sourceSha256\": \"" << (reference ? reference->sha256 : "") << "\",\n"
           << "  \"sourceBytes\": " << (reference ? reference->sourceBytes : 0) << ",\n"
           << "  \"pointCount\": " << (reference ? reference->pointCount : 0) << ",\n"
           << "  \"triangleCount\": " << (reference ? reference->triangleCount : 0) << "\n}\n";
    if (!feature->DetachHost()) throw std::runtime_error("Audit detach failed.");
    return passed ? 0 : 1;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 6 && std::wstring(argv[1]) == L"--audit")
            return Audit(std::filesystem::path(argv[2]), static_cast<std::size_t>(std::stoull(argv[3])),
                         static_cast<std::size_t>(std::stoull(argv[4])), std::filesystem::path(argv[5]));
        if (argc != 2) throw std::invalid_argument("Expected test output directory.");
        const auto folder = std::filesystem::path(argv[1]) /
            (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(folder);
        Loader(folder); PublicImport(folder);
    } catch (const std::exception &error) { ++failures; std::cerr << error.what() << '\n'; }
    return failures ? 1 : 0;
}
