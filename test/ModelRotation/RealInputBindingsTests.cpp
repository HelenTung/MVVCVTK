// 显式真实数据用例：从已核验原始 CT 无重采样裁取 128^3 ROI 后运行。
// 数据来源、父/子 SHA-256 与几何假设记录在本工作树 out/real-input-bindings/roi-evidence.json。
#include "Host/VtkAppHostSession.h"
#include "Host/ModelRotationHostFeature.h"

#include <vtkCommand.h>
#include <vtkObjectFactory.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class RealBindingInteractor final : public vtkRenderWindowInteractor {
public:
    static RealBindingInteractor* New();
    vtkTypeMacro(RealBindingInteractor, vtkRenderWindowInteractor);
    void Initialize() override { Initialized = 1; Enabled = 1; }
    void Start() override {}
    int timerId = 0;
protected:
    int InternalCreateTimer(int id, int, unsigned long) override
    {
        timerId = id;
        return id;
    }
    int InternalDestroyTimer(int) override { return 1; }
};
vtkStandardNewMacro(RealBindingInteractor);

void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Near(double first, double second, double tolerance = 1e-5)
{
    return std::abs(first - second) <= tolerance;
}

} // namespace

void TestRealInputBindings(const std::string& rawPath)
{
    const auto file = std::filesystem::u8path(rawPath);
    Expect(std::filesystem::is_regular_file(file), "Real ROI file missing");
    Expect(std::filesystem::file_size(file) == 128ULL * 128 * 128 * sizeof(float),
        "Real ROI must be float32 128^3");

    HostSessionConfig config;
    std::vector<vtkSmartPointer<RealBindingInteractor>> interactors;
    for (int index = 0; index < 2; ++index) {
        auto window = vtkSmartPointer<vtkRenderWindow>::New();
        window->SetOffScreenRendering(1);
        auto interactor = vtkSmartPointer<RealBindingInteractor>::New();
        window->SetInteractor(interactor);
        interactor->SetRenderWindow(window);
        interactors.push_back(interactor);

        HostRenderViewConfig view;
        view.id = index == 0 ? "real-primary" : "real-slice";
        view.role = index == 0
            ? HostRenderViewRole::Primary3D : HostRenderViewRole::TopDownSlice;
        view.window.width = 256;
        view.window.height = 256;
        view.window.viewInit.viewMode = index == 0
            ? HostRenderMode::Volume : HostRenderMode::SliceTopDown;
        view.renderWindow = window;
        view.isEventLoopEnabled = index == 0;
        view.inputMode = HostInputMode::HostInjected;
        config.renderViews.push_back(std::move(view));
    }

    VtkAppHostSession session(std::move(config));
    Expect(session.BuildSession(), "Real binding Session build failed");
    const auto rotation = std::make_shared<ModelRotationHostFeature>();
    Expect(session.AttachFeature(rotation), "Real binding rotation attach failed");
    Expect(session.Start(), "Real binding Session start failed");
    const auto tick = [&] {
        int id = interactors[0]->timerId;
        Expect(id != 0, "Real binding timer unavailable");
        interactors[0]->InvokeEvent(vtkCommand::TimerEvent, &id);
    };
    const auto wait = [&](const auto& ready) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(60);
        do {
            tick();
            if (ready()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("Real binding Session deadline exceeded");
    };

    HostLoadRequest load;
    load.filePath = rawPath;
    load.geometry.dimensions = { 128, 128, 128 };
    load.geometry.spacing = { 0.1537f, 0.1537f, 0.1537f };
    // 原始采集的 origin/direction 未独立确认；这里沿用已归档的
    // LPS identity/zero 假设，仅验证相对交互状态，不判定绝对物理坐标。
    const float assumedOrigin = 704.0f * 0.1537f;
    load.geometry.origin = { assumedOrigin, assumedOrigin, assumedOrigin };
    load.metadata.identity.datasetId = "ct-1536-center-128-input-bindings";
    load.metadata.source.kind = ImageSourceKind::RawFile;
    load.metadata.source.uri = rawPath;

    std::optional<HostResult> loaded;
    Expect(session.SendRequestResult(std::move(load),
        [&](HostResult result) { loaded = std::move(result); }),
        "Real binding RAW load admission failed");
    wait([&] { return loaded.has_value(); });
    if (!loaded->isSucceeded)
        throw std::runtime_error("Real binding RAW load failed: " + loaded->message);
    wait([&] {
        const auto state = session.GetRenderViewState({ "real-slice" });
        return state && GetDataEntityIdValid(state->dataRevision.entityId);
    });

    const auto descriptor = session.GetImageDescriptor();
    Expect(descriptor && descriptor->dims == std::array<int, 3>{ 128, 128, 128 },
        "Real binding image dimensions changed");
    Expect(descriptor->valueType == ImageValueType::Float32
            && descriptor->componentCount == 1,
        "Real binding image scalar format changed");
    Expect(Near(descriptor->scalarRange[0], -0.5046715140342712)
            && Near(descriptor->scalarRange[1], 0.18885990977287292),
        "Real binding ROI scalar range differs from verified source");

    auto* input = session.GetInputEndpoint();
    Expect(input != nullptr, "Real binding Host input endpoint missing");
    const auto send = [&](HostInputKind kind, int x = 128, int y = 128,
        bool isAlt = false) {
        HostInputEvent event;
        event.viewId = "real-slice";
        event.kind = kind;
        event.x = x;
        event.y = y;
        event.isAltDown = isAlt;
        const auto result = input->SendInput(event);
        if (!result.isSucceeded)
            throw std::runtime_error("Real binding input failed: " + result.message);
        return result;
    };
    const auto getState = [&] {
        const auto state = session.GetRenderViewState({ "real-slice" });
        Expect(state.has_value(), "Real binding slice state missing");
        return *state;
    };

    const auto before = getState();
    Expect(send(HostInputKind::WheelForward).isDefaultSuppressed,
        "Default wheel must be handled by SDK");
    const auto advanced = getState();
    const double stepMm = 5.0 * 0.1537f;
    Expect(Near(std::abs(advanced.cursorWorld[2] - before.cursorWorld[2]),
            stepMm, 1e-3),
        "Default wheel did not move five real-data slices");

    ConfigurableInputBindings wheelBindings;
    Expect(wheelBindings.SetOverride(
        std::string(NavigationBindingKeys::SliceForward),
        InputBindingOverride::Replace,
        { InputTriggerKind::WheelBackward, InputMouseButton::None, 0, 0, 0 }),
        "Forward wheel override rejected");
    Expect(wheelBindings.SetOverride(
        std::string(NavigationBindingKeys::SliceBackward),
        InputBindingOverride::Replace,
        { InputTriggerKind::WheelForward, InputMouseButton::None, 0, 0, 0 }),
        "Backward wheel override rejected");
    Expect(session.SetInputBindings(wheelBindings) == InputBindingStatus::Applied,
        "Real binding wheel swap not applied");
    Expect(send(HostInputKind::WheelForward).isDefaultSuppressed,
        "Swapped wheel must be handled by SDK");
    const auto reversed = getState();
    for (int axis = 0; axis < 3; ++axis)
        Expect(Near(reversed.cursorWorld[axis], before.cursorWorld[axis], 1e-3),
            "Swapped wheel did not restore real-data cursor");

    ConfigurableInputBindings dragBindings;
    const auto alt = static_cast<std::uint8_t>(InputModifierFlags::Alt);
    Expect(dragBindings.SetOverride(
        std::string(NavigationBindingKeys::WindowLevelDrag),
        InputBindingOverride::Replace,
        { InputTriggerKind::Drag, InputMouseButton::Secondary, alt, 0, 0 }),
        "Window-level drag override rejected");
    Expect(dragBindings.SetOverride(
        std::string(NavigationBindingKeys::ZoomDrag),
        InputBindingOverride::Replace,
        { InputTriggerKind::Drag, InputMouseButton::Secondary, 0, alt, 0 }),
        "Zoom drag override rejected");
    Expect(session.SetInputBindings(dragBindings) == InputBindingStatus::Applied,
        "Real binding drag rules not applied");
    const auto originalWindow = getState().windowLevel;
    Expect(send(HostInputKind::SecondaryPress, 100, 100, true).isDefaultSuppressed,
        "Rebound drag press not captured");
    Expect(getState().isInteracting, "Rebound drag did not start interaction");
    Expect(send(HostInputKind::PointerMove, 150, 120, true).isDefaultSuppressed,
        "Rebound drag move not captured");
    const auto changedWindow = getState().windowLevel;
    Expect(!Near(changedWindow.windowWidth, originalWindow.windowWidth)
            || !Near(changedWindow.windowCenter, originalWindow.windowCenter),
        "Rebound drag did not change window/level on real data");
    (void)send(HostInputKind::PrimaryRelease, 150, 120, true);
    Expect(getState().isInteracting,
        "Unrelated primary release ended rebound secondary drag");
    Expect(send(HostInputKind::SecondaryRelease, 150, 120, true).isDefaultSuppressed,
        "Rebound drag release not captured");
    Expect(!getState().isInteracting,
        "Rebound drag left interaction state active");

    ModelRotationRequest enableRotation;
    enableRotation.action = ModelRotationAction::SetEnabled;
    Expect(rotation->SendRequest(enableRotation),
        "Real binding rotation enable rejected");
    ConfigurableInputBindings rotationBindings;
    Expect(rotationBindings.SetOverride(
        std::string(ModelRotationBindingKeys::EnabledDrag),
        InputBindingOverride::Replace,
        { InputTriggerKind::Drag, InputMouseButton::Secondary, alt, 0, 0 }),
        "Real binding rotation drag override rejected");
    Expect(rotationBindings.SetOverride(
        std::string(ModelRotationBindingKeys::CancelKey),
        InputBindingOverride::Replace,
        { InputTriggerKind::KeyPress, InputMouseButton::None, 0, 0, 'q' }),
        "Real binding rotation cancel override rejected");
    Expect(rotation->SetInputBindings(rotationBindings)
            == InputBindingStatus::Applied,
        "Real binding rotation overrides not applied");

    const auto send3D = [&](HostInputKind kind, int x, int y,
        bool isAlt, char keyCode = 0, const char* keySym = "") {
        HostInputEvent event;
        event.viewId = "real-primary";
        event.kind = kind;
        event.x = x;
        event.y = y;
        event.isAltDown = isAlt;
        event.keyCode = keyCode;
        event.keySym = keySym;
        const auto result = input->SendInput(event);
        if (!result.isSucceeded)
            throw std::runtime_error("Real rotation input failed: "
                + result.message);
        return result;
    };
    const auto undoBefore = rotation->GetState().undoCount;
    Expect(send3D(HostInputKind::SecondaryPress, 170, 128, true)
            .isDefaultSuppressed,
        "Real rotation rebound press not captured");
    Expect(rotation->GetState().status == ModelRotationStatus::Dragging,
        "Real rotation rebound press did not start gesture");
    Expect(send3D(HostInputKind::PointerMove, 128, 180, true)
            .isDefaultSuppressed,
        "Real rotation rebound move not captured");
    tick();
    Expect(send3D(HostInputKind::SecondaryRelease, 128, 180, true)
            .isDefaultSuppressed,
        "Real rotation rebound release not captured");
    wait([&] {
        return rotation->GetState().status == ModelRotationStatus::Succeeded;
    });
    Expect(rotation->GetState().undoCount == undoBefore + 1,
        "Real rotation rebound gesture did not commit an undo item");

    Expect(send3D(HostInputKind::SecondaryPress, 170, 128, true)
            .isDefaultSuppressed,
        "Real rotation cancel gesture did not start");
    (void)send3D(HostInputKind::KeyPress, 170, 128, false, 27, "Escape");
    Expect(rotation->GetState().status == ModelRotationStatus::Dragging,
        "Old Escape binding cancelled the real rotation gesture");
    (void)send3D(HostInputKind::KeyPress, 170, 128, false, 'q', "q");
    wait([&] {
        return rotation->GetState().status == ModelRotationStatus::Cancelled;
    });
    (void)send3D(HostInputKind::SecondaryRelease, 170, 128, true);
    Expect(rotation->GetState().undoCount == undoBefore + 1,
        "Cancelled real rotation changed committed history");
    Expect(session.Stop(), "Real binding Session stop failed");

    std::cout << std::setprecision(17)
        << "real_input_bindings source=ct-1536-center-128"
        << " scalar_min=" << descriptor->scalarRange[0]
        << " scalar_max=" << descriptor->scalarRange[1]
        << " cursor_z_before=" << before.cursorWorld[2]
        << " cursor_z_forward=" << advanced.cursorWorld[2]
        << " cursor_z_swapped=" << reversed.cursorWorld[2]
        << " window_width_before=" << originalWindow.windowWidth
        << " window_width_rebound=" << changedWindow.windowWidth
        << " rotation_commit=PASS rotation_cancel=PASS"
        << " status=PASS\n";
}
