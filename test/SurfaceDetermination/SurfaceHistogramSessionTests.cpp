#include "SurfaceDeterminationTestCases.h"
#include "Host/VtkAppHostSession.h"
#include "Host/SurfaceDeterminationHostFeature.h"
#include "Platform/MemMappedFile.h"

#include <vtkWin32OpenGLRenderWindow.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void Require(const bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
struct StopGuard final {
    VtkAppHostSession& session;
    std::mutex& ownerMutex;
    std::vector<std::function<void()>>& ownerTasks;
    std::chrono::steady_clock::time_point deadline;
    ~StopGuard() {
        while (!session.Stop() && std::chrono::steady_clock::now()<deadline) {
            std::vector<std::function<void()>> ready;
            {std::lock_guard<std::mutex> lock(ownerMutex);ready.swap(ownerTasks);}
            for (auto& task:ready) {
                try {task();} catch (...) {std::cerr<<"Real-session owner task failed during cleanup\n";}
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // 失败的隔离测试进程不能带着悬空的owner投递器继续展开栈。
        if (!session.GetIsStopped()) {
            std::cerr<<"Real-session cleanup failed; terminating the isolated test process\n"<<std::flush;
            std::_Exit(EXIT_FAILURE);
        }
    }
};
}

int GetSurfaceHistogramSessionResult(const int argc, char* argv[])
{
    Require(argc==11,"--real-histogram-session float32-raw nx ny nz spacingMm originLpsX originLpsY originLpsZ timeoutMs");
    const std::array<int,3> dimensions{std::stoi(argv[3]),std::stoi(argv[4]),std::stoi(argv[5])};
    const auto spacing=std::stod(argv[6]);
    const std::array<double,3> origin{std::stod(argv[7]),std::stod(argv[8]),std::stod(argv[9])};
    const auto started=std::chrono::steady_clock::now();
    const auto timeoutMs=std::stoull(argv[10]);
    const auto maxTimeout=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max()-started).count();
    Require(std::isfinite(spacing) && spacing>0 && timeoutMs>0
        && timeoutMs<=static_cast<unsigned long long>(maxTimeout),"Invalid real-session geometry or safety timeout");
    const auto timeout=std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeoutMs));
    const auto deadline=started+timeout;
    std::size_t count=1;
    for (const auto n:dimensions) {
        Require(n>=2 && count<=std::numeric_limits<std::size_t>::max()/static_cast<std::size_t>(n),"Invalid real-session dimensions");
        count*=static_cast<std::size_t>(n);
    }
    Require(count<=std::numeric_limits<std::size_t>::max()/sizeof(float),"Real-session bytes overflow");
    for (const auto value:origin) Require(std::isfinite(value),"Non-finite real-session origin");
    MemMappedFile raw;
    Require(raw.Load(argv[2]) && raw.GetSize()==count*sizeof(float),"Real-session RAW does not match its declared layout");
    const auto* values=static_cast<const float*>(raw.GetData());
    std::mutex ownerMutex;
    std::vector<std::function<void()>> ownerTasks;
    HostSessionConfig config;
    config.driveMode=HostDriveMode::HostDriven;
    config.onWorkAvailable=[]{};
    config.sendOwnerTask=[&](std::function<void()> task) {
        std::lock_guard<std::mutex> lock(ownerMutex);
        ownerTasks.push_back(std::move(task));
        return true;
    };
    const std::array<HostRenderMode,4> modes{HostRenderMode::IsoSurface,HostRenderMode::SliceTopDown,
        HostRenderMode::SliceFrontBack,HostRenderMode::SliceLeftRight};
    const std::array<HostRenderViewRole,4> roles{HostRenderViewRole::Primary3D,HostRenderViewRole::TopDownSlice,
        HostRenderViewRole::FrontBackSlice,HostRenderViewRole::LeftRightSlice};
    for (std::size_t i=0;i<modes.size();++i) {
        HostRenderViewConfig view;
        view.id="surface-histogram-real-"+std::to_string(i);
        view.role=roles[i];
        view.inputMode=HostInputMode::HostInjected;
        view.window.viewInit.viewMode=modes[i];
        auto window=vtkSmartPointer<vtkWin32OpenGLRenderWindow>::New();
        window->SetOffScreenRendering(1);
        window->SetSize(64,64);
        window->SetMultiSamples(0);
        view.renderWindow=window;
        config.renderViews.push_back(std::move(view));
    }
    // 回调槽先于 Session 声明，异常退出时也要活到 Stop 完成。
    std::optional<HostResult> loaded;
    std::optional<SurfaceDeterminationResult> terminal;
    unsigned completions=0;
    VtkAppHostSession session(std::move(config));
    StopGuard stop{session,ownerMutex,ownerTasks,deadline};
    Require(session.BuildSession(),"Real HostSession construction failed");
    const auto pump=[&] {
        std::vector<std::function<void()>> ready;
        { std::lock_guard<std::mutex> lock(ownerMutex); ready.swap(ownerTasks); }
        for (auto& task:ready) task();
        Require(session.SendUpdates().status!=HostUpdateStatus::Stopped,"Real Host stopped unexpectedly");
    };
    const auto wait=[&](const auto& completed) {
        while (!completed() && std::chrono::steady_clock::now()<deadline) {
            pump();
            if (!completed()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(completed(),"Real-session safety deadline expired");
    };
    HostLoadRequest load;
    load.filePath=argv[2];
    load.geometry.dimensions=dimensions;
    load.geometry.spacing={spacing,spacing,spacing};
    load.geometry.origin=origin;
    load.metadata.identity.datasetId="surface-histogram-real-roi";
    load.metadata.source.kind=ImageSourceKind::RawFile;
    load.metadata.source.uri=argv[2];
    Require(session.SendRequest(std::move(load),[&](HostResult result){loaded=std::move(result);}).status
        ==HostAdmissionStatus::Accepted,"Real RAW load was not admitted");
    wait([&]{return loaded.has_value();});
    Require(loaded->isSucceeded,"Real RAW load did not complete");
    const auto descriptor=session.GetImageDescriptor();
    Require(descriptor && descriptor->dims==dimensions && descriptor->valueType==ImageValueType::Float32
        && descriptor->componentCount==1,"Real loaded scalar descriptor differs from input");
    for (std::size_t axis=0;axis<3;++axis) {
        const auto expected=axis<2 ? -origin[axis]-(dimensions[axis]-1)*spacing : origin[axis];
        // 只约束浮点几何运输的舍入，不作为物理精度容差。
        const auto tolerance=8*std::numeric_limits<float>::epsilon()*(1+std::abs(origin[axis])+dimensions[axis]*spacing);
        Require(std::abs(descriptor->spacing[axis]-spacing)<=tolerance && std::abs(descriptor->origin[axis]-expected)<=tolerance,
            "Real ROI geometry was not preserved through LPS/RAS loading");
    }
    ImageReadRequest read;
    read.region=ImageReadRegion{{0,0,0},{2,1,1}};
    const auto probe=session.GetImageReadResult(read);
    Require(probe.error==ImageReadError::None && probe.state && probe.state->values
        && probe.state->values->size()==2*sizeof(float),"Real scalar readback failed");
    const auto lastRow=static_cast<std::size_t>(dimensions[1]-1)*dimensions[0];
    const std::array<float,2> expectedProbe{values[lastRow+dimensions[0]-1],values[lastRow+dimensions[0]-2]};
    Require(std::memcmp(expectedProbe.data(),probe.state->values->data(),sizeof(expectedProbe))==0,
        "Real RAW values or orientation changed on loading");
    SurfaceDeterminationConfig surfaceConfig;
    surfaceConfig.isOverlayVisible=false;
    auto feature=std::make_shared<SurfaceDeterminationHostFeature>(surfaceConfig);
    Require(session.AttachFeature(feature),"Real Surface feature attachment failed");
    SurfaceDeterminationStartParams params;
    params.sourceVolume=descriptor->dataRevision;
    params.targetViews.viewIds={"surface-histogram-real-0"};
    params.modelUnit="mm";
    SurfaceStartRequest request;
    request.start=params;
    Require(feature->SendRequest(request,[&](SurfaceDeterminationResult result){terminal=std::move(result);++completions;}).status
        ==SurfaceAdmissionStatus::Accepted,"Real Surface request was not admitted");
    wait([&]{return terminal.has_value();});
    Require(terminal->status==SurfaceResultStatus::Succeeded && terminal->isPublished,"Real Surface request did not publish a result");
    const auto snapshot=feature->GetSurfaceSnapshot();
    Require(snapshot && snapshot->sourceRevision==descriptor->dataRevision && snapshot->isoEstimate
        && snapshot->points && !snapshot->points->empty() && snapshot->triangleIndices && !snapshot->triangleIndices->empty(),
        "Real published Surface result has missing or mismatched data");
    const auto& estimate=*snapshot->isoEstimate;
    std::array<std::uint64_t,512> independent{};
    std::uint64_t excluded=0;
    for (std::size_t i=0;i<count;++i) {
        const auto value=static_cast<double>(values[i]);
        if (!std::isfinite(value) || value<estimate.histogramEdges.front() || value>estimate.histogramEdges.back()) {++excluded;continue;}
        // 用公开边界查区间，独立于产品的缩放取整实现。
        const auto edge=std::upper_bound(estimate.histogramEdges.begin()+1,estimate.histogramEdges.end()-1,value);
        ++independent[static_cast<std::size_t>(edge-(estimate.histogramEdges.begin()+1))];
    }
    Require(independent==estimate.histogramCounts && excluded==estimate.excludedSampleCount
        && estimate.sampleCount+excluded==count && estimate.histogramSampleSteps==std::array<int,3>{1,1,1},
        "Real published histogram does not conserve the original ROI values");
    for (const auto& point:*snapshot->points)
        for (const auto value:point.positionModel) Require(std::isfinite(value),"Real Surface has a non-finite position");
    for (const auto index:*snapshot->triangleIndices) Require(index<snapshot->points->size(),"Real Surface topology is out of range");
    pump();pump();
    Require(completions==1,"Real Surface completion was repeated");
    Require(session.DetachFeature(*feature),"Real Surface detach failed");
    Require(session.Stop() && session.GetIsStopped(),"Real HostSession did not stop cleanly");
    std::cout<<std::setprecision(17)<<"real-session PASS samples="<<estimate.sampleCount<<" excluded="<<excluded
        <<" background="<<estimate.backgroundValue<<" material="<<estimate.materialValue<<" iso="<<estimate.isoValue
        <<" points="<<snapshot->points->size()<<" triangles="<<snapshot->triangleIndices->size()/3<<'\n';
    return 0;
}
