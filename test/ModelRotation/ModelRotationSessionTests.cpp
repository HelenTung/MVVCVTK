// 测试用途：验证旋转会话的输入、帧提交、取消、恢复以及宿主驱动失败路径。
#include "Host/ModelRotationHostFeature.h"
#include "Host/FeatureModelTransformPort.h"
#include "Host/VtkAppHostSession.h"
#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Contracts/FeatureOverlay.h"
#include <vtkActor.h>
#include <vtkAbstractVolumeMapper.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkObjectFactory.h>
#include <vtkPolyDataMapper.h>
#include <vtkProp3D.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkVolume.h>
#include <chrono>
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

#define VERIFY(value) do { if (!(value)) throw std::runtime_error(#value); } while(false)

// 使用真实 OpenGL 绘制，仅替换平台 timer/event loop，便于确定性泵送原生事件。
class RotationInteractor final : public vtkRenderWindowInteractor {
public:
    static RotationInteractor* New();
    vtkTypeMacro(RotationInteractor, vtkRenderWindowInteractor);
    void Initialize() override { this->Initialized = 1; this->Enabled = 1; }
    void Start() override {}
    int timerId = 0;
protected:
    int InternalCreateTimer(int id, int, unsigned long) override { timerId = id; return id; }
    int InternalDestroyTimer(int) override { return 1; }
};
vtkStandardNewMacro(RotationInteractor);

class RotationProbe final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override { return "rotation.test.probe"; }
    bool AttachHost(const HostFeatureContext& value) override { context = value; return true; }
    bool DetachHost() override { return true; } // 故意保留窄端口，验证 Host 确实撤销其 lease。
    bool OnHostTick() override { return true; }
    HostFeatureContext context;
};

class RotationFailureOverlay final : public FeatureOverlay {
public:
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
    void AttachRenderer(vtkSmartPointer<vtkRenderer>) override {}
    void DetachRenderer(vtkSmartPointer<vtkRenderer>) override {}
    void SetOverlayState(const FeatureOverlayState& state) override
    {
        matrices.push_back(state.modelToWorld);
        if (failures > 0) { --failures; throw std::runtime_error("Injected overlay failure"); }
    }
    int failures = 0;
    std::vector<std::array<double,16>> matrices;
};

void TestRotationSession()
{
    HostSessionConfig config;
    std::vector<vtkSmartPointer<RotationInteractor>> interactors;
    const std::array<HostRenderMode,4> modes{HostRenderMode::IsoSurface,
        HostRenderMode::SliceTopDown,HostRenderMode::SliceFrontBack,HostRenderMode::SliceLeftRight};
    const std::array<HostRenderViewRole,4> roles{HostRenderViewRole::Primary3D,
        HostRenderViewRole::TopDownSlice,HostRenderViewRole::FrontBackSlice,HostRenderViewRole::LeftRightSlice};
    for (int index=0; index<4; ++index) {
        auto window = vtkSmartPointer<vtkRenderWindow>::New();
        window->SetOffScreenRendering(1);
        auto interactor = vtkSmartPointer<RotationInteractor>::New();
        window->SetInteractor(interactor);
        interactor->SetRenderWindow(window);
        interactors.push_back(interactor);
        HostRenderViewConfig view;
        view.id = "rotation-" + std::to_string(index);
        view.role = roles[index];
        view.window.width = 256; view.window.height = 256;
        view.window.viewInit.viewMode = modes[index];
        view.window.viewInit.hasIso = true; view.window.viewInit.isoThreshold = 50;
        view.renderWindow = window;
        view.isEventLoopEnabled = index == 0;
        view.inputMode = index == 0 ? HostInputMode::HostInjected : HostInputMode::NativeInteractor;
        config.renderViews.push_back(view);
    }
    VtkAppHostSession session(std::move(config));
    VERIFY(session.BuildSession());
    const auto probe = std::make_shared<RotationProbe>();
    const auto feature = std::make_shared<ModelRotationHostFeature>();
    VERIFY(!feature->SendRequest({}));
    ModelRotationConfig invalidConfig;
    invalidConfig.targetViews = { {"missing-view"}, {} };
    VERIFY(!session.AttachFeature(std::make_shared<ModelRotationHostFeature>(invalidConfig)));
    VERIFY(session.AttachFeature(probe));
    VERIFY(session.AttachFeature(feature));
    VERIFY(!session.AttachFeature(feature));
    VERIFY(session.Start());
    const auto port = std::dynamic_pointer_cast<FeatureModelTransformPort>(probe->context.host);
    VERIFY(port);
    const auto tick = [&] {
        int id = interactors[0]->timerId;
        VERIFY(id != 0);
        interactors[0]->InvokeEvent(vtkCommand::TimerEvent,&id);
    };
    const auto wait = [&](const auto& ready) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(30);
        do {
            tick();
            if (ready()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("Rotation session deadline exceeded");
    };
    HostReloadRequest reload;
    reload.geometry.dimensions = {16,16,16};
    reload.geometry.spacing = {1,2,3};
    reload.geometry.origin = {10,20,30};
    reload.geometry.direction = {0,-1,0, 1,0,0, 0,0,1};
    reload.metadata.identity.datasetId = "rotation-session";
    reload.metadata.source.kind = ImageSourceKind::Memory;
    reload.metadata.source.uri = "memory://rotation-session";
    reload.voxels.resize(16*16*16,0);
    for (int z=4;z<12;++z) for(int y=4;y<12;++y) for(int x=4;x<12;++x)
        reload.voxels[x+16*(y+16*z)] = 100;
    const auto reloadAgain = reload;
    bool loaded = false;
    VERIFY(session.SendRequest(std::move(reload),[&](bool success){loaded=success;}));
    wait([&]{return loaded;});
    for (int index=0; index<5; ++index) tick();
    const auto dataBefore = probe->context.data->GetPrimaryImage();
    const auto descriptor = session.GetImageDescriptor();
    const auto values = session.GetImageReadState();
    VERIFY(dataBefore && descriptor && values);
    const auto imageTime = dataBefore->image->GetMTime();
    const auto getProduct = [&]() -> vtkDataObject* {
        auto* props = session.GetRenderViewEndpoint("rotation-0")->renderer->GetViewProps();
        props->InitTraversal();
        while (auto* item = props->GetNextProp()) {
            if (auto* volume = vtkVolume::SafeDownCast(item))
                return volume->GetMapper()->GetInputDataObject(0,0);
            if (auto* actor = vtkActor::SafeDownCast(item)) {
                if (auto* mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper()))
                    return mapper->GetInputDataObject(0,0);
            }
        }
        return nullptr;
    };
    const auto* isoProduct = getProduct();
    VERIFY(isoProduct);
    const auto cameraBefore = session.GetSceneViewState({"rotation-0"})->camera;
    const auto initial = *port->GetTransformState();
    ModelRotationRequest numeric;
    numeric.angleDeg = 90;
    VERIFY(feature->SendRequest(numeric));
    VERIFY(feature->GetState().status == ModelRotationStatus::Pending);
    wait([&]{return feature->GetState().status == ModelRotationStatus::Succeeded;});
    const auto rotated = *port->GetTransformState();
    VERIFY(rotated.modelToWorld != initial.modelToWorld);
    VERIFY(rotated.dataRevision == initial.dataRevision && rotated.bindingRevision == initial.bindingRevision);
    VERIFY(dataBefore->image == probe->context.data->GetPrimaryImage()->image);
    VERIFY(imageTime == dataBefore->image->GetMTime());
    VERIFY(getProduct() == isoProduct);
    VERIFY(*values->values == *session.GetImageReadState()->values);
    const auto descriptorAfter = *session.GetImageDescriptor();
    VERIFY(descriptor->spacing == descriptorAfter.spacing && descriptor->origin == descriptorAfter.origin
        && descriptor->direction == descriptorAfter.direction && descriptor->extent == descriptorAfter.extent);
    const auto cameraAfter = session.GetSceneViewState({"rotation-0"})->camera;
    VERIFY(cameraBefore->position == cameraAfter->position && cameraBefore->focalPoint == cameraAfter->focalPoint
        && cameraBefore->viewUp == cameraAfter->viewUp);
    for (const auto& endpoint : session.GetRenderViewEndpoints()) {
        VERIFY(probe->context.views->GetFeaturePort(endpoint.id)->GetModelToWorld() == rotated.modelToWorld);
        bool hasMatrix = false;
        auto* props = endpoint.renderer->GetViewProps();
        props->InitTraversal();
        while (auto* item = props->GetNextProp()) {
            // 标尺等二维 prop 也在同一集合内；继续遍历，检查实际三维模型的矩阵。
            auto* prop = vtkProp3D::SafeDownCast(item);
            if (!prop) continue;
            if (auto* matrix = prop->GetUserMatrix()) {
                bool same = true;
                for (int index=0; index<16; ++index)
                    same = same && std::abs(matrix->GetData()[index]-rotated.modelToWorld[index])<1e-8;
                hasMatrix = hasMatrix || same;
            }
        }
        VERIFY(hasMatrix);
    }
    // 候选投影和旧状态恢复各抛一次；下一 collect 必须重试旧状态，不能丢失 Transform 门铃。
    auto failureOverlay = std::make_shared<RotationFailureOverlay>();
    const auto overlayPort = probe->context.views->GetOverlayPort("rotation-0");
    VERIFY(overlayPort && overlayPort->AttachOverlay(failureOverlay));
    tick();
    failureOverlay->matrices.clear();
    failureOverlay->failures = 2;
    const auto epochBeforeFailure = session.GetSceneViewState({"rotation-0"})->renderedEpoch;
    numeric.angleDeg = 15;
    VERIFY(feature->SendRequest(numeric));
    tick();
    VERIFY(port->GetTransformState()->modelToWorld == rotated.modelToWorld);
    VERIFY(session.GetSceneViewState({"rotation-0"})->renderedEpoch == epochBeforeFailure);
    VERIFY(failureOverlay->matrices.size()==2);
    tick();
    VERIFY(failureOverlay->matrices.size()>=4);
    VERIFY(failureOverlay->matrices[2] == rotated.modelToWorld);
    wait([&]{return feature->GetState().status==ModelRotationStatus::Succeeded;});
    ModelRotationRequest failureUndo; failureUndo.action=ModelRotationAction::Undo;
    VERIFY(feature->SendRequest(failureUndo));
    wait([&]{return feature->GetState().status==ModelRotationStatus::Succeeded;});
    VERIFY(port->GetTransformState()->modelToWorld==rotated.modelToWorld);
    overlayPort->RemoveOverlay(failureOverlay);
    HostViewSetRequest volumeMode;
    volumeMode.targetView.viewId = "rotation-0";
    volumeMode.mode = HostRenderMode::Volume;
    VERIFY(session.SendRequest(std::move(volumeMode)));
    wait([&]{return session.GetRenderViewState({"rotation-0"})->viewMode==HostRenderMode::Volume
        && getProduct()!=nullptr;});
    VERIFY(port->GetTransformState()->modelToWorld == rotated.modelToWorld);
    const auto* volumeProduct = getProduct();
    numeric.angleDeg = 15;
    VERIFY(feature->SendRequest(numeric));
    wait([&]{return feature->GetState().status==ModelRotationStatus::Succeeded;});
    VERIFY(getProduct() == volumeProduct);
    ModelRotationRequest volumeUndo; volumeUndo.action=ModelRotationAction::Undo;
    VERIFY(feature->SendRequest(volumeUndo));
    wait([&]{return feature->GetState().status==ModelRotationStatus::Succeeded;});
    VERIFY(port->GetTransformState()->modelToWorld == rotated.modelToWorld);
    VERIFY(getProduct() == volumeProduct);
    ModelRotationRequest enable;
    enable.action = ModelRotationAction::SetEnabled;
    VERIFY(feature->SendRequest(enable));
    auto* input = session.GetInputEndpoint();
    const auto send = [&](HostInputKind kind,int x,int y,
        bool isShift=false,bool isCtrl=false,bool isAlt=false) {
        HostInputEvent event; event.viewId="rotation-0"; event.kind=kind; event.x=x;event.y=y;
        event.isShiftDown=isShift; event.isCtrlDown=isCtrl; event.isAltDown=isAlt;
        const auto result = input->SendInput(event);
        if (!result.isSucceeded || !result.isDefaultSuppressed)
            throw std::runtime_error("Input " + std::to_string(static_cast<int>(kind))
                + " rejected: " + result.message + " / feature status "
                + std::to_string(static_cast<int>(feature->GetState().status)));
    };
    send(HostInputKind::PrimaryPress,170,128);
    const auto gestureToken = port->GetTransformState()->editToken;
    VERIFY(gestureToken != 0);
    VERIFY(session.SetInputBindings(ConfigurableInputBindings{})
        == InputBindingStatus::Busy);
    const auto renderEpoch = session.GetSceneViewState({"rotation-0"})->renderedEpoch;
    for (int index=0;index<1000;++index) send(HostInputKind::PointerMove,180,130);
    VERIFY(port->GetTransformState()->hasPending);
    VERIFY(session.GetSceneViewState({"rotation-0"})->renderedEpoch == renderEpoch);
    tick();
    const auto preview = port->GetTransformState()->modelToWorld;
    VERIFY(preview != rotated.modelToWorld);
    HostDataExportRequest exportRequest;
    exportRequest.outputPath = "rotation-must-not-export";
    VERIFY(!session.SendRequest(std::move(exportRequest)));
    send(HostInputKind::PrimaryRelease,128,190);
    wait([&]{return feature->GetState().status == ModelRotationStatus::Succeeded;});
    VERIFY(port->GetTransformState()->modelToWorld != preview);
    VERIFY(!session.GetRenderViewState({"rotation-0"})->isInteracting);
    VERIFY(feature->GetState().undoCount == 2);
    ModelRotationRequest undo; undo.action=ModelRotationAction::Undo;
    VERIFY(feature->SendRequest(undo));
    wait([&]{return feature->GetState().status==ModelRotationStatus::Succeeded;});
    VERIFY(port->GetTransformState()->modelToWorld == rotated.modelToWorld);

    ConfigurableInputBindings rotationBindings;
    const auto alt = static_cast<std::uint8_t>(InputModifierFlags::Alt);
    rotationBindings.SetOverride(
        std::string(ModelRotationBindingKeys::EnabledDrag),
        InputBindingOverride::Replace,
        { InputTriggerKind::Drag, InputMouseButton::Secondary, alt, 0, 0 });
    VERIFY(feature->SetInputBindings(rotationBindings)
        == InputBindingStatus::Applied);
    send(HostInputKind::SecondaryPress,170,128,false,false,true);
    VERIFY(port->GetTransformState()->editToken != 0);
    VERIFY(feature->SetInputBindings(ConfigurableInputBindings{})
        == InputBindingStatus::Busy);
    send(HostInputKind::PointerMove,128,180,false,false,true);
    tick();
    send(HostInputKind::SecondaryRelease,128,180,false,false,true);
    wait([&]{return feature->GetState().status == ModelRotationStatus::Succeeded;});
    ModelRotationRequest reboundUndo; reboundUndo.action=ModelRotationAction::Undo;
    VERIFY(feature->SendRequest(reboundUndo));
    wait([&]{return feature->GetState().status == ModelRotationStatus::Succeeded;});
    VERIFY(port->GetTransformState()->modelToWorld == rotated.modelToWorld);
    VERIFY(feature->SetInputBindings(ConfigurableInputBindings{})
        == InputBindingStatus::Applied);
    ConfigurableInputBindings cancelBindings;
    cancelBindings.SetOverride(std::string(ModelRotationBindingKeys::CancelKey),
        InputBindingOverride::Replace,
        { InputTriggerKind::KeyPress, InputMouseButton::None, 0, 0, 'q' });
    VERIFY(feature->SetInputBindings(cancelBindings)
        == InputBindingStatus::Applied);
    send(HostInputKind::PrimaryPress,170,128);
    HostInputEvent key;
    key.viewId="rotation-0";
    key.kind=HostInputKind::KeyPress;
    key.keyCode=27; key.keySym="Escape";
    VERIFY(input->SendInput(key).isSucceeded);
    VERIFY(port->GetTransformState()->editToken != 0);
    key.keyCode='q'; key.keySym="q";
    VERIFY(input->SendInput(key).isSucceeded);
    wait([&]{return feature->GetState().status == ModelRotationStatus::Cancelled;});
    HostInputEvent releaseAfterCancel;
    releaseAfterCancel.viewId="rotation-0";
    releaseAfterCancel.kind=HostInputKind::PrimaryRelease;
    VERIFY(input->SendInput(releaseAfterCancel).isSucceeded);
    VERIFY(feature->SetInputBindings(ConfigurableInputBindings{})
        == InputBindingStatus::Applied);

    // 原生切片 Ctrl+拖拽只由 Feature 消费；Esc 恢复开始姿态，保留既有撤销项。
    auto* native = interactors[1].GetPointer();
    native->SetEventInformation(180,128,1,0);
    native->InvokeEvent(vtkCommand::LeftButtonPressEvent);
    VERIFY(port->GetTransformState()->editToken != 0);
    native->SetEventInformation(128,180,1,0);
    native->InvokeEvent(vtkCommand::MouseMoveEvent);
    tick();
    native->SetKeyEventInformation(0,0,27,0,"Escape");
    native->InvokeEvent(vtkCommand::KeyPressEvent);
    wait([&]{return feature->GetState().status==ModelRotationStatus::Cancelled;});
    VERIFY(port->GetTransformState()->modelToWorld == rotated.modelToWorld);
    VERIFY(feature->GetState().undoCount == 1);
    send(HostInputKind::PrimaryPress,170,128);
    session.GetRenderViewEndpoint("rotation-0")->renderWindow->SetSize(280,256);
    tick();
    wait([&]{return feature->GetState().status==ModelRotationStatus::Cancelled;});
    VERIFY(port->GetTransformState()->editToken==0);

    send(HostInputKind::PrimaryPress,170,128);
    send(HostInputKind::PointerMove,128,180);
    tick();
    loaded=false;
    auto secondReload=reloadAgain;
    VERIFY(session.SendRequest(std::move(secondReload),[&](bool success){loaded=success;}));
    wait([&]{return loaded;});
    VERIFY(feature->GetState().status==ModelRotationStatus::Invalidated);
    VERIFY(feature->GetState().undoCount==0);
    VERIFY(port->GetTransformState()->modelToWorld==rotated.modelToWorld);
    HostInputEvent staleRelease;
    staleRelease.viewId = "rotation-0";
    staleRelease.kind = HostInputKind::PrimaryRelease;
    staleRelease.x = 128; staleRelease.y = 180;
    VERIFY(input->SendInput(staleRelease).isSucceeded);
    VERIFY(port->GetTransformState()->editToken==0);
    VERIFY(port->GetTransformState()->modelToWorld==rotated.modelToWorld);
    // 保留的平移/缩放也使用同一 token；预览有界，Cancel 与换数据不会回写旧起点。
    HostToolSetRequest modelTool;
    modelTool.targetView.viewId="rotation-0";
    modelTool.toolMode=HostToolMode::ModelTransform;
    VERIFY(session.SendRequest(std::move(modelTool)));
    send(HostInputKind::PrimaryPress,170,128,true);
    VERIFY(port->GetTransformState()->editToken!=0);
    send(HostInputKind::PointerMove,200,150,true);
    VERIFY(port->GetTransformState()->modelToWorld==rotated.modelToWorld);
    tick();
    VERIFY(port->GetTransformState()->modelToWorld!=rotated.modelToWorld);
    send(HostInputKind::Cancel,200,150,true);
    tick();
    VERIFY(port->GetTransformState()->modelToWorld==rotated.modelToWorld);
    send(HostInputKind::PrimaryPress,170,128,true,true);
    send(HostInputKind::PointerMove,170,150,true,true);
    tick();
    send(HostInputKind::PrimaryRelease,170,175,true,true);
    wait([&]{return port->GetTransformState()->editToken==0;});
    VERIFY(std::abs(port->GetTransformState()->modelToWorld[1]
        - rotated.modelToWorld[1]*std::exp(0.47))<1e-8);
    const auto restore = port->StartTransform(*port->GetTransformState());
    VERIFY(restore && port->SetTransformCommit(*restore,1,rotated.modelToWorld));
    wait([&]{return port->GetTransformState()->editToken==0;});
    send(HostInputKind::PrimaryPress,170,128,true);
    send(HostInputKind::PointerMove,200,150,true);
    tick();
    loaded=false;
    auto panReload=reloadAgain;
    VERIFY(session.SendRequest(std::move(panReload),[&](bool success){loaded=success;}));
    wait([&]{return loaded;});
    send(HostInputKind::PrimaryRelease,240,170,true);
    VERIFY(port->GetTransformState()->modelToWorld==rotated.modelToWorld);
    VERIFY(!session.GetRenderViewState({"rotation-0"})->isInteracting);
    send(HostInputKind::PrimaryPress,170,128);
    send(HostInputKind::PointerMove,128,180);
    tick();
    VERIFY(session.DetachFeature(*feature));
    tick();
    VERIFY(port->GetTransformState()->modelToWorld==rotated.modelToWorld);
    VERIFY(!session.GetRenderViewState({"rotation-0"})->isInteracting);
    VERIFY(feature->DetachHost());
    VERIFY(!feature->SendRequest(numeric));
    VERIFY(session.AttachFeature(feature));
    VERIFY(feature->SendRequest(enable));
    send(HostInputKind::PrimaryPress,170,128);
    VERIFY(session.DetachFeature(*probe));
    VERIFY(!port->GetTransformState());
    VERIFY(session.Stop());
    VERIFY(session.GetIsStopped());
    VERIFY(session.BuildSession());
    VERIFY(session.AttachFeature(probe));
    VERIFY(session.AttachFeature(feature));
    VERIFY(session.Start());
    const auto rebuiltPort = std::dynamic_pointer_cast<FeatureModelTransformPort>(probe->context.host);
    VERIFY(rebuiltPort && rebuiltPort->GetTransformState());
    VERIFY(rebuiltPort->GetTransformState()->sessionGeneration != initial.sessionGeneration);
    VERIFY(!port->GetTransformState());
    VERIFY(!rebuiltPort->StartTransform(initial));
    loaded=false;
    auto rebuiltReload=reloadAgain;
    VERIFY(session.SendRequest(std::move(rebuiltReload),[&](bool success){loaded=success;}));
    wait([&]{return loaded;});
    VERIFY(feature->SendRequest(numeric));
    wait([&]{return feature->GetState().status==ModelRotationStatus::Succeeded;});
    VERIFY(session.Stop());
    std::cout << "ModelRotation real OpenGL/session/native input passed\n";
}

void TestRotationHostDriven()
{
    auto window = vtkSmartPointer<vtkRenderWindow>::New();
    window->SetOffScreenRendering(1);
    window->SetSize(128, 128);
    auto interactor = vtkSmartPointer<RotationInteractor>::New();
    window->SetInteractor(interactor);
    interactor->SetRenderWindow(window);
    std::atomic<int> notifications{0};
    HostSessionConfig config;
    config.driveMode = HostDriveMode::HostDriven;
    config.onWorkAvailable = [&] { ++notifications; };
    HostRenderViewConfig view;
    view.id = "rotation-driven";
    view.role = HostRenderViewRole::Primary3D;
    view.renderWindow = window;
    view.window.viewInit.viewMode = HostRenderMode::IsoSurface;
    view.window.viewInit.hasIso = true;
    view.window.viewInit.isoThreshold = 50;
    config.renderViews.push_back(view);
    VtkAppHostSession session(std::move(config));
    auto probe = std::make_shared<RotationProbe>();
    auto feature = std::make_shared<ModelRotationHostFeature>();
    VERIFY(session.BuildSession() && session.AttachFeature(probe));
    VERIFY(session.AttachFeature(feature));
    auto port = std::dynamic_pointer_cast<FeatureModelTransformPort>(probe->context.host);
    VERIFY(port && interactor->timerId == 0);
    const auto update = [&] {
        const auto result = session.SendUpdates();
        VERIFY(result.status == HostUpdateStatus::Completed);
        if (!result.renderViewIds.empty())
            VERIFY(session.SendRender({result.renderViewIds, 0.001}).status == HostRenderStatus::Rendered);
    };
    const auto drain = [&] {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto count = notifications.load();
            update();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (notifications.load() == count) return;
        }
        throw std::runtime_error("Rotation work did not settle");
    };
    HostReloadRequest reload;
    reload.geometry.dimensions = {8,8,8};
    reload.geometry.spacing = {1,1,1};
    reload.metadata.identity.datasetId = "rotation-driven";
    reload.metadata.source.kind = ImageSourceKind::Memory;
    reload.metadata.source.uri = "memory://rotation-driven";
    reload.voxels.resize(8*8*8, 0);
    for (int z=2; z<6; ++z) for (int y=2; y<6; ++y) for (int x=2; x<6; ++x)
        reload.voxels[x+8*(y+8*z)] = 100;
    const auto reloadAgain = reload;
    bool loaded = false;
    VERIFY(session.SendRequest(std::move(reload), [&](bool value) { loaded = value; }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!loaded && std::chrono::steady_clock::now() < deadline) {
        update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    VERIFY(loaded);
    drain();
    const auto before = *port->GetTransformState();
    auto count = notifications.load();
    const auto token = port->StartTransform(before);
    VERIFY(token);
    VERIFY(notifications.load() > count);
    drain();
    auto candidate = before.modelToWorld;
    candidate[3] += 2;
    count = notifications.load();
    VERIFY(port->SetTransformPreview(*token, 1, candidate) && notifications.load() > count);
    VERIFY(port->GetTransformState()->modelToWorld == before.modelToWorld);
    drain();
    VERIFY(port->GetTransformState()->modelToWorld == candidate);
    count = notifications.load();
    VERIFY(port->SetTransformCommit(*token, 2, candidate) && notifications.load() > count);
    drain();
    VERIFY(port->GetTransformState()->completion == ModelTransformStatus::Committed);
    const auto cancel = port->StartTransform(*port->GetTransformState());
    VERIFY(cancel);
    drain();
    count = notifications.load();
    VERIFY(port->StopTransform(*cancel) && notifications.load() > count);
    drain();
    VERIFY(port->GetTransformState()->completion == ModelTransformStatus::Cancelled);
    // 合法显式请求在另一工具拖动期间排队；逐次提交，撤销按同一顺序执行。
    const auto sourcePort = probe->context.views->GetFeaturePort("rotation-driven");
    VERIFY(sourcePort);
    const InteractionSource source{ "rotation.test.probe", "drag" };
    VERIFY(sourcePort->SetInteracting(source, true));
    const auto queuedBefore = port->GetTransformState()->modelToWorld;
    ModelRotationRequest numeric;
    numeric.angleDeg = 10;
    numeric.worldCenter = std::array<double, 3>{0, 0, 0};
    for (int index = 0; index < 3; ++index) VERIFY(feature->SendRequest(numeric));
    ModelRotationRequest undo;
    undo.action = ModelRotationAction::Undo;
    for (int index = 0; index < 3; ++index) VERIFY(feature->SendRequest(undo));
    update();
    VERIFY(feature->GetState().status == ModelRotationStatus::Pending);
    VERIFY(port->GetTransformState()->modelToWorld == queuedBefore);
    VERIFY(sourcePort->SetInteracting(source, false));
    drain();
    VERIFY(feature->GetState().status == ModelRotationStatus::Succeeded);
    VERIFY(feature->GetState().undoCount == 0);
    VERIFY(port->GetTransformState()->modelToWorld == queuedBefore);

    // 连续用户操作不受旧的 100 次历史保留上限影响。
    for (int index = 0; index < 101; ++index) VERIFY(feature->SendRequest(numeric));
    for (int attempt = 0; attempt < 300
        && feature->GetState().status == ModelRotationStatus::Pending; ++attempt) update();
    VERIFY(feature->GetState().status == ModelRotationStatus::Succeeded);
    VERIFY(feature->GetState().undoCount == 101);
    for (int index = 0; index < 101; ++index) VERIFY(feature->SendRequest(undo));
    for (int attempt = 0; attempt < 300
        && feature->GetState().status == ModelRotationStatus::Pending; ++attempt) update();
    VERIFY(feature->GetState().status == ModelRotationStatus::Succeeded);
    VERIFY(feature->GetState().undoCount == 0);
    VERIFY(port->GetTransformState()->modelToWorld == queuedBefore);
    // Cancel 清除尚未开始的请求；非法参数仍拒绝，不占据队列。
    VERIFY(sourcePort->SetInteracting(source, true));
    VERIFY(feature->SendRequest(numeric));
    auto invalid = numeric;
    invalid.worldAxis = {0, 0, 0};
    VERIFY(!feature->SendRequest(invalid));
    ModelRotationRequest stop;
    stop.action = ModelRotationAction::Cancel;
    VERIFY(feature->SendRequest(stop));
    VERIFY(feature->GetState().status == ModelRotationStatus::Cancelled);
    VERIFY(sourcePort->SetInteracting(source, false));
    drain();
    VERIFY(port->GetTransformState()->modelToWorld == queuedBefore);

    // 等待期间换源使请求失效，不把旧数据的旋转应用到新数据。
    VERIFY(sourcePort->SetInteracting(source, true));
    VERIFY(feature->SendRequest(numeric));
    loaded = false;
    auto secondReload = reloadAgain;
    VERIFY(session.SendRequest(std::move(secondReload), [&](bool value) { loaded = value; }));
    for (int attempt = 0; attempt < 1000 && !loaded; ++attempt) {
        update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    VERIFY(loaded);
    VERIFY(sourcePort->SetInteracting(source, false));
    drain();
    VERIFY(feature->GetState().status == ModelRotationStatus::Invalidated);
    VERIFY(session.DetachFeature(*probe));
    VERIFY(!port->GetTransformState());
    VERIFY(session.Stop());
}
