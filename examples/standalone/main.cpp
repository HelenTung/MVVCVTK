#define _CRT_SECURE_NO_WARNINGS

#ifndef MVVCVTK_CMAKE_AUTOINIT
#include <vtkAutoInit.h>
#endif
#include <vtkSMPTools.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkProp.h>
#include <vtkPropCollection.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkWindowToImageFilter.h>

#include "Host/CropHostFeature.h"
#include "Host/GapHostFeature.h"
#include "Host/HostFeature.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/VtkAppHostSession.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef MVVCVTK_CMAKE_AUTOINIT
VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
VTK_MODULE_INIT(vtkRenderingFreeType);
#endif

namespace {

class DragAudit final {
public:
    bool Start(VtkAppHostSession& session)
    {
        constexpr std::array<std::string_view, 2> viewIds{
            "primary-3d", "composite-volume"
        };
        bool isPassed = true;
        for (const auto viewId : viewIds) {
            const auto* endpoint =
                session.GetRenderViewEndpoint(std::string(viewId));
            isPassed = endpoint
                && StartView(*endpoint)
                && isPassed;
        }
        return isPassed;
    }

private:
    using Clock = std::chrono::steady_clock;

    struct RenderProbe final {
        std::optional<Clock::time_point> start;
        std::vector<double> samplesMs;
    };

    static void OnRender(
        vtkObject* caller,
        const unsigned long eventId,
        void* clientData,
        void*)
    {
        auto* probe = static_cast<RenderProbe*>(clientData);
        auto* renderWindow = vtkRenderWindow::SafeDownCast(caller);
        if (!probe || !renderWindow) return;
        if (eventId == vtkCommand::StartEvent) {
            probe->start = Clock::now();
            return;
        }
        if (eventId != vtkCommand::EndEvent || !probe->start) {
            return;
        }
        renderWindow->WaitForCompletion();
        probe->samplesMs.push_back(
            std::chrono::duration<double, std::milli>(
                Clock::now() - *probe->start).count());
        probe->start.reset();
    }

    static double GetPercentile(
        std::vector<double> samples,
        const double percentile)
    {
        if (samples.empty()) return 0.0;
        std::sort(samples.begin(), samples.end());
        const auto index = static_cast<std::size_t>(
            std::clamp(percentile, 0.0, 1.0)
            * static_cast<double>(samples.size() - 1));
        return samples[index];
    }

    static std::vector<unsigned char> GetPixels(
        const HostRenderViewEndpoint& endpoint)
    {
        if (!endpoint.renderWindow) return {};
        endpoint.renderWindow->Render();
        endpoint.renderWindow->WaitForCompletion();
        vtkNew<vtkWindowToImageFilter> capture;
        capture->SetInput(endpoint.renderWindow);
        capture->SetInputBufferTypeToRGB();
        capture->ReadFrontBufferOff();
        capture->ShouldRerenderOff();
        capture->Update();
        auto* output = capture->GetOutput();
        auto* pixels = output
            ? static_cast<unsigned char*>(
                output->GetScalarPointer())
            : nullptr;
        const vtkIdType valueCount = output
            ? output->GetNumberOfPoints()
                * output->GetNumberOfScalarComponents()
            : 0;
        return pixels && valueCount > 0
            ? std::vector<unsigned char>(
                pixels, pixels + valueCount)
            : std::vector<unsigned char>{};
    }

    static bool GetVisualValid(
        const std::vector<unsigned char>& background,
        const std::vector<unsigned char>& pixels)
    {
        if (pixels.size() != background.size()
            || pixels.size() < 3
            || pixels.size() % 3 != 0) {
            return false;
        }

        constexpr int signalThreshold = 8;
        std::size_t signalPixelCount = 0;
        for (std::size_t index = 0;
            index < pixels.size(); index += 3) {
            int pixelDifference = 0;
            for (std::size_t component = 0;
                component < 3; ++component) {
                pixelDifference = std::max(
                    pixelDifference,
                    std::abs(
                        static_cast<int>(pixels[index + component])
                        - static_cast<int>(
                            background[index + component])));
            }
            if (pixelDifference > signalThreshold) {
                ++signalPixelCount;
            }
        }
        const std::size_t pixelCount = pixels.size() / 3;
        return signalPixelCount * 200 > pixelCount;
    }

    static bool StartView(const HostRenderViewEndpoint& endpoint)
    {
        if (!endpoint.renderer || !endpoint.renderWindow
            || !endpoint.interactor) {
            return false;
        }

        endpoint.renderer->ResetCamera();
        const auto warmupStart = Clock::now();
        constexpr int warmupCount = 2;
        for (int index = 0; index < warmupCount; ++index) {
            endpoint.renderWindow->Render();
            endpoint.renderWindow->WaitForCompletion();
        }
        const double warmupMs =
            std::chrono::duration<double, std::milli>(
                Clock::now() - warmupStart).count();

        std::vector<std::pair<vtkProp*, int>> propStates;
        auto* props = endpoint.renderer->GetViewProps();
        if (props) {
            props->InitTraversal();
            while (auto* prop = props->GetNextProp()) {
                propStates.emplace_back(
                    prop, prop->GetVisibility());
                prop->VisibilityOff();
            }
        }
        const auto backgroundPixels = GetPixels(endpoint);
        for (const auto& propState : propStates) {
            if (propState.first) {
                propState.first->SetVisibility(propState.second);
            }
        }
        const auto beforePixels = GetPixels(endpoint);

        RenderProbe probe;
        auto callback = vtkSmartPointer<vtkCallbackCommand>::New();
        callback->SetClientData(&probe);
        callback->SetCallback(&DragAudit::OnRender);
        const unsigned long startTag =
            endpoint.renderWindow->AddObserver(
                vtkCommand::StartEvent, callback);
        const unsigned long endTag =
            endpoint.renderWindow->AddObserver(
                vtkCommand::EndEvent, callback);
        if (startTag == 0 || endTag == 0) {
            if (startTag != 0) {
                endpoint.renderWindow->RemoveObserver(startTag);
            }
            if (endTag != 0) {
                endpoint.renderWindow->RemoveObserver(endTag);
            }
            callback->SetClientData(nullptr);
            return false;
        }

        const int* windowSize = endpoint.renderWindow->GetSize();
        const int centerX = windowSize ? windowSize[0] / 2 : 300;
        const int centerY = windowSize ? windowSize[1] / 2 : 300;
        endpoint.interactor->SetEventPosition(centerX, centerY);
        endpoint.interactor->InvokeEvent(
            vtkCommand::LeftButtonPressEvent);
        probe.samplesMs.clear();
        probe.start.reset();
        constexpr int dragSamples = 30;
        for (int index = 0; index < dragSamples; ++index) {
            endpoint.interactor->SetEventPosition(
                centerX + 4 + index % 10 * 3,
                centerY + 3 + index % 7 * 2);
            endpoint.interactor->InvokeEvent(
                vtkCommand::MouseMoveEvent);
        }
        const auto dragSamplesMs = probe.samplesMs;
        endpoint.renderWindow->RemoveObserver(startTag);
        endpoint.renderWindow->RemoveObserver(endTag);
        callback->SetClientData(nullptr);

        const auto duringPixels = GetPixels(endpoint);
        endpoint.interactor->InvokeEvent(
            vtkCommand::LeftButtonReleaseEvent);
        const auto afterPixels = GetPixels(endpoint);

        const double p50Ms = GetPercentile(dragSamplesMs, 0.50);
        const double p95Ms = GetPercentile(dragSamplesMs, 0.95);
        const double maxMs = GetPercentile(dragSamplesMs, 1.00);
        const bool isBeforeVisible =
            GetVisualValid(backgroundPixels, beforePixels);
        const bool isDuringVisible =
            GetVisualValid(backgroundPixels, duringPixels);
        const bool isAfterVisible =
            GetVisualValid(backgroundPixels, afterPixels);
        const bool isRenderValid =
            dragSamplesMs.size()
                >= static_cast<std::size_t>(dragSamples)
            && p95Ms > 0.0
            && p95Ms <= 33.0;
        const bool isVisualValid = isBeforeVisible
            && isDuringVisible && isAfterVisible;
        std::cout
            << "AUDIT_DRAG: view=" << endpoint.id
            << " warmup_ms=" << warmupMs
            << " samples=" << dragSamplesMs.size()
            << " p50_ms=" << p50Ms
            << " p95_ms=" << p95Ms
            << " max_ms=" << maxMs
            << " visible=" << isBeforeVisible << ','
            << isDuringVisible << ',' << isAfterVisible
            << " render_ok=" << isRenderValid
            << " visual_ok=" << isVisualValid
            << '\n';
        return isRenderValid && isVisualValid;
    }
};

bool GetArgFound(
    const int argc,
    char* argv[],
    const std::string_view expected)
{
    for (int index = 1; index < argc; ++index) {
        if (argv[index] && expected == argv[index]) return true;
    }
    return false;
}

bool StopEventLoop(VtkAppHostSession& session)
{
    const auto* endpoint = session.GetRenderViewEndpoint(
        "slice-top-down");
    const bool isInteractorStopped = endpoint && endpoint->interactor;
    if (isInteractorStopped) endpoint->interactor->TerminateApp();
#if defined(_WIN32)
    // Win32 的 VTK Start() 会重置 pre-start Done；提前投递 WM_QUIT，
    // 可让随后进入的 GetMessage() 立即退出，也兼容已启动的事件循环。
    PostQuitMessage(0);
#endif
    return isInteractorStopped;
}

class MainControlFeature final
    : public HostFeature,
      public std::enable_shared_from_this<MainControlFeature> {
public:
    MainControlFeature(
        VtkAppHostSession& session,
        HostViewTarget target,
        HostViewTargets inputViews,
        std::weak_ptr<GapHostFeature> gapFeature,
        GapHostStartParams gapStart)
        : m_session(session),
          m_target(std::move(target)),
          m_inputViews(std::move(inputViews)),
          m_gapFeature(std::move(gapFeature)),
          m_gapStart(std::move(gapStart)),
          m_keys{
              HostKeyChord{ 'c' },
              HostKeyChord{ 'c', {}, false, false, true },
              HostKeyChord{ 'v' },
              HostKeyChord{ 'v', {}, false, false, true },
              HostKeyChord{ 'l' },
              HostKeyChord{ 'l', {}, false, false, true },
              HostKeyChord{ 'g' }
          }
    {
    }

    std::string_view GetFeatureId() const noexcept override
    {
        return featureId;
    }

    bool AttachHost(const HostFeatureContext& context) override
    {
        if (m_isAttached || !context.host) return false;
        const auto weakOwner = weak_from_this();
        if (weakOwner.expired()) return false;

        m_host = context.host;
        HostInputBinding binding;
        binding.featureId = std::string(featureId);
        binding.targetViews = m_inputViews;
        binding.onInput = [weakOwner](
            const InteractionEvent& event) {
            const auto owner = weakOwner.lock();
            return owner
                ? owner->OnInput(event)
                : InteractionResult{};
        };
        if (!m_host->AttachInput(std::move(binding))) {
            m_host.reset();
            return false;
        }
        m_isAttached = true;
        return true;
    }

    bool DetachHost() override
    {
        if (!m_isAttached) return true;
        if (m_host
            && !m_host->DetachInput(featureId)) {
            return false;
        }
        m_isKeyDown.fill(false);
        m_host.reset();
        m_isAttached = false;
        return true;
    }

    bool OnHostTick() override
    {
        return SendQualityAudit();
    }

    bool StartQualityAudit()
    {
        if (m_qualityAuditPhase != QualityAuditPhase::None) return false;
        m_qualityAuditPhase = QualityAuditPhase::SetLow;
        m_qualityAuditTicks = 0;
        return true;
    }

    bool GetQualityAuditDone() const noexcept
    {
        return m_qualityAuditPhase == QualityAuditPhase::Done;
    }

    bool GetQualityAuditPassed() const noexcept
    {
        return GetQualityAuditDone() && m_isQualityAuditPassed;
    }

private:
    enum class ControlAction : std::uint8_t {
        ColorUp,
        ColorDown,
        OpacityUp,
        OpacityDown,
        QualityNext,
        QualityPrevious,
        StartGap,
        Count
    };

    enum class QualityAuditPhase : std::uint8_t {
        None,
        SetLow,
        WaitLow,
        SetHigh,
        WaitHigh,
        SetXHigh,
        WaitXHigh,
        SetUltra,
        WaitUltra,
        Done
    };

    static constexpr std::string_view featureId =
        "main.tf-quality-controls";
    static constexpr std::size_t actionCount =
        static_cast<std::size_t>(ControlAction::Count);

    static bool GetCharMatched(
        const InteractionEvent& event,
        const char keyCode)
    {
        if (keyCode == 0) return false;
        const char upper = keyCode >= 'a' && keyCode <= 'z'
            ? static_cast<char>(keyCode - 'a' + 'A')
            : keyCode;
        return event.keyCode == keyCode
            || event.keyCode == upper
            || event.keySym == std::string(1, keyCode)
            || event.keySym == std::string(1, upper);
    }

    static bool GetChordMatched(
        const InteractionEvent& event,
        const HostKeyChord& chord)
    {
        const bool hasKey = GetCharMatched(
            event, chord.keyCode)
            || (!chord.keySym.empty()
                && event.keySym == chord.keySym);
        return hasKey
            && event.isCtrlDown == chord.isCtrlDown
            && event.isAltDown == chord.isAltDown
            && event.isShiftDown == chord.isShiftDown;
    }

    std::optional<ControlAction> GetAction(
        const InteractionEvent& event) const
    {
        for (std::size_t index = 0;
            index < m_keys.size(); ++index) {
            if (GetChordMatched(event, m_keys[index])) {
                return static_cast<ControlAction>(index);
            }
        }
        return std::nullopt;
    }

    static std::size_t GetEditIndex(
        const std::size_t nodeCount)
    {
        return nodeCount > 2 ? nodeCount - 2 : nodeCount - 1;
    }

    bool SendViewRequest(HostViewSetRequest request)
    {
        // ViewSet 是传输函数和质量偏好的唯一同步写入入口。
        return m_session.SendRequest(std::move(request));
    }

    bool SetQuality(const HostVolumeQuality quality)
    {
        HostViewSetRequest request;
        request.targetView = m_target;
        request.volumeQuality = quality;
        return SendViewRequest(std::move(request));
    }

    bool StartAuditRender()
    {
        const auto* endpoint = m_session.GetRenderViewEndpoint(
            m_target.viewId);
        if (!endpoint || !endpoint->renderWindow) return false;
        endpoint->renderWindow->Render();
        endpoint->renderWindow->WaitForCompletion();
        return true;
    }

    bool StopQualityAudit(
        const bool isPassed,
        const HostVolumeQuality appliedQuality)
    {
        m_isQualityAuditPassed = isPassed && StopEventLoop(m_session);
        m_qualityAuditPhase = QualityAuditPhase::Done;
        std::cout
            << "AUDIT_QUALITY: passed=" << m_isQualityAuditPassed
            << " applied=" << static_cast<int>(appliedQuality)
            << " high=" << m_isHighApplied
            << " xhigh=" << m_isXHighApplied
            << " ultra=" << m_isUltraApplied
            << '\n' << std::flush;
        return m_isQualityAuditPassed;
    }

    bool SendQualityAudit()
    {
        if (m_qualityAuditPhase == QualityAuditPhase::None
            || m_qualityAuditPhase == QualityAuditPhase::Done) {
            return true;
        }
        const auto state = m_session.GetRenderViewState(m_target);
        if (!state) {
            return StopQualityAudit(false, HostVolumeQuality::Auto);
        }

        constexpr int tickLimit = 10;
        switch (m_qualityAuditPhase) {
        case QualityAuditPhase::SetLow:
            if (!SetQuality(HostVolumeQuality::Low)) {
                return StopQualityAudit(false, state->volumeQuality);
            }
            m_qualityAuditPhase = QualityAuditPhase::WaitLow;
            m_qualityAuditTicks = 0;
            return true;
        case QualityAuditPhase::WaitLow:
            if (state->volumeQuality == HostVolumeQuality::Low) {
                if (!StartAuditRender()) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                m_lastAuditQuality = HostVolumeQuality::Low;
                m_qualityAuditPhase = QualityAuditPhase::SetHigh;
                m_qualityAuditTicks = 0;
                return true;
            }
            if (++m_qualityAuditTicks > tickLimit) {
                return StopQualityAudit(false, state->volumeQuality);
            }
            return true;
        case QualityAuditPhase::SetHigh:
            if (!SetQuality(HostVolumeQuality::High)) {
                return StopQualityAudit(false, state->volumeQuality);
            }
            m_qualityAuditPhase = QualityAuditPhase::WaitHigh;
            m_qualityAuditTicks = 0;
            return true;
        case QualityAuditPhase::WaitHigh:
            if (state->volumeQuality == HostVolumeQuality::High) {
                if (!StartAuditRender()) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                m_isHighApplied = true;
                m_lastAuditQuality = HostVolumeQuality::High;
                m_qualityAuditPhase = QualityAuditPhase::SetXHigh;
                m_qualityAuditTicks = 0;
                return true;
            }
            if (++m_qualityAuditTicks <= tickLimit) return true;
            if (state->volumeQuality != m_lastAuditQuality
                || !StartAuditRender()) {
                return StopQualityAudit(false, state->volumeQuality);
            }
            m_qualityAuditPhase = QualityAuditPhase::SetXHigh;
            m_qualityAuditTicks = 0;
            return true;
        case QualityAuditPhase::SetXHigh:
            if (!SetQuality(HostVolumeQuality::XHigh)) {
                return StopQualityAudit(false, state->volumeQuality);
            }
            m_qualityAuditPhase = QualityAuditPhase::WaitXHigh;
            m_qualityAuditTicks = 0;
            return true;
        case QualityAuditPhase::WaitXHigh:
            if (state->volumeQuality == HostVolumeQuality::XHigh) {
                if (!StartAuditRender()) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                m_isXHighApplied = true;
                m_lastAuditQuality = HostVolumeQuality::XHigh;
                m_qualityAuditPhase = QualityAuditPhase::SetUltra;
                m_qualityAuditTicks = 0;
                return true;
            }
            if (++m_qualityAuditTicks <= tickLimit) return true;
            if (state->volumeQuality != m_lastAuditQuality
                || !StartAuditRender()) {
                return StopQualityAudit(false, state->volumeQuality);
            }
            m_qualityAuditPhase = QualityAuditPhase::SetUltra;
            m_qualityAuditTicks = 0;
            return true;
        case QualityAuditPhase::SetUltra:
            if (!SetQuality(HostVolumeQuality::Ultra)) {
                return StopQualityAudit(false, state->volumeQuality);
            }
            m_qualityAuditPhase = QualityAuditPhase::WaitUltra;
            m_qualityAuditTicks = 0;
            return true;
        case QualityAuditPhase::WaitUltra:
            if (state->volumeQuality == HostVolumeQuality::Ultra) {
                if (!StartAuditRender()) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                m_isUltraApplied = true;
                m_lastAuditQuality = HostVolumeQuality::Ultra;
                return StopQualityAudit(
                    m_isHighApplied
                        && m_isXHighApplied
                        && m_isUltraApplied,
                    state->volumeQuality);
            }
            if (++m_qualityAuditTicks <= tickLimit) return true;
            if (state->volumeQuality != m_lastAuditQuality
                || !StartAuditRender()) {
                return StopQualityAudit(false, state->volumeQuality);
            }
            return StopQualityAudit(
                m_isHighApplied
                    && m_isXHighApplied
                    && m_isUltraApplied,
                state->volumeQuality);
        default:
            return true;
        }
    }

    bool SetTransfer(const ControlAction action)
    {
        const auto state =
            m_session.GetRenderViewState(m_target);
        if (!state) return false;

        const auto& current =
            state->volumeTransferFunction;
        auto next = current;
        std::string_view valueName;
        double nextValue = 0.0;
        switch (action) {
        case ControlAction::ColorUp:
        case ControlAction::ColorDown: {
            if (next.colorNodes.empty()) return false;
            auto& node = next.colorNodes[
                GetEditIndex(next.colorNodes.size())];
            const double delta =
                action == ControlAction::ColorUp ? 0.05 : -0.05;
            nextValue = std::clamp(node.r + delta, 0.0, 1.0);
            if (nextValue == node.r) return true;
            node.r = nextValue;
            valueName = "color.r";
            break;
        }
        case ControlAction::OpacityUp:
        case ControlAction::OpacityDown: {
            if (next.opacityNodes.empty()) return false;
            auto& node = next.opacityNodes[
                GetEditIndex(next.opacityNodes.size())];
            const double delta =
                action == ControlAction::OpacityUp ? 0.05 : -0.05;
            nextValue = std::clamp(
                node.opacity + delta, 0.0, 1.0);
            if (nextValue == node.opacity) return true;
            node.opacity = nextValue;
            valueName = "opacity";
            break;
        }
        default:
            return false;
        }

        HostViewSetRequest request;
        request.targetView = m_target;
        request.volumeTransferFunction = std::move(next);
        if (!SendViewRequest(std::move(request))) {
            return false;
        }
        std::cout << "[TF] " << valueName
            << '=' << nextValue << '\n';
        return true;
    }

    static std::size_t GetQualityIndex(
        const HostVolumeQuality quality)
    {
        constexpr std::array<HostVolumeQuality, 5> qualities{
            HostVolumeQuality::Auto,
            HostVolumeQuality::Low,
            HostVolumeQuality::High,
            HostVolumeQuality::XHigh,
            HostVolumeQuality::Ultra
        };
        const auto found = std::find(
            qualities.begin(), qualities.end(), quality);
        return found != qualities.end()
            ? static_cast<std::size_t>(found - qualities.begin())
            : 0;
    }

    bool SwitchQuality(const int direction)
    {
        const auto state =
            m_session.GetRenderViewState(m_target);
        if (!state || (direction != -1 && direction != 1)) {
            return false;
        }

        constexpr std::array<HostVolumeQuality, 5> qualities{
            HostVolumeQuality::Auto,
            HostVolumeQuality::Low,
            HostVolumeQuality::High,
            HostVolumeQuality::XHigh,
            HostVolumeQuality::Ultra
        };
        constexpr std::array<std::string_view, 5> qualityNames{
            "Auto", "Low", "High", "XHigh", "Ultra"
        };
        const int currentIndex = static_cast<int>(
            GetQualityIndex(state->volumeQuality));
        const int nextIndex = (
            currentIndex + direction
            + static_cast<int>(qualities.size()))
            % static_cast<int>(qualities.size());

        HostViewSetRequest request;
        request.targetView = m_target;
        request.volumeQuality =
            qualities[static_cast<std::size_t>(nextIndex)];
        if (!SendViewRequest(std::move(request))) return false;

        std::cout << "[Quality] "
            << qualityNames[static_cast<std::size_t>(nextIndex)]
            << '\n';
        return true;
    }

    bool SetGapStatus(const std::string& status)
    {
        if (!m_host || m_target.viewId.empty()) {
            return false;
        }
        const std::vector<std::string> statusViews{
            m_target.viewId
        };
        return m_host->SetViewStatus(statusViews, status);
    }

public:
    bool StartGap()
    {
        const auto gapFeature = m_gapFeature.lock();
        if (!gapFeature) {
            (void)SetGapStatus("Gap: unavailable");
            std::cerr
                << "[GapAnalysis] G request rejected: feature unavailable\n"
                << std::flush;
            return false;
        }

        GapHostRequest request;
        request.action = GapHostAction::Start;
        request.start = m_gapStart;
        const auto gapOwner = m_gapFeature;
        const auto controlOwner = weak_from_this();
        const bool isAccepted = gapFeature->SendRequest(
            std::move(request),
            [gapOwner, controlOwner](const bool isSuccess) {
                const auto completedFeature = gapOwner.lock();
                std::ostringstream status;
                status << "Gap: "
                    << (isSuccess ? "succeeded" : "failed");
                if (isSuccess && completedFeature) {
                    const auto statistics =
                        completedFeature->GetState().statistics;
                    status << " | void="
                        << statistics.voidVoxelCount
                        << " | object="
                        << statistics.objectVoxelCount
                        << " | porosity="
                        << statistics.porosityRatio;
                }
                if (const auto owner = controlOwner.lock()) {
                    (void)owner->SetGapStatus(status.str());
                }
                std::cerr << "[GapAnalysis] "
                    << status.str() << '\n' << std::flush;
            });
        const std::string requestStatus = isAccepted
            ? "Gap: running"
            : "Gap: rejected";
        (void)SetGapStatus(requestStatus);
        std::cerr << "[GapAnalysis] G request "
            << (isAccepted ? "accepted; calculation started"
                           : "rejected; calculation did not start")
            << '\n' << std::flush;
        return isAccepted;
    }

private:
    bool SendControl(const ControlAction action)
    {
        switch (action) {
        case ControlAction::ColorUp:
        case ControlAction::ColorDown:
        case ControlAction::OpacityUp:
        case ControlAction::OpacityDown:
            return SetTransfer(action);
        case ControlAction::QualityNext:
            return SwitchQuality(1);
        case ControlAction::QualityPrevious:
            return SwitchQuality(-1);
        case ControlAction::StartGap:
            return StartGap();
        default:
            return false;
        }
    }

    InteractionResult OnInput(
        const InteractionEvent& event)
    {
        const auto action = GetAction(event);
        if (!action) return {};
        const auto index = static_cast<std::size_t>(*action);

        if (event.eventKind
            == InteractionEventKind::KeyRelease) {
            const bool wasDown = m_isKeyDown[index];
            m_isKeyDown[index] = false;
            return wasDown
                ? InteractionResult{ true, true }
                : InteractionResult{};
        }
        if (event.eventKind
            == InteractionEventKind::TextInput) {
            return m_isKeyDown[index]
                ? InteractionResult{ true, true }
                : InteractionResult{};
        }
        if (event.eventKind
            != InteractionEventKind::KeyPress) {
            return {};
        }
        if (m_isKeyDown[index]) {
            return { true, true };
        }

        m_isKeyDown[index] = true;
        const bool isSucceeded = SendControl(*action);
        return {
            true,
            true,
            isSucceeded,
            isSucceeded
                ? InteractionFailureReason::None
                : InteractionFailureReason::StateRejected
        };
    }

    VtkAppHostSession& m_session;
    HostViewTarget m_target;
    HostViewTargets m_inputViews;
    std::weak_ptr<GapHostFeature> m_gapFeature;
    GapHostStartParams m_gapStart;
    std::array<HostKeyChord, actionCount> m_keys;
    std::array<bool, actionCount> m_isKeyDown{};
    std::shared_ptr<FeatureHostControl> m_host;
    bool m_isAttached = false;
    QualityAuditPhase m_qualityAuditPhase = QualityAuditPhase::None;
    HostVolumeQuality m_lastAuditQuality = HostVolumeQuality::Auto;
    int m_qualityAuditTicks = 0;
    bool m_isQualityAuditPassed = false;
    bool m_isHighApplied = false;
    bool m_isXHighApplied = false;
    bool m_isUltraApplied = false;
};

HostRenderViewConfig BuildView(
    std::string id,
    const HostRenderViewRole role,
    HostWindowConfig window,
    const bool isEventLoopEnabled = false)
{
    HostRenderViewConfig view;
    view.id = std::move(id);
    view.role = role;
    view.window = std::move(window);
    view.isEventLoopEnabled = isEventLoopEnabled;
    return view;
}

std::vector<HostRenderViewConfig> BuildViews()
{
    HostWindowConfig composite;
    composite.title = "Window E: Composite Volume";
    composite.width = 600;
    composite.height = 600;
    composite.posX = 660;
    composite.posY = 50;
    composite.viewInit.viewMode =
        HostRenderMode::CompositeVolume;
    composite.viewInit.background = { 0.08, 0.08, 0.12 };
    composite.viewInit.hasBackground = true;

    HostWindowConfig topDown;
    topDown.title = "Window B: Top_down Slice";
    topDown.width = 400;
    topDown.height = 400;
    topDown.posX = 50;
    topDown.posY = 660;
    topDown.viewInit.viewMode =
        HostRenderMode::SliceTopDown;
    topDown.viewInit.background = { 0.0, 0.0, 0.0 };
    topDown.viewInit.hasBackground = true;

    HostWindowConfig frontBack = topDown;
    frontBack.title = "Window C: Front_back Slice";
    frontBack.posX = 460;
    frontBack.viewInit.viewMode =
        HostRenderMode::SliceFrontBack;

    HostWindowConfig leftRight = topDown;
    leftRight.title = "Window D: Left_right Slice";
    leftRight.posX = 870;
    leftRight.viewInit.viewMode =
        HostRenderMode::SliceLeftRight;

    HostWindowConfig primary;
    primary.title = "Window A: Composite IsoSurface";
    primary.width = 600;
    primary.height = 600;
    primary.posX = 50;
    primary.posY = 50;
    primary.isAxesVisible = true;
    primary.viewInit.viewMode =
        HostRenderMode::CompositeIsoSurface;
    primary.viewInit.material = {
        0.3, 0.6, 0.2, 15.0, 0.4, false };
    primary.viewInit.background = {
        0.05, 0.05, 0.05 };
    primary.viewInit.hasBackground = true;

    std::vector<HostRenderViewConfig> views;
    views.push_back(BuildView(
        "primary-3d",
        HostRenderViewRole::Primary3D,
        std::move(primary)));
    views.push_back(BuildView(
        "composite-volume",
        HostRenderViewRole::Composite3D,
        std::move(composite)));
    views.push_back(BuildView(
        "slice-top-down",
        HostRenderViewRole::TopDownSlice,
        std::move(topDown),
        true));
    views.push_back(BuildView(
        "slice-front-back",
        HostRenderViewRole::FrontBackSlice,
        std::move(frontBack)));
    views.push_back(BuildView(
        "slice-left-right",
        HostRenderViewRole::LeftRightSlice,
        std::move(leftRight)));
    return views;
}

HostViewTargets GetAllViews(
    const std::vector<HostRenderViewConfig>& views)
{
    HostViewTargets targets;
    for (const auto& view : views) {
        if (view.role == HostRenderViewRole::Auxiliary
            || std::find(
                targets.viewRoles.begin(),
                targets.viewRoles.end(),
                view.role) != targets.viewRoles.end()) {
            continue;
        }
        targets.viewRoles.push_back(view.role);
    }
    return targets;
}

HostHotkeyConfig GetHotkeys(
    const HostViewTargets& targets)
{
    HostHotkeyConfig config;
    config.isContextInputEnabled = true;
    config.contextInputViews = targets;
    config.isCommandInputEnabled = true;
    config.commandInputViews = targets;
    config.modelSwitchKey = 'm';
    config.dataExportKey = 's';
    config.sliceExportKey = 't';
    config.exitKeySym = "Escape";
    config.dataExportPath = "F:\\data";
    config.dataExportFormat = HostDataExportFormat::Ply;
    config.sliceExportDir = "F:\\data";
    return config;
}

CropHostConfig BuildCrop(
    const HostViewTargets& targets)
{
    CropHostConfig config;
    config.defaultTarget.referenceView = {
        "", true, HostRenderViewRole::Primary3D };
    config.defaultTarget.targetViews = targets;
    config.defaultTarget.isTargetViewsUsed = true;
    config.defaultTarget.isStatusVisible = true;
    config.inputViews = targets;
    config.keys.box.keyCode = 'o';
    config.keys.plane.keyCode = 'p';
    config.keys.noMode.keyCode = '0';
    config.keys.keepMode.keyCode = '1';
    config.keys.removeMode.keyCode = '2';
    config.keys.buildResult.keyCode = '3';
    config.keys.buildResult.isCtrlDown = true;
    config.keys.restoreOriginal.keyCode = '6';
    config.keys.previous.keyCode = '4';
    config.keys.next.keyCode = '5';
    config.keys.exit.keySym = "Escape";
    for (std::size_t index = 0;
        index < config.keys.nodes.size(); ++index) {
        config.keys.nodes[index].keyCode =
            static_cast<char>('0' + index);
        config.keys.nodes[index].isAltDown = true;
    }
    return config;
}

GapHostConfig GetGapConfig(
    const HostViewTargets& inputViews)
{
    GapHostConfig config;
    // 同一次分析结果同时送入 3D 等值面叠加层与俯视切片叠加层。
    config.defaultStart.targetViews.viewIds = {
        "primary-3d", "slice-top-down"
    };
    config.defaultStart.surface.isoMode =
        GapIsoMode::AbsoluteValue;
    config.defaultStart.surface.absoluteIsoValue = 0.172;
    config.defaultStart.surface.backgroundMean = -1.617f;
    config.defaultStart.surface.materialMean = 0.453f;
    config.defaultStart.voidParams.isFilterEnabled = false;
    config.defaultStart.voidParams.minVolumeMM3 = 0.0;
    config.inputViews = inputViews;
    config.keys.switchOverlay.keyCode = 'j';
    config.keys.exit.keySym = "Escape";
    return config;
}

} // namespace

int main(int argc, char* argv[])
{
    // 后端切换和初始化都不是线程安全 API；必须在任何 Feature worker 启动前完成。
    // 构建若未包含 STDThread，则显式回退 Sequential，保持功能可用。
    const bool isThreaded =
        vtkSMPTools::SetBackend("STDThread");
    if (!isThreaded) {
        (void)vtkSMPTools::SetBackend("Sequential");
    }
    vtkSMPTools::Initialize();

    auto renderViews = BuildViews();
    const HostViewTargets allViews =
        GetAllViews(renderViews);
    HostSessionConfig sessionConfig;
    sessionConfig.renderViews =
        std::move(renderViews);
    VtkAppHostSession session(
        std::move(sessionConfig));
    if (!session.BuildSession()) {
        return 1;
    }

    const HostViewTarget primaryTarget{
        "primary-3d",
        false,
        HostRenderViewRole::Primary3D
    };
    const HostViewTarget volumeTarget{
        "composite-volume",
        false,
        HostRenderViewRole::Composite3D
    };

    // Host 初始化会隐藏 3D 参考平面；加载前为两个组合视图恢复可见位。
    HostVisibilityParams planeVisibility;
    planeVisibility.isPlanes3DVisible = false;
    HostViewSetRequest primaryRequest;
    primaryRequest.targetView = primaryTarget;
    primaryRequest.visibility = planeVisibility;
    if (!session.SendRequest(std::move(primaryRequest))) {
        return 1;
    }

    HostViewSetRequest volumeRequest;
    volumeRequest.targetView = volumeTarget;
    volumeRequest.volumeQuality = HostVolumeQuality::Auto;
    volumeRequest.visibility = planeVisibility;
    if (!session.SendRequest(std::move(volumeRequest))) {
        return 1;
    }

    // 十字线是各 Slice View 的私有展示状态；3D View 请求不会自动传播该可见性位。
    constexpr std::array<HostRenderViewRole, 3> sliceRoles{
        HostRenderViewRole::TopDownSlice,
        HostRenderViewRole::FrontBackSlice,
        HostRenderViewRole::LeftRightSlice
    };
    HostVisibilityParams crosshairVisibility;
    crosshairVisibility.isCrosshairVisible = false;
    for (const auto role : sliceRoles) {
        HostViewSetRequest sliceRequest;
        sliceRequest.targetView = { "", true, role };
        sliceRequest.visibility = crosshairVisibility;
        if (!session.SendRequest(std::move(sliceRequest))) {
            return 1;
        }
    }

    std::vector<std::shared_ptr<HostFeature>> features;
    features.push_back(
        std::make_shared<CropHostFeature>(
            BuildCrop(allViews)));
    auto gapConfig = GetGapConfig(allViews);
    auto gapStart = gapConfig.defaultStart;
    auto gapFeature = std::make_shared<GapHostFeature>(
        std::move(gapConfig));
    features.push_back(gapFeature);
    // G 应在任一 MVVCVTK 窗口获得焦点时都能启动；状态统一显示在 composite-volume 标题栏。
    auto controlViews = allViews;
    auto controlFeature = std::make_shared<MainControlFeature>(
        session,
        volumeTarget,
        std::move(controlViews),
        gapFeature,
        std::move(gapStart));
    features.push_back(controlFeature);
    std::size_t attachedCount = 0;
    bool isTimerAttached = false;
    bool isHotkeyAttached = false;

    const auto clearAttached = [&]() {
        bool isCleared = true;
        if (isHotkeyAttached) {
            if (session.AttachHotkeys({})) {
                isHotkeyAttached = false;
            }
            else {
                isCleared = false;
            }
        }
        if (isTimerAttached) {
            if (session.AttachTimer({})) {
                isTimerAttached = false;
            }
            else {
                isCleared = false;
            }
        }
        while (attachedCount > 0) {
            if (!session.DetachFeature(
                    *features[attachedCount - 1])) {
                isCleared = false;
                break;
            }
            --attachedCount;
        }
        return isCleared;
    };

    for (const auto& feature : features) {
        if (!feature || !session.AttachFeature(feature)) {
            if (!clearAttached()) {
                return 20;
            }
            return 2;
        }
        ++attachedCount;
    }

    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView = {
        "", true, HostRenderViewRole::TopDownSlice };
    if (!session.AttachTimer(timer)) {
        if (!clearAttached()) {
            return 21;
        }
        return 3;
    }
    isTimerAttached = true;

    if (!session.AttachHotkeys(GetHotkeys(allViews))) {
        if (!clearAttached()) {
            return 22;
        }
        return 4;
    }
    isHotkeyAttached = true;

    const bool isDragAudit = GetArgFound(
        argc, argv, "--drag-audit");
    const bool isQualityAudit = GetArgFound(
        argc, argv, "--quality-audit");
    const bool isGapAuto = GetArgFound(
        argc, argv, "--gap-auto");
    if ((isDragAudit && isQualityAudit)
        || (isGapAuto && (isDragAudit || isQualityAudit))) {
        std::cerr
            << "--drag-audit, --quality-audit and --gap-auto "
               "are mutually exclusive\n";
        if (!clearAttached()) return 25;
        features.clear();
        return 8;
    }
    bool isAuditComplete = false;
    bool isAuditPassed = false;

    HostLoadRequest load;
    load.filePath = "F:\\data\\ct\\1536x1536x1536_1440.raw";
    load.geometry.dimensions = { 1536, 1536, 1536 };
    load.geometry.spacing = {
        0.1537f, 0.1537f, 0.1537f };
    load.geometry.origin = { 0.0f, 0.0f, 0.0f };
    if (!session.SendRequestResult(
            std::move(load),
             [&](HostResult result) {
                if (isDragAudit) {
                    isAuditPassed = result.isSucceeded
                        && DragAudit{}.Start(session);
                    isAuditComplete = true;
                }
                else if (isQualityAudit) {
                    isAuditPassed = result.isSucceeded
                        && controlFeature->StartQualityAudit();
                    if (isAuditPassed) return;
                    isAuditComplete = true;
                }
                else if (isGapAuto) {
                    if (!result.isSucceeded) {
                        std::cerr
                            << "[GapAnalysis] data load failed; "
                               "automatic request skipped\n"
                            << std::flush;
                        return;
                    }
                    (void)controlFeature->StartGap();
                    return;
                }
                else {
                    return;
                }
                (void)StopEventLoop(session);
            })) {
        if (!clearAttached()) {
            return 23;
        }
        return 5;
    }

    std::cout
        << "TF/quality controls for composite-volume:\n"
        << "  C / Shift+C: color red +/- 0.05\n"
        << "  V / Shift+V: opacity +/- 0.05\n"
        << "  L / Shift+L: next / previous quality tier\n"
        << "GapAnalysis controls:\n"
        << "  G: analyze and show the result in Window A (3D) "
           "and Window B (slice)\n"
        << "  J: start if inactive; otherwise hide/show Gap overlays\n"
        << "  --gap-auto: start Gap automatically after data loading\n"
        << "  Result statistics are shown in the composite-volume title\n";

    const bool isStarted = session.Start();
    if (isQualityAudit) {
        isAuditComplete = controlFeature->GetQualityAuditDone();
        isAuditPassed = controlFeature->GetQualityAuditPassed();
    }
    const bool isCleared = clearAttached();
    if (!isCleared) {
        return 24;
    }
    features.clear();
    if (!isStarted) return 6;
    if ((isDragAudit || isQualityAudit)
        && (!isAuditComplete || !isAuditPassed)) {
        return 7;
    }
    return 0;
}
