#include "Host/VtkAppHostSession.h"
#include "Host/PartSegmentationHostFeature.h"

#include <vtkSmartPointer.h>
#include <vtkWin32OpenGLRenderWindow.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::uint64_t GetCpuTicks()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0, "GetProcessTimes failed");
    const auto ticks = [](FILETIME value) { return (std::uint64_t{value.dwHighDateTime} << 32) | value.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}
struct Phase final {
    Clock::time_point started = Clock::now();
    std::uint64_t cpuStarted = GetCpuTicks();
    SIZE_T peakWorking = 0, peakPrivate = 0;
    void Sample() {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        Check(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != 0,
            "GetProcessMemoryInfo failed");
        peakWorking = std::max(peakWorking, memory.WorkingSetSize);
        peakPrivate = std::max(peakPrivate, memory.PrivateUsage);
    }
    void Report(const char* name) {
        Sample();
        std::cout << "phase=" << name << " wallMs=" << std::chrono::duration<double, std::milli>(Clock::now()-started).count()
            << " cpuMs=" << (GetCpuTicks()-cpuStarted)/10000.0
            << " sampledWorkingBytes=" << peakWorking << " sampledPrivateBytes=" << peakPrivate << '\n';
    }
};
std::uint64_t GetHash(const void* values, std::size_t bytes)
{
    const auto* data = static_cast<const unsigned char*>(values);
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t index=0; index<bytes; ++index) { hash ^= data[index]; hash *= 1099511628211ULL; }
    return hash;
}
std::vector<PartLabelId> ReadLabels(VtkAppHostSession& session, const PartSegmentationState& state, std::size_t count)
{
    const auto descriptors = session.GetLabelMapDescriptors();
    const auto found = std::find_if(descriptors.begin(), descriptors.end(), [&](const auto& d) { return d.dataRevision == state.labelMap; });
    Check(found != descriptors.end(), "Current label descriptor is missing");
    LabelMapReadRequest request;
    request.id = found->id;
    request.expectedRevision = state.labelMap;
    request.maxBytes = count * sizeof(PartLabelId);
    const auto result = session.GetLabelMapReadResult(request);
    Check(result.error == LabelMapError::None && result.state && result.state->values
        && result.state->values->size() == request.maxBytes, "Public label read failed");
    std::vector<PartLabelId> labels(count);
    std::memcpy(labels.data(), result.state->values->data(), request.maxBytes);
    return labels;
}
void ReportLabels(const std::vector<PartLabelId>& labels, const PartSetSnapshot& parts,
    const std::string& prefix, const std::string& stage)
{
    std::vector<std::uint64_t> histogram(parts.parts.size()+1, 0);
    for (auto label : labels) { Check(label < histogram.size(), "Output label exceeds catalog"); ++histogram[label]; }
    std::ofstream output(prefix + "." + stage + ".metrics.txt");
    Check(output.good(), "Cannot write audit metrics");
    output << std::setprecision(17);
    for (const auto& part : parts.parts) {
        Check(histogram[part.labelId] == part.metrics.voxelCount, "Catalog counts do not match output labels");
        output << part.labelId << ' ' << part.metrics.voxelCount << ' ' << part.metrics.physicalVolumeMM3;
        for (auto v : part.metrics.voxelExtent) output << ' ' << v;
        for (auto v : part.metrics.centroidInputPhysical) output << ' ' << v;
        for (auto v : part.metrics.inputPhysicalBounds) output << ' ' << v;
        output << '\n';
    }
    std::ofstream raw(prefix + "." + stage + ".labels.bin", std::ios::binary);
    raw.write(reinterpret_cast<const char*>(labels.data()), static_cast<std::streamsize>(labels.size()*sizeof(PartLabelId)));
    Check(raw.good(), "Cannot write audit labels");
    std::cout << "stage=" << stage << " parts=" << parts.parts.size() << " labelsHash="
        << GetHash(labels.data(), labels.size()*sizeof(PartLabelId)) << '\n';
}
}

// 手工审计入口：真实 RAW、源立方边长、三个相同 ROI 起点、ROI 边长、阈值、输出前缀。
// 不注册为无数据即通过的 CTest；缺数据或公共链失败一律返回非零。
int main(int argc, char** argv)
{
    try {
        Check(argc == 7, "Usage: PartPerformanceAudit raw sourceSide roiStart roiSide threshold outputPrefix");
        const int sourceSide = std::stoi(argv[2]), roiStart = std::stoi(argv[3]), side = std::stoi(argv[4]);
        const double threshold = std::stod(argv[5]);
        Check(sourceSide>0 && sourceSide<=8192 && roiStart>=0 && side>1 && side<=512
            && roiStart <= sourceSide-side && std::isfinite(threshold), "Invalid audit geometry or threshold");
        const std::size_t count = static_cast<std::size_t>(side)*side*side;
        std::ifstream raw(argv[1], std::ios::binary | std::ios::ate);
        Check(raw.good() && raw.tellg() == static_cast<std::streamoff>(static_cast<std::uint64_t>(sourceSide)*sourceSide*sourceSide*sizeof(float)),
            "Source RAW byte count disagrees with the declared geometry");
        HostReloadRequest reload;
        reload.voxels.resize(count);
        for (int z=0; z<side; ++z) for (int y=0; y<side; ++y) {
            const auto offset = ((static_cast<std::uint64_t>(z+roiStart)*sourceSide+y+roiStart)*sourceSide+roiStart)*sizeof(float);
            raw.seekg(static_cast<std::streamoff>(offset));
            raw.read(reinterpret_cast<char*>(reload.voxels.data()+(static_cast<std::size_t>(z)*side+y)*side), side*sizeof(float));
            Check(raw.good(), "Reading real ROI failed");
        }
        constexpr float spacing = 0.1537F;
        reload.geometry.dimensions = {side, side, side};
        reload.geometry.spacing = {spacing, spacing, spacing};
        reload.geometry.origin = {roiStart*spacing, roiStart*spacing, roiStart*spacing};
        reload.metadata.identity.datasetId = "real-ct-1440-roi-" + std::to_string(roiStart) + "-" + std::to_string(side);
        reload.metadata.source.kind = ImageSourceKind::Memory;
        reload.metadata.source.uri = "memory://real-ct-roi";
        reload.metadata.attributes.push_back({"parentRaw", std::string(argv[1])});
        reload.metadata.attributes.push_back({"roiStart", static_cast<double>(roiStart)});
        reload.metadata.attributes.push_back({"sourceSide", static_cast<double>(sourceSide)});
        std::cout << std::setprecision(17) << "source=" << argv[1] << " sourceSide=" << sourceSide
            << " roiStart=" << roiStart << " roiSide=" << side << " threshold=" << threshold
            << " spacing=" << spacing << " origin=" << roiStart*spacing << " inputHash="
            << GetHash(reload.voxels.data(), count*sizeof(float)) << '\n';

        HostSessionConfig config;
        config.driveMode = HostDriveMode::HostDriven;
        config.onWorkAvailable = [] {};
        std::mutex taskMutex;
        std::vector<std::function<void()>> tasks;
        config.sendOwnerTask = [&](std::function<void()> task) {
            const std::lock_guard<std::mutex> lock(taskMutex); tasks.push_back(std::move(task)); return true;
        };
        HostRenderViewConfig view;
        view.id = "audit-primary";
        view.role = HostRenderViewRole::Primary3D;
        view.inputMode = HostInputMode::HostInjected;
        view.window.viewInit.viewMode = HostRenderMode::CompositeIsoSurface;
        view.window.viewInit.hasIso = true;
        view.window.viewInit.isoThreshold = threshold;
        auto window = vtkSmartPointer<vtkWin32OpenGLRenderWindow>::New();
        window->SetOffScreenRendering(1);
        window->SetSize(256, 256);
        view.renderWindow = window;
        config.renderViews.push_back(view);
        // 回调状态先于 Session 创建，失败展开时仍存活到 Stop/析构完成。
        std::optional<HostResult> loaded;
        std::optional<PartSegmentationResult> completed;
        VtkAppHostSession session(config);
        Check(session.BuildSession(), "Host BuildSession failed");
        PartSegmentationConfig partConfig;
        partConfig.defaultStart.targetViews.viewIds = {view.id};
        partConfig.defaultStart.threshold = threshold;
        partConfig.defaultStart.minPartVoxels = 32;
        partConfig.maxWorkingBytes = std::size_t{4}*1024*1024*1024;
        partConfig.maxHistoryBytes = std::size_t{2}*1024*1024*1024;
        partConfig.editTimeoutMs = 120000;
        auto feature = std::make_shared<PartSegmentationHostFeature>(partConfig);
        Check(session.AttachFeature(feature), "Attach Part feature failed");
        std::vector<double> tickTimes;
        const auto pump = [&](const auto& ready, Phase& phase) {
            const auto deadline = Clock::now()+std::chrono::seconds(180);
            while (!ready() && Clock::now()<deadline) {
                std::vector<std::function<void()>> current;
                { const std::lock_guard<std::mutex> lock(taskMutex); current.swap(tasks); }
                for (auto& task : current) task();
                const auto tickStart = Clock::now();
                const auto result = session.SendUpdates();
                tickTimes.push_back(std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count());
                Check(result.status != HostUpdateStatus::Failed && result.status != HostUpdateStatus::Stopped,
                    "Host update failed during audit");
                phase.Sample();
                if (!ready()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Check(ready(), "Public operation timed out");
        };
        Phase load;
        Check(session.SendRequestResult(std::move(reload), [&](HostResult r) { loaded=std::move(r); }),
            "Reload not admitted");
        pump([&]{return loaded.has_value();},load);
        Check(loaded->isSucceeded, loaded->message.c_str());
        load.Report("load");
        const auto descriptor = session.GetImageDescriptor();
        Check(descriptor && descriptor->dims == std::array<int,3>{side,side,side}, "Loaded ROI geometry is wrong");
        Phase start;
        PartSegmentationRequest request;
        request.action=PartSegmentationAction::Start;
        Check(feature->SendRequest(request,[&](auto r){completed=std::move(r);}).status==PartAdmissionStatus::Accepted,
            "Part Start not admitted");
        pump([&]{return completed.has_value();},start);
        start.Report("start");
        Check(completed->status==PartResultStatus::Succeeded,completed->message.c_str());
        std::cout << "detail=" << completed->message << '\n';
        auto parts=feature->GetPartSetSnapshot();
        Check(parts && !parts->parts.empty(), "Real ROI produced no retained part");
        auto labels=ReadLabels(session,feature->GetState(),count);
        ReportLabels(labels,*parts,argv[6],"start");
        for (int editIndex=0; editIndex<3; ++editIndex) {
            const auto selected=std::max_element(parts->parts.begin(),parts->parts.end(),[](const auto& a,const auto& b){
                return a.metrics.voxelCount<b.metrics.voxelCount;
            });
            Check(selected!=parts->parts.end() && selected->metrics.voxelCount>=2,"No splittable part");
            const auto first=std::find(labels.begin(),labels.end(),selected->labelId);
            const auto last=std::find(labels.rbegin(),labels.rend(),selected->labelId);
            const auto indexOf=[&](std::size_t index){return std::array<int,3>{static_cast<int>(index%side),
                static_cast<int>((index/side)%side),static_cast<int>(index/(static_cast<std::size_t>(side)*side))};};
            PartSplitEdit split;
            split.target=selected->binding;
            split.seeds={{indexOf(static_cast<std::size_t>(first-labels.begin())),1},
                {indexOf(labels.size()-1-static_cast<std::size_t>(last-labels.rbegin())),2}};
            PartEditRequest edit;
            edit.expectedLabelMap=feature->GetState().labelMap;
            edit.expectedCatalogRevision=parts->catalogRevision;
            edit.operation=split;
            completed.reset();
            Phase previewPhase;
            Check(feature->SendEditRequest(edit,[&](auto r){completed=std::move(r);}).status==PartAdmissionStatus::Accepted,
                "Split not admitted");
            pump([&]{return completed.has_value();},previewPhase);
            previewPhase.Report(("split"+std::to_string(editIndex)).c_str());
            Check(completed->status==PartResultStatus::PreviewReady,completed->message.c_str());
            std::cout << "detail=" << completed->message << '\n';
            auto preview=feature->GetEditPreview();
            Check(preview && preview->labels,"Split preview missing");
            completed.reset();
            Phase commit;
            Check(feature->SetEditCommit(preview->previewId,[&](auto r){completed=std::move(r);}).status==PartAdmissionStatus::Accepted,
                "Split commit not admitted");
            pump([&]{return completed.has_value();},commit);
            commit.Report(("commit"+std::to_string(editIndex)).c_str());
            Check(completed->status==PartResultStatus::Succeeded,completed->message.c_str());
            parts=feature->GetPartSetSnapshot();
            labels=ReadLabels(session,feature->GetState(),count);
            Check(labels==*preview->labels,"Public committed labels differ from the preview");
            ReportLabels(labels,*parts,argv[6],"split"+std::to_string(editIndex));
        }
        std::sort(tickTimes.begin(),tickTimes.end());
        std::cout << "tickSamples=" << tickTimes.size() << " tickP95Ms="
            << tickTimes[static_cast<std::size_t>(0.95*(tickTimes.size()-1))] << '\n';
        Phase stop;
        Check(session.DetachFeature(*feature),"Detach failed");
        Check(session.Stop(),"Host Stop failed");
        stop.Report("stop");
        std::cout << "[PASS] real ROI public load/start/three splits/commit/read/stop\n";
        return 0;
    }
    catch(const std::exception& e) { std::cerr << "[FAIL] " << e.what() << '\n'; return 1; }
}
