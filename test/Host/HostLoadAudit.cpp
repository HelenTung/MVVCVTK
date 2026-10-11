#include "Host/VtkAppHostSession.h"

#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkDataArray.h>
#include <vtkPointData.h>
#include <vtkInformation.h>
#include <vtkInformationVector.h>
#include <vtkRenderer.h>
#include <vtkVolume.h>
#include <vtkVolumeCollection.h>
#include <vtkCamera.h>
#include <vtkPNGWriter.h>
#include <vtkWindowToImageFilter.h>
#include <vtkWin32OpenGLRenderWindow.h>
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// 仓内测试器从 Windows 进程创建时间计时；原始文件只经实际 Host 请求读取。
// 不预读、不清空系统缓存。首帧计时在额外值核验前截止，CPU 返回不冒充 GPU 呈现。
class LoadAudit final {
    struct Memory final {
        std::uint64_t working=0,privateBytes=0,peakWorking=0,peakCommit=0,gpuLocalPeak=0,gpuNonLocalPeak=0;
        bool hasGpu=false;
    };
    class Sampler final {
    public:
        Sampler() {
            Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
            if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
                SIZE_T largest=0;
                for(UINT i=0;;++i) {
                    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                    if(factory->EnumAdapters1(i,adapter.GetAddressOf())==DXGI_ERROR_NOT_FOUND)break;
                    if(!adapter)break;
                    DXGI_ADAPTER_DESC1 desc{};
                    if(SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && desc.DedicatedVideoMemory>largest) {
                        Microsoft::WRL::ComPtr<IDXGIAdapter3> candidate;
                        if(SUCCEEDED(adapter.As(&candidate))) { m_adapter=candidate;largest=desc.DedicatedVideoMemory; }
                    }
                }
            }
            Sample();
            m_thread=std::thread([this] { while(!m_stopped.load()) { Sample();std::this_thread::sleep_for(std::chrono::milliseconds(50)); } });
        }
        ~Sampler() { m_stopped.store(true);if(m_thread.joinable())m_thread.join(); }
        Memory Read() {
            Sample();std::lock_guard<std::mutex> lock(m_mutex);return m_memory;
        }
    private:
        void Sample() {
            PROCESS_MEMORY_COUNTERS_EX p{};p.cb=sizeof(p);
            if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&p),sizeof(p)))return;
            DXGI_QUERY_VIDEO_MEMORY_INFO local{},nonLocal{};
            const bool hasGpu=m_adapter && SUCCEEDED(m_adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local))
                && SUCCEEDED(m_adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&nonLocal));
            std::lock_guard<std::mutex> lock(m_mutex);
            m_memory.working=p.WorkingSetSize;m_memory.privateBytes=p.PrivateUsage;
            m_memory.peakWorking=std::max(m_memory.peakWorking,static_cast<std::uint64_t>(p.PeakWorkingSetSize));
            m_memory.peakCommit=std::max(m_memory.peakCommit,static_cast<std::uint64_t>(p.PeakPagefileUsage));
            m_memory.hasGpu=m_memory.hasGpu || hasGpu;
            if(hasGpu) { m_memory.gpuLocalPeak=std::max(m_memory.gpuLocalPeak,local.CurrentUsage);m_memory.gpuNonLocalPeak=std::max(m_memory.gpuNonLocalPeak,nonLocal.CurrentUsage); }
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter3> m_adapter;
        Memory m_memory;
        std::mutex m_mutex;
        std::atomic<bool> m_stopped{false};
        std::thread m_thread;
    };
public:
    int Start(int argc, char** argv)
    {
        Check(argc >= 3, "Usage: HostLoadAudit raw sourceSide [Low|High|Ultra] [--range-audit] [--quality-audit] [--geometry spacingMm originX originY originZ] [--output directory] [--view-size pixels] [--frames count] [--timeout-seconds count]");
        bool hasRangeAudit = false;
        bool hasQualityAudit = false;
        double spacing = 0.1537;
        std::array<double, 3> origin{};
        const bool hasQuality = argc >= 4 && std::string(argv[3]).rfind("--", 0) != 0;
        const std::string qualityName = hasQuality ? argv[3] : "Ultra";
        const int firstOption = hasQuality ? 4 : 3;
        for (int i = firstOption; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--range-audit") hasRangeAudit = true;
            else if (option == "--quality-audit") hasQualityAudit = true;
            else if (option == "--geometry") {
                Check(i + 4 < argc, "Geometry requires spacing and three LPS origin components");
                spacing = std::stod(argv[++i]);
                for (auto& value : origin) value = std::stod(argv[++i]);
            } else if(option=="--output") { Check(i+1<argc,"Missing output directory");m_output=std::filesystem::u8path(argv[++i]); }
            else if(option=="--view-size") { Check(i+1<argc,"Missing view size");m_viewSize=std::stoi(argv[++i]); }
            else if(option=="--frames") { Check(i+1<argc,"Missing frame count");m_frameCount=std::stoi(argv[++i]); }
            else if(option=="--timeout-seconds") { Check(i+1<argc,"Missing audit timeout");m_timeout=std::chrono::seconds(std::stoi(argv[++i]));
            } else Check(false, "Unknown audit option");
        }
        Check(m_viewSize>0 && m_frameCount>=0 && m_timeout.count()>0,"Invalid audit settings");
        Check(std::isfinite(spacing) && spacing > 0 && std::all_of(origin.begin(), origin.end(),
            [](double value) { return std::isfinite(value); }), "Invalid physical geometry");
        const std::string filePath = argv[1];
        const int side = std::stoi(argv[2]);
        Check(side > 1 && side <= 8192, "Invalid source dimensions");
        Check(qualityName == "Low" || qualityName == "High" || qualityName == "Ultra", "Invalid quality");
        const auto quality = qualityName == "Low" ? HostVolumeQuality::Low
            : qualityName == "High" ? HostVolumeQuality::High : HostVolumeQuality::Ultra;
        const auto sourceBytes = static_cast<std::uint64_t>(side) * side * side * sizeof(float);
        m_source=filePath;m_side=side;m_spacing=spacing;m_quality=qualityName;m_sourceBytes=sourceBytes;
        if(!m_output.empty())std::filesystem::create_directories(m_output);
        Check(std::filesystem::file_size(filePath) == sourceBytes, "RAW size disagrees with dimensions");
        std::cout << "source=" << filePath << " sourceBytes=" << sourceBytes << " sourceSide=" << side
            << " quality=" << qualityName << " spacingMm=" << spacing
            << " originLPS=" << origin[0] << ',' << origin[1] << ',' << origin[2]
            << " views=4 pixels="<<m_viewSize<<'x'<<m_viewSize<<" fileCache=not-pre-read\n";

        HostSessionConfig config;
        config.driveMode = HostDriveMode::HostDriven;
        config.onWorkAvailable = [] {};
        std::mutex mutex;
        std::vector<std::function<void()>> ownerTasks;
        config.sendOwnerTask = [&](std::function<void()> onTask) {
            std::lock_guard<std::mutex> lock(mutex);
            ownerTasks.push_back(std::move(onTask));
            return true;
        };
        const std::array<HostRenderMode, 4> modes{HostRenderMode::Volume, HostRenderMode::SliceTopDown,
            HostRenderMode::SliceFrontBack, HostRenderMode::SliceLeftRight};
        const std::array<HostRenderViewRole, 4> roles{HostRenderViewRole::Primary3D, HostRenderViewRole::TopDownSlice,
            HostRenderViewRole::FrontBackSlice, HostRenderViewRole::LeftRightSlice};
        HostRenderRequest draw;
        std::vector<vtkSmartPointer<vtkWin32OpenGLRenderWindow>> windows;
        for (std::size_t i = 0; i < modes.size(); ++i) {
            HostRenderViewConfig view;
            view.id = "load-audit-" + std::to_string(i);
            view.role = roles[i];
            view.inputMode = HostInputMode::HostInjected;
            view.window.viewInit.viewMode = modes[i];
            auto window = vtkSmartPointer<vtkWin32OpenGLRenderWindow>::New();
            window->SetOffScreenRendering(1);
            window->SetSize(m_viewSize, m_viewSize);
            window->SetMultiSamples(0);windows.push_back(window);
            view.renderWindow = window;
            draw.viewIds.push_back(view.id);
            config.renderViews.push_back(std::move(view));
        }
        VtkAppHostSession session(config);
        Check(session.BuildSession(), "Session construction failed");
        HostViewSetRequest qualityRequest;
        qualityRequest.targetView.viewId = draw.viewIds.front();
        qualityRequest.volumeQuality = quality;
        Check((session.SendRequest(std::move(qualityRequest)).status == HostAdmissionStatus::Accepted), "Quality request rejected");
        SendPhase("session-ready");

        HostLoadRequest request;
        request.filePath = filePath;
        request.geometry.dimensions = {side, side, side};
        request.geometry.spacing = {spacing, spacing, spacing};
        request.geometry.origin = origin;
        request.metadata.identity.datasetId = "real-ct-load-audit";
        request.metadata.source.kind = ImageSourceKind::RawFile;
        request.metadata.source.uri = filePath;
        std::optional<HostResult> completed;
        const auto sendOwnerWork = [&] {
            std::vector<std::function<void()>> ready;
            { std::lock_guard<std::mutex> lock(mutex); ready.swap(ownerTasks); }
            for (auto& onTask : ready) onTask();
            Check(session.SendUpdates().status != HostUpdateStatus::Stopped, "Host stopped during audit");
            SetMemorySample();
        };
        const auto requestStart = Clock::now();
        Check((session.SendRequest(std::move(request), [&](HostResult result) { completed = std::move(result); }).status == HostAdmissionStatus::Accepted),
            "Load admission failed");
        bool hasAccepted = false;
        while (!completed && Clock::now() - requestStart < m_timeout) {
            sendOwnerWork();
            if (!hasAccepted) {
                const auto state = session.GetStateSnapshot();
                if (state && GetAccepted(*state, 0)) {
                    hasAccepted = true;
                    // 这是 owner 轮询首次观察到数据的时刻，是接纳延迟的上界。
                    SendPhase("accepted-observed", GetElapsedMs(requestStart));
                }
            }
            if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(completed && completed->isSucceeded, "Load and display activation failed");
        m_loadMs=GetElapsedMs(requestStart);
        SendPhase("display-active", GetElapsedMs(requestStart));

        // 时间边界先截止，再做读回/RAW 探针等额外核验。
        const auto renderStart = Clock::now();
        (void)session.SendUpdates();
        const auto rendered = session.SendRender(draw);
        const double renderMs = GetElapsedMs(renderStart);
        m_firstReturnMs=renderMs;
        for(const auto &window:windows)window->WaitForCompletion();
        m_firstGpuMs=GetElapsedMs(renderStart);m_requestToGpuMs=GetElapsedMs(requestStart);
        const double requestToRenderMs = GetElapsedMs(requestStart);
        Check(rendered.status == HostRenderStatus::Rendered && rendered.views.size() == 4, "First four-view render failed");
        SendPhase("first-render-return", requestToRenderMs);
        std::cout << "renderWallMs=" << renderMs << " scalarUploadBytes=unavailable gpuUploadUs=unavailable\n";
        std::cout<<"gpuCompletedFirstFrameMs="<<*m_firstGpuMs<<" requestToGpuCompletedMs="<<*m_requestToGpuMs<<std::endl;

        if(m_frameCount>0) {
            const auto *endpoint=session.GetRenderViewEndpoint(draw.viewIds.front());
            Check(endpoint && endpoint->renderer,"Render endpoint unavailable");
            for(int i=0;i<3+m_frameCount;++i) {
                const auto start=Clock::now();endpoint->renderer->GetActiveCamera()->Azimuth(1);
                endpoint->renderer->ResetCameraClippingRange();
                // 稳态审计主动绘制四个已激活窗口，避免 Host 的 unchanged 路径被当作渲染。
                for(const auto &window:windows) { window->SetDesiredUpdateRate(draw.desiredUpdateRate);window->Render(); }
                for(const auto &window:windows)window->WaitForCompletion();
                const auto elapsed=GetElapsedMs(start);if(i>=3)m_frames.push_back(elapsed);
            }
        }
        m_measured=m_sampler.Read();
        SendPhase("measured-render-complete");
        if(!m_output.empty()) {
            for(std::size_t i=0;i<windows.size();++i) {
                auto capture=vtkSmartPointer<vtkWindowToImageFilter>::New();capture->SetInput(windows[i]);
                capture->ReadFrontBufferOff();capture->Update();
                auto writer=vtkSmartPointer<vtkPNGWriter>::New();const auto path=(m_output/("view-"+std::to_string(i)+".png")).u8string();
                writer->SetFileName(path.c_str());writer->SetInputConnection(capture->GetOutputPort());writer->Write();
            }
        }

        const auto sourceDescriptor = session.GetImageDescriptor();
        Check(bool(sourceDescriptor), "Formal descriptor unavailable");
        const auto getDisplayCheck = [&](const std::string& targetQuality) {
            const auto descriptor = session.GetImageDescriptor();
            Check(descriptor && descriptor->dims == std::array<int, 3>{side, side, side}, "Formal dimensions changed");
            Check(descriptor->dataRevision == sourceDescriptor->dataRevision
                && descriptor->bindingRevision == sourceDescriptor->bindingRevision
                && descriptor->extent == sourceDescriptor->extent
                && descriptor->spacing == sourceDescriptor->spacing
                && descriptor->origin == sourceDescriptor->origin
                && descriptor->direction == sourceDescriptor->direction
                && descriptor->scalarRange == sourceDescriptor->scalarRange
                && descriptor->valueType == sourceDescriptor->valueType
                && descriptor->componentBytes == sourceDescriptor->componentBytes
                && descriptor->componentCount == sourceDescriptor->componentCount,
                "Display quality changed formal data or geometry");
            for (int axis = 0; axis < 3; ++axis) {
                const double expected = axis < 2 ? -double(origin[axis]) - double(side - 1) * spacing : origin[axis];
                Check(descriptor->spacing[axis] == double(spacing)
                    && std::abs(descriptor->origin[axis] - expected) < 1e-5 * std::max(1.0, std::abs(expected)),
                    "Physical spacing or RAS origin changed");
            }
            const auto state = session.GetRenderViewState({draw.viewIds.front()});
            const auto target = targetQuality == "Low" ? HostVolumeQuality::Low
                : targetQuality == "High" ? HostVolumeQuality::High : HostVolumeQuality::Ultra;
            Check(state && state->volumeQuality == target, "Target quality changed");
            const auto* endpoint = session.GetRenderViewEndpoint(draw.viewIds.front());
            Check(endpoint && endpoint->renderer, "Primary endpoint unavailable");
            auto* volumes = endpoint->renderer->GetVolumes();
            volumes->InitTraversal();
            auto* volume = volumes->GetNextVolume();
            auto* mapper = volume ? vtkGPUVolumeRayCastMapper::SafeDownCast(volume->GetMapper()) : nullptr;
            auto* display = mapper ? vtkImageData::SafeDownCast(mapper->GetInput()) : nullptr;
            const int displaySide = targetQuality == "Low" ? (side + 3) / 4 : targetQuality == "High" ? (side + 1) / 2 : side;
            Check(display && display->GetDimensions()[0] == displaySide && display->GetDimensions()[1] == displaySide
                && display->GetDimensions()[2] == displaySide, "Actual display dimensions changed");
            m_displaySide=displaySide;m_scalarRange=descriptor->scalarRange;
            std::cout << "checkedQuality=" << targetQuality << " displayDimensions="
                << displaySide << 'x' << displaySide << 'x' << displaySide << '\n';
            for (int index : {0, side / 2, side - 2}) {
                ImageReadRequest read;
                read.region = ImageReadRegion{{static_cast<std::size_t>(index), static_cast<std::size_t>(index), static_cast<std::size_t>(index)}, {2, 1, 1}};
                const auto result = session.GetImageReadResult(read);
                Check(result.state && result.state->values && result.state->values->size() == 2 * sizeof(float), "Scalar probe failed");
                std::ifstream raw(filePath, std::ios::binary);
                const auto offset = ((static_cast<std::uint64_t>(index) * side + side - 1 - index) * side + side - 2 - index) * sizeof(float);
                raw.seekg(static_cast<std::streamoff>(offset));
                std::array<std::uint32_t, 2> bits{};
                raw.read(reinterpret_cast<char*>(bits.data()), sizeof(bits));
                std::swap(bits[0], bits[1]);
                Check(raw.good() && std::memcmp(bits.data(), result.state->values->data(), sizeof(bits)) == 0, "Scalar bits/orientation changed");
            }
            const auto scenes = session.GetSceneViewStates();
            Check(scenes.size() == 4 && std::all_of(scenes.begin(), scenes.end(), [&](const auto& scene) {
                return scene.isAvailable && scene.presentation && scene.presentation->dataRevision == descriptor->dataRevision;
            }), "View revisions diverged");
            return display;
            };
        auto* display = getDisplayCheck(qualityName);
        if (hasRangeAudit) SendRangeAudit(display);
        struct QualityState final {
            std::optional<HostResult> result;
            int callbackCount = 0;
        };
        std::vector<std::shared_ptr<QualityState>> qualityStates;
        if (hasQualityAudit) {
            // 真正驱动同一 Session 的降档、升档与缓存复用；业务读取始终对照完整 RAW。
            for (const std::string targetName : {"Low", "High", "Ultra", "High", "Low", "Ultra"}) {
                const auto target = targetName == "Low" ? HostVolumeQuality::Low
                    : targetName == "High" ? HostVolumeQuality::High : HostVolumeQuality::Ultra;
                HostViewSetRequest update;
                update.targetView.viewId = draw.viewIds.front();
                update.volumeQuality = target;
                auto completion = std::make_shared<QualityState>();
                qualityStates.push_back(completion);
                const auto started = Clock::now();
                std::cout << "audit=quality-switch target=" << targetName << std::endl;
                Check((session.SendRequest(std::move(update), [completion](HostResult result) {
                    ++completion->callbackCount;
                    completion->result = std::move(result);
                }).status == HostAdmissionStatus::Accepted), "Quality switch admission failed");
                while (!completion->result && Clock::now() - started < std::chrono::minutes(5)) {
                    sendOwnerWork();
                    if (!completion->result) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                Check(completion->result && completion->result->isSucceeded
                    && completion->callbackCount == 1, "Quality switch did not complete exactly once successfully");
                const double switchMs = GetElapsedMs(started);
                const auto drawStart = Clock::now();
                const auto frame = session.SendRender(draw);
                const double drawMs = GetElapsedMs(drawStart);
                Check(frame.status == HostRenderStatus::Rendered && frame.views.size() == 4,
                    "Quality switch four-view render failed");
                (void)getDisplayCheck(targetName);
                std::cout << "audit=quality-switch target=" << targetName << " switchWallMs=" << switchMs
                    << " renderWallMs=" << drawMs << " completionCount=" << completion->callbackCount << std::endl;
                SendPhase("quality-checked", GetElapsedMs(started));
            }
            // 无效目标必须明确失败，并保留上一档的完整可渲染状态。
            HostViewSetRequest invalid;
            invalid.targetView.viewId = draw.viewIds.front();
            invalid.volumeQuality = static_cast<HostVolumeQuality>(99);
            auto rejection = std::make_shared<QualityState>();
            qualityStates.push_back(rejection);
            const auto rejectedStart = Clock::now();
            Check((session.SendRequest(std::move(invalid), [rejection](HostResult result) {
                ++rejection->callbackCount;
                rejection->result = std::move(result);
            }).status == HostAdmissionStatus::Accepted), "Invalid quality request was not handled");
            // 已识别请求的失败也沿 Session 的 owner 完成队列投递。
            while (!rejection->result && Clock::now() - rejectedStart < std::chrono::seconds(5)) {
                sendOwnerWork();
                if (!rejection->result) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Check(rejection->result && !rejection->result->isSucceeded
                && rejection->result->errorCode == HostErrorCode::OperationFailed
                && rejection->callbackCount == 1, "Invalid quality did not reject exactly once");
            sendOwnerWork();
            const auto frame = session.SendRender(draw);
            Check(frame.status == HostRenderStatus::Rendered && frame.views.size() == 4,
                "Invalid quality damaged active rendering");
            (void)getDisplayCheck("Ultra");
        }
        Check(session.Stop(), "Host Stop failed");
        m_afterStop=m_sampler.Read();
        Check(std::all_of(qualityStates.begin(), qualityStates.end(), [](const auto& state) {
            return state->callbackCount == 1;
        }), "Quality callback repeated during cleanup");
        if (hasQualityAudit) {
            std::cout << "[PASS] quality audit: six switches, invalid target rejection, full RAW probes and Stop\n";
        }
        std::cout << "[PASS] fresh-process full RAW load, scalar probes, dimensions and four-view consistency\n";
        SaveReport(true,"");
        return 0;
    }

    void SaveReport(bool succeeded,const std::string &message) {
        if(m_output.empty())return;
        if(!succeeded)m_measured=m_sampler.Read();
        std::ofstream out(m_output/"report.json",std::ios::binary);out<<std::setprecision(12);
        const auto number=[&](const std::optional<double> &value) { if(value)out<<*value;else out<<"null"; };
        out<<"{\n\"status\":"<<std::quoted(succeeded ? "PASS" : "FAIL")<<",\"message\":"<<std::quoted(message)
            <<",\"source\":"<<std::quoted(m_source)<<",\"sourceBytes\":"<<m_sourceBytes<<",\"sourceSide\":"<<m_side
            <<",\"spacingMm\":"<<m_spacing<<",\"quality\":"<<std::quoted(m_quality)<<",\"displaySide\":"<<m_displaySide
            <<",\"partitions\":null"
            <<",\"scalarRange\":["<<m_scalarRange[0]<<','<<m_scalarRange[1]<<"],\"views\":4,\"viewSize\":"<<m_viewSize
            <<",\"loadAndDisplayActiveMs\":";number(m_loadMs);
        out<<",\"firstRenderReturnMs\":";number(m_firstReturnMs);
        out<<",\"firstGpuCompletedFrameMs\":";number(m_firstGpuMs);
        out<<",\"requestToFirstGpuCompletedMs\":";number(m_requestToGpuMs);
        out<<",\"frameTimesMs\":[";for(std::size_t i=0;i<m_frames.size();++i){if(i)out<<',';out<<m_frames[i];}
        out<<"],\"processPeakWorkingSetBytes\":"<<m_measured.peakWorking<<",\"processPeakCommitBytes\":"<<m_measured.peakCommit
            <<",\"processGpuLocalPeakBytes\":";
        if(m_measured.hasGpu && m_measured.gpuLocalPeak)out<<m_measured.gpuLocalPeak;else out<<"null";
        out<<",\"processGpuNonLocalPeakBytes\":";if(m_measured.hasGpu && m_measured.gpuLocalPeak)out<<m_measured.gpuNonLocalPeak;else out<<"null";
        out<<",\"afterStopWorkingSetBytes\":"<<m_afterStop.working<<",\"afterStopPrivateBytes\":"<<m_afterStop.privateBytes
            <<",\"samplingIntervalMs\":50,\"frameTiming\":\"first frame: Host SendRender plus GPU fence; steady frames: four native window Render plus GPU fence; 3 warmup frames, rotating volume camera\""
            <<",\"peakScope\":\"process startup through measured rendering; readback probes and PNG encoding excluded; WDDM per-process GPU usage sampled every 50ms\"\n}\n";
    }

private:
    using Clock = std::chrono::steady_clock;
    SIZE_T m_peakWorking = 0;
    SIZE_T m_peakPrivate = 0;
    Sampler m_sampler;
    Memory m_measured,m_afterStop;
    std::filesystem::path m_output;
    std::string m_source,m_quality;
    int m_side=0,m_displaySide=0,m_viewSize=256,m_frameCount=0;
    float m_spacing=0;
    std::uint64_t m_sourceBytes=0;
    std::array<double,2> m_scalarRange{};
    std::chrono::seconds m_timeout{300};
    std::optional<double> m_loadMs,m_firstReturnMs,m_firstGpuMs,m_requestToGpuMs;
    std::vector<double> m_frames;

    static void Check(bool isValid, const char* message)
    { if (!isValid) throw std::runtime_error(message); }

    static double GetElapsedMs(Clock::time_point start)
    { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

    static std::uint64_t GetTicks(FILETIME value)
    { return (std::uint64_t{value.dwHighDateTime} << 32) | value.dwLowDateTime; }

    void SetMemorySample()
    {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        Check(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != 0,
            "Memory sample failed");
        m_peakWorking = std::max(m_peakWorking, memory.PeakWorkingSetSize);
        m_peakPrivate = std::max(m_peakPrivate, memory.PrivateUsage);
    }

    void SendPhase(const char* phase, double requestMs = 0)
    {
        FILETIME created{}, exited{}, kernel{}, user{}, now{};
        Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0, "Process timing failed");
        GetSystemTimePreciseAsFileTime(&now);
        SetMemorySample();
        std::cout << "phase=" << phase << " processWallMs=" << (GetTicks(now) - GetTicks(created)) / 10000.0
            << " processCpuMs=" << (GetTicks(kernel) + GetTicks(user)) / 10000.0
            << " requestWallMs=" << requestMs << " peakWorkingBytes=" << m_peakWorking
            << " sampledPrivateBytes=" << m_peakPrivate << std::endl;
    }

    static void SendRangeAudit(vtkImageData* image)
    {
        auto* source = image->GetPointData()->GetScalars();
        Check(source && source->GetNumberOfComponents() == 1, "Range audit requires scalar input");
        const auto createView = [&] {
            auto values = vtkSmartPointer<vtkDataArray>::Take(vtkDataArray::CreateDataArray(source->GetDataType()));
            values->SetNumberOfComponents(1);
            values->SetVoidArray(source->GetVoidPointer(0), source->GetNumberOfValues(), 1);
            return values;
        };
        // 两个只读壳借用同一数组，均在当前 Session 活跃期间销毁；不修改原始数据/MTime。
        auto fresh = createView();
        std::array<double, 2> measured{};
        auto start = Clock::now();
        fresh->GetRange(measured.data());
        const auto scanMs = GetElapsedMs(start);
        auto cached = createView();
        auto ranges = vtkSmartPointer<vtkInformationVector>::New();
        ranges->SetNumberOfInformationObjects(1);
        // 只复用同一不可变数组实际算出的范围，不信任外部声明或估计值。
        ranges->GetInformationObject(0)->Set(vtkDataArray::COMPONENT_RANGE(), measured.data(), 2);
        cached->GetInformation()->Set(vtkAbstractArray::PER_COMPONENT(), ranges);
        std::array<double, 2> reused{};
        start = Clock::now();
        cached->GetRange(reused.data());
        const auto cacheMs = GetElapsedMs(start);
        Check(measured == reused, "Cached scalar range changed values");
        std::cout << "diagnostic=scalar-range freshShellWallMs=" << scanMs
            << " cachedShellWallMs=" << cacheMs << " tupleCount=" << source->GetNumberOfTuples()
            << " min=" << measured[0] << " max=" << measured[1] << std::endl;
    }

    template<class Snapshot>
    static auto GetAccepted(const Snapshot& snapshot, int) -> decltype(snapshot.load.acceptedRevision, bool())
    { return GetDataRevisionRefValid(snapshot.load.acceptedRevision); }
    template<class Snapshot>
    static bool GetAccepted(const Snapshot&, long) { return false; }
};

int main(int argc, char** argv)
{
    LoadAudit audit;
    try { return audit.Start(argc, argv); }
    catch (const std::exception& error) { audit.SaveReport(false,error.what());std::cerr << "[FAIL] " << error.what() << '\n'; return 1; }
}
