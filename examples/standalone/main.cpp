#define _CRT_SECURE_NO_WARNINGS

#ifndef MVVCVTK_CMAKE_AUTOINIT
#include <vtkAutoInit.h>
#endif
#include <vtkSMPTools.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkPNGWriter.h>
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
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
#include "Host/PartSegmentationHostFeature.h"
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
#include "Host/SurfaceDeterminationHostFeature.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iomanip>
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

    bool GetKeyMatched(
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

    bool GetChordMatched(
        const InteractionEvent& event,
        const HostKeyChord& chord)
    {
        const bool hasKey = GetKeyMatched(event, chord.keyCode)
            || (!chord.keySym.empty() && event.keySym == chord.keySym);
        return hasKey
            && event.isCtrlDown == chord.isCtrlDown
            && event.isAltDown == chord.isAltDown
            && event.isShiftDown == chord.isShiftDown;
    }

    HostReloadRequest BuildDemoReload(bool hasVoid = false)
    {
        constexpr int side = 32;
        HostReloadRequest reload;
        reload.metadata.identity.datasetId = hasVoid ? "standalone-integrated-demo" : "standalone-part-synthetic";
        reload.metadata.source.kind = ImageSourceKind::Memory;
        reload.metadata.source.uri = "memory://" + reload.metadata.identity.datasetId;
        reload.voxels.resize(
            static_cast<std::size_t>(side) * side * side, 0.0F);
        for (int z = 0; z < side; ++z) {
            for (int y = 0; y < side; ++y) {
                for (int x = 0; x < side; ++x) {
                    const bool isFirst = x >= 2 && x < 10
                        && y >= 2 && y < 10
                        && z >= 2 && z < 10;
                    // 让一个零件穿过三个默认中心切片，人工模式启动后可直接观察。
                    const bool isSecond = x >= 12 && x < 20
                        && y >= 12 && y < 20
                        && z >= 12 && z < 20;
                    if (!isFirst && !isSecond) continue;
                    const auto index = static_cast<std::size_t>(
                        x + side * (y + side * z));
                    reload.voxels[index] = hasVoid && isSecond
                        && x >= 15 && x < 18 && y >= 15 && y < 18 && z >= 15 && z < 18 ? 0.0F : 1.0F;
                }
            }
        }
        reload.geometry.dimensions = { side, side, side };
        reload.geometry.spacing = { 0.5F, 0.5F, 0.5F };
        reload.geometry.origin = { 0.0F, 0.0F, 0.0F };
        return reload;
    }

#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    PartSegmentationConfig GetPartConfig()
    {
        PartSegmentationConfig config;
        config.defaultStart.targetViews.viewIds = {
            "primary-3d",
            "slice-top-down",
            "slice-front-back",
            "slice-left-right"
        };
        config.defaultStart.threshold = 0.5;
        config.defaultStart.minPartVoxels = 8;
        constexpr std::size_t gibibyte = std::size_t{1} << 30U;
        config.maxWorkingBytes = 32U * gibibyte;
#if defined(_WIN32)
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory))
            config.maxWorkingBytes = static_cast<std::size_t>(std::min<ULONGLONG>(
                48U * gibibyte, memory.ullAvailPhys / 2U));
#endif
        std::cout << "[Part] working budget bytes=" << config.maxWorkingBytes << '\n';
        return config;
    }

    bool RenderPartViews(VtkAppHostSession& session)
    {
        const std::array<std::string, 4> viewIds{
            "primary-3d",
            "slice-top-down",
            "slice-front-back",
            "slice-left-right"
        };
        for (const auto& viewId : viewIds) {
            const auto* endpoint = session.GetRenderViewEndpoint(viewId);
            if (!endpoint || !endpoint->renderWindow) return false;
            endpoint->renderWindow->Render();
            endpoint->renderWindow->WaitForCompletion();
        }
        return true;
    }

    bool StartPartQtSim(
        VtkAppHostSession& session,
        const std::shared_ptr<PartSegmentationHostFeature>& feature,
        const std::optional<std::size_t> expectedPartCount,
        bool& isComplete,
        bool& isPassed)
    {
        if (!feature) return false;
        PartSegmentationRequest request;
        request.action = PartSegmentationAction::Start;
        const std::weak_ptr<PartSegmentationHostFeature> weakFeature = feature;
        const auto admission = feature->SendRequest(
            std::move(request),
            [&session, &isComplete, &isPassed,
            weakFeature, expectedPartCount](
                PartSegmentationResult result) {
                    const auto owner = weakFeature.lock();
                    const auto state = owner
                        ? owner->GetState() : PartSegmentationState{};
                    const auto snapshot = owner
                        ? owner->GetPartSetSnapshot() : nullptr;
                    const auto* firstPart = snapshot
                        && !snapshot->parts.empty()
                        ? &snapshot->parts.front() : nullptr;
                    isPassed = result.status == PartResultStatus::Succeeded
                        && result.failureReason == PartFailureReason::None
                        && (expectedPartCount
                            ? result.partCount == *expectedPartCount
                            : result.partCount > 0)
                        && state.status == PartSegmentationStatus::Succeeded
                        && state.partCount == result.partCount
                        && state.resultRevision == result.resultRevision
                        && snapshot
                        && snapshot->partSetId == state.partSetId
                        && snapshot->resultRevision == state.resultRevision
                        && snapshot->catalogRevision == state.catalogRevision
                        && snapshot->parts.size() == state.partCount
                        && firstPart
                        && state.resultSet == result.resultSet
                        && state.labelMap == result.labelMap
                        && state.partTable == result.partTable
                        && GetDataRevisionRefValid(state.labelMap)
                        && GetDataRevisionRefValid(state.partTable);
                    if (!expectedPartCount && isPassed) {
                        const auto renderStart = std::chrono::steady_clock::now();
                        isPassed = RenderPartViews(session);
                        const auto renderElapsed =
                            std::chrono::steady_clock::now() - renderStart;
                        std::cout
                            << "QT_PART_RENDER: passed=" << isPassed
                            << " elapsed_ms="
                            << std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                                renderElapsed).count()
                            << '\n' << std::flush;
                    }
                    isComplete = true;
                    std::cout
                        << "QT_PART_RESULT: request=" << result.requestId
                        << " status=" << static_cast<int>(result.status)
                        << " failure=" << static_cast<int>(result.failureReason)
                        << " source_generation="
                        << result.sourceRevision.generation
                        << " label_generation="
                        << result.labelMap.generation
                        << " table_generation="
                        << result.partTable.generation
                        << " result_generation="
                        << result.resultSet.generation
                        << " commit=" << result.commitId
                        << "catalog_revision=" << state.catalogRevision
                        << " parts=" << result.partCount
                        << " part_set_high=" << state.partSetId.high
                        << " part_set_low=" << state.partSetId.low
                        << " first_object_high=" << (firstPart
                            ? firstPart->binding.object.objectId.high : 0)
                        << " first_object_low=" << (firstPart
                            ? firstPart->binding.object.objectId.low : 0)
                        << " first_label=" << (firstPart
                            ? firstPart->labelId : 0)
                        << " message=" << result.message
                        << " passed=" << isPassed
                        << '\n' << std::flush;
                    (void)StopEventLoop(session);
            });
        const bool isAccepted =
            admission.status == PartAdmissionStatus::Accepted;
        std::cout
            << "QT_PART_ADMISSION: status="
            << static_cast<int>(admission.status)
            << " request=" << admission.requestId
            << " accepted=" << isAccepted
            << '\n' << std::flush;
        if (isAccepted) return true;

        isComplete = true;
        isPassed = false;
        (void)StopEventLoop(session);
        return false;
    }
#endif

    std::string GetRevisionText(const DataRevisionRef& revision)
    {
        if (!GetDataRevisionRefValid(revision)) return "none";
        std::ostringstream text;
        text << std::hex << std::setfill('0');
        for (const auto value : revision.entityId.bytes) text << std::setw(2) << static_cast<unsigned int>(value);
        text << ':' << std::dec << revision.generation;
        return text.str();
    }

    void PrintDemoHelp()
    {
        std::cout << "\n=== Integrated feature demo (focus any viewer) ===\n"
            << "F1 help | F2 image descriptor + DataGraph | F3 label maps + sample values | F4 scene/frame state | F5 fit views\n"
            << "Crop: O box, P plane, 1 keep inside, 2 remove inside; drag widget before building\n"
            << "      Ctrl+7 build result, Ctrl+8 display result, Ctrl+9 restore source, 4/5 undo/redo\n"
            << "Gap: G analyze, J hide/show\n"
            << "Rendering: L/Shift+L volume quality, I/Shift+I iso quality, C/Shift+C color, V/Shift+V opacity\n";
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        std::cout << "Parts: B analyze, Shift+B hide/show all, Ctrl+B clear, Alt+B cancel\n"
            << "       N/Shift+N select next/previous, Ctrl+H hide/show selected, Ctrl+R mark reviewed\n";
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        std::cout << "Surface: U estimate ISO50 and apply isovalue, Ctrl+U clear estimate, Alt+U cancel\n";
#endif
        std::cout << "M view mode | S export data | T export slices | Escape leave active tool / exit\n"
            << "--demo: small two-object volume for interactive exploration\n"
            << "--demo-audit: exercise the same shortcut routes and exit\n"
            << "--real-audit: run U then B on the main real-data input, measure owner responsiveness\n"
            << "Results/progress appear in window titles; F2/F3/F4 print details here.\n" << std::flush;
    }

    class MainControlFeature final
        : public HostFeature,
        public std::enable_shared_from_this<MainControlFeature> {
    public:
        MainControlFeature(
            VtkAppHostSession& session,
            HostViewTarget volumeTarget,
            HostViewTarget isoTarget,
            HostViewTargets inputViews,
            std::weak_ptr<CropHostFeature> cropFeature,
            std::weak_ptr<GapHostFeature> gapFeature,
            GapHostStartParams gapStart)
            : m_session(session),
            m_volumeTarget(std::move(volumeTarget)),
            m_isoTarget(std::move(isoTarget)),
            m_inputViews(std::move(inputViews)),
            m_cropFeature(std::move(cropFeature)),
            m_gapFeature(std::move(gapFeature)),
            m_gapStart(std::move(gapStart)),
            m_keys{
                HostKeyChord{ 'c' },
                HostKeyChord{ 'c', {}, false, false, true },
                HostKeyChord{ 'v' },
                HostKeyChord{ 'v', {}, false, false, true },
                HostKeyChord{ 'l' },
                HostKeyChord{ 'l', {}, false, false, true },
                HostKeyChord{ 'i' },
                HostKeyChord{ 'i', {}, false, false, true },
                HostKeyChord{ 'g' },
                HostKeyChord{ '7', {}, true },
                HostKeyChord{ '8', {}, true },
                HostKeyChord{ '9', {}, true },
                HostKeyChord{ 0, "F1" },
                HostKeyChord{ 0, "F2" },
                HostKeyChord{ 0, "F3" },
                HostKeyChord{ 0, "F4" },
                HostKeyChord{ 0, "F5" },
                HostKeyChord{ 'u' },
                HostKeyChord{ 'u', {}, true },
                HostKeyChord{ 'u', {}, false, true }
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
            m_data = context.data;
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
            m_data.reset();
            m_isAttached = false;
            return true;
        }

        bool OnHostTick() override
        {
            if (m_isDemoFitPending && m_host) {
                const auto* endpoint = m_session.GetRenderViewEndpoint(m_isoTarget.viewId);
                double bounds[6]{};
                if (endpoint && endpoint->renderer) {
                    endpoint->renderer->ComputeVisiblePropBounds(bounds);
                    // 异步等值面提交前没有有效 bounds，此时 ResetCamera 不会适配新数据。
                    if (bounds[0] <= bounds[1] && bounds[2] <= bounds[3] && bounds[4] <= bounds[5]) {
                        const auto weak = weak_from_this();
                        if (m_host->SendOwnerComplete([weak] {
                            if (const auto owner = weak.lock()) (void)owner->SendControl(ControlAction::FitViews);
                        })) m_isDemoFitPending = false;
                    }
                }
            }
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
            SendSurfaceProgress();
#endif
            return SendQualityAudit();
        }

        void StartDemoFit() { m_isDemoFitPending = true; }

#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        void SetSurfaceFeature(std::weak_ptr<SurfaceDeterminationHostFeature> feature)
        { m_surfaceFeature = std::move(feature); }
#endif

        bool StartQualityAudit()
        {
            if (m_qualityAuditPhase != QualityAuditPhase::None) return false;
            m_qualityAuditPhase = QualityAuditPhase::SetLow;
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
            IsoQualityNext,
            IsoQualityPrevious,
            StartGap,
            BuildCropResult,
            SetCropPrimary,
            RestoreCropSource,
            Help, Data, Labels, Scenes, FitViews,
            SurfaceStart, SurfaceClear, SurfaceStop,
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

        bool SetAuditQuality(const HostVolumeQuality quality)
        {
            HostViewSetRequest request;
            request.targetView = m_volumeTarget;
            request.volumeQuality = quality;
            return SendViewRequest(std::move(request));
        }

        bool StartAuditRender()
        {
            const auto* endpoint = m_session.GetRenderViewEndpoint(
                m_volumeTarget.viewId);
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
            const auto state = m_session.GetRenderViewState(
                m_volumeTarget);
            if (!state) {
                return StopQualityAudit(false, HostVolumeQuality::Auto);
            }

            constexpr auto phaseWaitLimit = std::chrono::minutes(2);
            const auto setWaitPhase = [&](const QualityAuditPhase phase) {
                m_qualityAuditPhase = phase;
                m_qualityAuditDeadline =
                    std::chrono::steady_clock::now() + phaseWaitLimit;
            };
            const auto hasTimedOut = [&]() {
                return std::chrono::steady_clock::now()
                    >= m_qualityAuditDeadline;
            };
            switch (m_qualityAuditPhase) {
            case QualityAuditPhase::SetLow:
                if (!SetAuditQuality(HostVolumeQuality::Low)) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                setWaitPhase(QualityAuditPhase::WaitLow);
                return true;
            case QualityAuditPhase::WaitLow:
                if (state->volumeQuality == HostVolumeQuality::Low) {
                    if (!StartAuditRender()) {
                        return StopQualityAudit(false, state->volumeQuality);
                    }
                    m_qualityAuditPhase = QualityAuditPhase::SetHigh;
                    return true;
                }
                if (hasTimedOut()) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                return true;
            case QualityAuditPhase::SetHigh:
                if (!SetAuditQuality(HostVolumeQuality::High)) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                setWaitPhase(QualityAuditPhase::WaitHigh);
                return true;
            case QualityAuditPhase::WaitHigh:
                if (state->volumeQuality == HostVolumeQuality::High) {
                    if (!StartAuditRender()) {
                        return StopQualityAudit(false, state->volumeQuality);
                    }
                    m_isHighApplied = true;
                    m_qualityAuditPhase = QualityAuditPhase::SetXHigh;
                    return true;
                }
                if (hasTimedOut()) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                return true;
            case QualityAuditPhase::SetXHigh:
                if (!SetAuditQuality(HostVolumeQuality::XHigh)) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                setWaitPhase(QualityAuditPhase::WaitXHigh);
                return true;
            case QualityAuditPhase::WaitXHigh:
                if (state->volumeQuality == HostVolumeQuality::XHigh) {
                    if (!StartAuditRender()) {
                        return StopQualityAudit(false, state->volumeQuality);
                    }
                    m_isXHighApplied = true;
                    m_qualityAuditPhase = QualityAuditPhase::SetUltra;
                    return true;
                }
                if (hasTimedOut()) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                return true;
            case QualityAuditPhase::SetUltra:
                if (!SetAuditQuality(HostVolumeQuality::Ultra)) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                setWaitPhase(QualityAuditPhase::WaitUltra);
                return true;
            case QualityAuditPhase::WaitUltra:
                if (state->volumeQuality == HostVolumeQuality::Ultra) {
                    if (!StartAuditRender()) {
                        return StopQualityAudit(false, state->volumeQuality);
                    }
                    m_isUltraApplied = true;
                    return StopQualityAudit(
                        m_isHighApplied
                        && m_isXHighApplied
                        && m_isUltraApplied,
                        state->volumeQuality);
                }
                if (hasTimedOut()) {
                    return StopQualityAudit(false, state->volumeQuality);
                }
                return true;
            default:
                return true;
            }
        }

        bool SetTransfer(const ControlAction action)
        {
            const auto state =
                m_session.GetRenderViewState(m_volumeTarget);
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
            request.targetView = m_volumeTarget;
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

        bool SwitchQuality(
            const HostViewTarget& target,
            const int direction)
        {
            const auto state =
                m_session.GetRenderViewState(target);
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
            request.targetView = target;
            request.volumeQuality =
                qualities[static_cast<std::size_t>(nextIndex)];
            if (!SendViewRequest(std::move(request))) return false;

            std::cout << "[Quality] view=" << target.viewId << ' '
                << qualityNames[static_cast<std::size_t>(nextIndex)]
                << '\n';
            return true;
        }

        bool SetDemoStatus(const std::string& status)
        {
            if (!m_host || m_volumeTarget.viewId.empty()) {
                return false;
            }
            const std::vector<std::string> statusViews{
                m_volumeTarget.viewId
            };
            return m_host->SetViewStatus(statusViews, status);
        }

    public:
        bool StartGap()
        {
            const auto gapFeature = m_gapFeature.lock();
            if (!gapFeature) {
                (void)SetDemoStatus("Gap: unavailable");
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
                [gapOwner, controlOwner](GapHostResult result) {
                    const bool isSuccess = result.status
                            == GapResultStatus::Succeeded
                        || result.status
                            == GapResultStatus::SucceededWithDisplayFailure;
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
                        (void)owner->SetDemoStatus(status.str());
                    }
                    std::cerr << "[GapAnalysis] "
                        << status.str()
                        << " | commit=" << result.commitId
                        << " | source_generation="
                        << result.sourceRevision.generation
                        << " | label_generation="
                        << result.labelMap.generation
                        << " | void_table_generation="
                        << result.voidTable.generation
                        << " | mesh_generation="
                        << result.voidMesh.generation
                        << " | statistics_generation="
                        << result.statisticsData.generation
                        << " | result_generation="
                        << result.resultSet.generation
                        << '\n' << std::flush;
                });
            const std::string requestStatus = isAccepted
                ? "Gap: running"
                : "Gap: rejected";
            (void)SetDemoStatus(requestStatus);
            std::cerr << "[GapAnalysis] G request "
                << (isAccepted ? "accepted; calculation started"
                    : "rejected; calculation did not start")
                << '\n' << std::flush;
            return isAccepted;
        }

        bool SendCrop(const CropHostAction action)
        {
            const auto cropFeature = m_cropFeature.lock();
            if (!cropFeature) {
                std::cerr
                    << "[OrthogonalCrop] request rejected: feature unavailable\n"
                    << std::flush;
                return false;
            }

            CropHostRequest request;
            request.action = action;
            if (action == CropHostAction::BuildResult) {
                const bool isAccepted = cropFeature->SendRequest(
                    std::move(request),
                    [](CropBuildResult result) {
                        std::cerr
                            << "[OrthogonalCrop] BuildResult"
                            << " | succeeded=" << result.isSucceeded
                            << " | failure="
                            << static_cast<int>(result.failureReason)
                            << " | commit=" << result.commitId
                            << " | source_generation="
                            << result.sourceRevision.generation
                            << " | recipe_generation="
                            << result.recipeRevision.generation
                            << " | output_generation="
                            << result.outputRevision.generation
                            << " | message=" << result.message << '\n'
                            << std::flush;
                    });
                std::cerr
                    << "[OrthogonalCrop] BuildResult request "
                    << (isAccepted ? "accepted" : "rejected") << '\n'
                    << std::flush;
                return isAccepted;
            }

            const bool isAccepted = cropFeature->SendRequest(
                std::move(request));
            const auto state = cropFeature->GetState();
            std::cerr
                << "[OrthogonalCrop] action="
                << static_cast<int>(action)
                << " | accepted=" << isAccepted
                << " | commit=" << state.commitId
                << " | source_generation="
                << state.sourceRevision.generation
                << " | recipe_generation="
                << state.recipeRevision.generation
                << " | output_generation="
                << state.outputRevision.generation << '\n'
                << std::flush;
            return isAccepted;
        }

    private:
        bool PrintData()
        {
            const auto descriptor = m_session.GetImageDescriptor();
            if (!descriptor || !m_data) {
                (void)SetDemoStatus("Demo: waiting for image");
                return false;
            }
            const auto graph = m_data->GetDataGraph();
            if (!graph.view) return false;
            std::cout << "[Image] dataset=" << descriptor->metadata.identity.datasetId
                << " dims=" << descriptor->dims[0] << 'x' << descriptor->dims[1] << 'x' << descriptor->dims[2]
                << " spacing=" << descriptor->spacing[0] << ',' << descriptor->spacing[1] << ',' << descriptor->spacing[2]
                << " range=" << descriptor->scalarRange[0] << ',' << descriptor->scalarRange[1]
                << " quantity=" << descriptor->metadata.scalar.quantity << " unit=" << descriptor->metadata.scalar.unit
                << " source=" << descriptor->metadata.source.uri
                << " revision=" << GetRevisionText(descriptor->dataRevision) << '\n';
            const auto bindings = graph.view->GetDataBindings();
            std::cout << "[DataGraph] commit=" << graph.commitId << " bindings=" << bindings.size() << '\n';
            for (const auto& binding : bindings) {
                if (!binding.target) continue;
                const auto data = graph.view->GetData(*binding.target);
                std::cout << "  " << binding.name << " -> " << GetRevisionText(*binding.target)
                    << " binding_revision=" << binding.revision;
                if (data) {
                    std::cout << " type=" << data->type.name;
                    for (const auto& input : data->inputs)
                        std::cout << " | " << input.role << "=" << GetRevisionText(input.source);
                }
                std::cout << '\n';
            }
            std::ostringstream status;
            status << descriptor->metadata.identity.datasetId << " | " << descriptor->dims[0] << 'x'
                << descriptor->dims[1] << 'x' << descriptor->dims[2] << " | graph=" << graph.commitId;
            (void)SetDemoStatus(status.str());
            return true;
        }

        bool PrintLabels()
        {
            const auto labels = m_session.GetLabelMapDescriptors();
            std::cout << "[LabelMap] active=" << labels.size() << " (run B or G first)\n";
            bool isPassed = true;
            for (const auto& label : labels) {
                LabelMapReadRequest request;
                request.id = label.id;
                request.expectedRevision = label.dataRevision;
                request.maxBytes = 8 * label.componentBytes;
                const auto chunk = m_session.GetLabelMapReadChunk(request, 0);
                std::cout << "  " << label.id << " dataset=" << label.datasetId
                    << " source=" << GetRevisionText(label.sourceRevision)
                    << " revision=" << GetRevisionText(label.dataRevision) << " samples=";
                const auto printValues = [&](auto value) {
                    using Value = decltype(value);
                    if (!chunk.state || !chunk.state->values) return;
                    const auto& bytes = *chunk.state->values;
                    for (std::size_t offset = 0; offset + sizeof(Value) <= bytes.size(); offset += sizeof(Value)) {
                        Value sample{};
                        std::memcpy(&sample, bytes.data() + offset, sizeof(Value));
                        std::cout << +sample << ' ';
                    }
                };
                switch (label.valueType) {
                case ImageValueType::Int8: printValues(std::int8_t{}); break;
                case ImageValueType::UInt8: printValues(std::uint8_t{}); break;
                case ImageValueType::Int16: printValues(std::int16_t{}); break;
                case ImageValueType::UInt16: printValues(std::uint16_t{}); break;
                case ImageValueType::Int32: printValues(std::int32_t{}); break;
                case ImageValueType::UInt32: printValues(std::uint32_t{}); break;
                case ImageValueType::Int64: printValues(std::int64_t{}); break;
                case ImageValueType::UInt64: printValues(std::uint64_t{}); break;
                default: break;
                }
                isPassed = isPassed && chunk.error == LabelMapError::None;
                std::cout << "read_status=" << static_cast<int>(chunk.error) << '\n';
            }
            (void)SetDemoStatus("Label maps: " + std::to_string(labels.size()) + " | details in console (F3)");
            return isPassed;
        }

        bool PrintScenes()
        {
            const auto scenes = m_session.GetSceneViewStates();
            for (const auto& scene : scenes) {
                std::cout << "[Scene] " << scene.id << " committed=" << scene.sceneEpoch
                    << " rendered=" << scene.renderedEpoch << " features=";
                for (const auto& id : scene.activeFeatureIds) std::cout << id << ' ';
                if (scene.presentation) std::cout << " quality=" << static_cast<int>(scene.presentation->volumeQuality)
                    << " interacting=" << scene.presentation->isInteracting
                    << " input=" << GetRevisionText(scene.presentation->dataRevision);
                std::cout << '\n';
            }
            (void)SetDemoStatus("Scene/frame snapshots: " + std::to_string(scenes.size()) + " views | details in console (F4)");
            return !scenes.empty();
        }

#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        static std::string_view GetMethodName(SurfaceDeterminationMethod method)
        {
            switch (method) {
            case SurfaceDeterminationMethod::GlobalIsoPreview: return "GlobalIsoPreview";
            case SurfaceDeterminationMethod::LocalAdaptiveIso50: return "LocalAdaptiveIso50";
            case SurfaceDeterminationMethod::GradientPeak: return "GradientPeak";
            case SurfaceDeterminationMethod::AutomaticIso50: return "AutomaticIso50";
            }
            return "Unknown";
        }

        bool SendSurface(ControlAction action)
        {
            const auto feature = m_surfaceFeature.lock();
            if (!feature) return false;
            SurfaceDeterminationRequest request;
            if (action == ControlAction::SurfaceStart) {
                const auto image = m_session.GetImageDescriptor();
                if (!image) return false;
                SurfaceDeterminationStartParams start;
                start.targetViews.viewIds = { m_isoTarget.viewId };
                start.method = m_surfaceMethod;
                start.componentSelection = SurfaceComponentSelection::All;
                start.initialIsoValue.reset();
                request.action = SurfaceDeterminationAction::Start;
                request.start = start;
            }
            else if (action == ControlAction::SurfaceClear) request.action = SurfaceDeterminationAction::Clear;
            else request.action = SurfaceDeterminationAction::Stop;
            const auto weakOwner = weak_from_this();
            const auto admission = feature->SendRequest(std::move(request), [weakOwner, action](SurfaceDeterminationResult result) {
                if (const auto owner = weakOwner.lock()) {
                    std::ostringstream text;
                    const auto feature = owner->m_surfaceFeature.lock();
                    const auto outcome = result.status != SurfaceResultStatus::Succeeded
                        ? (result.status == SurfaceResultStatus::Cancelled ? "cancelled" : "failed")
                        : action == ControlAction::SurfaceClear ? "cleared"
                        : "ready";
                    text << "Surface: " << outcome;
                    if (action == ControlAction::SurfaceStart && result.status == SurfaceResultStatus::Succeeded
                        && result.isoEstimate) {
                        const auto image = owner->m_session.GetImageDescriptor();
                        HostViewSetRequest request;
                        request.targetView = owner->m_isoTarget;
                        request.iso = result.isoEstimate->isoValue;
                        const bool isApplied = image && image->dataRevision == result.sourceRevision
                            && owner->m_session.SendRequest(std::move(request));
                        text << " | isovalue=" << result.isoEstimate->isoValue
                            << (isApplied ? " applied" : " apply rejected")
                            << " | samples=" << result.isoEstimate->sampleCount;
                    }
                    (void)owner->SetDemoStatus(text.str());
                    std::cout << "[Surface] " << text.str() << " | reason=" << static_cast<int>(result.failureReason)
                        << " | " << result.message << '\n' << std::flush;
                }
            });
            if (admission.status != SurfaceAdmissionStatus::Accepted) {
                (void)SetDemoStatus("Surface request rejected: " + std::to_string(static_cast<int>(admission.status)));
                return false;
            }
            if (action == ControlAction::SurfaceStart) m_surfaceRunningMethod = m_surfaceMethod;
            return true;
        }

        void SendSurfaceProgress()
        {
            const auto feature = m_surfaceFeature.lock();
            if (!feature) return;
            const auto state = feature->GetState();
            const int progress = static_cast<int>(state.progress01 * 100.0);
            if (state.requestId == 0 || (m_surfaceStage == state.stage && m_surfaceProgress == progress)) return;
            m_surfaceStage = state.stage;
            m_surfaceProgress = progress;
            std::ostringstream status;
            status << "Surface: " << GetMethodName(m_surfaceRunningMethod) << " | " << progress << "%";
            const auto generation = feature->GetSurfaceSnapshot();
            if (generation && generation->isoEstimate)
                status << " | isovalue=" << generation->isoEstimate->isoValue;
            else if (m_surfaceRunningMethod != SurfaceDeterminationMethod::AutomaticIso50)
                status << " | points=" << state.pointCount << " | objects=" << state.objectCount;
            if (!state.errorMessage.empty()) status << " | " << state.errorMessage;
            (void)SetDemoStatus(status.str());
        }
#endif

        bool SendControl(const ControlAction action)
        {
            switch (action) {
            case ControlAction::ColorUp:
            case ControlAction::ColorDown:
            case ControlAction::OpacityUp:
            case ControlAction::OpacityDown:
                return SetTransfer(action);
            case ControlAction::QualityNext:
                return SwitchQuality(m_volumeTarget, 1);
            case ControlAction::QualityPrevious:
                return SwitchQuality(m_volumeTarget, -1);
            case ControlAction::IsoQualityNext:
                return SwitchQuality(m_isoTarget, 1);
            case ControlAction::IsoQualityPrevious:
                return SwitchQuality(m_isoTarget, -1);
            case ControlAction::Help:
                PrintDemoHelp();
                (void)SetDemoStatus("F1 help | F2 data | F3 labels | F4 frames | F5 fit | U surface | B parts | G gap");
                return true;
            case ControlAction::Data: return PrintData();
            case ControlAction::Labels: return PrintLabels();
            case ControlAction::Scenes: return PrintScenes();
            case ControlAction::FitViews: {
                bool isSucceeded = true;
                for (const auto& view : m_session.GetRenderViewStates()) {
                    HostViewResetRequest request;
                    request.targetView.viewId = view.id;
                    isSucceeded = m_session.SendRequest(std::move(request)) && isSucceeded;
                }
                return isSucceeded;
            }
            case ControlAction::SurfaceStart:
            case ControlAction::SurfaceClear:
            case ControlAction::SurfaceStop:
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
                return SendSurface(action);
#else
                (void)SetDemoStatus("SurfaceDetermination is not enabled in this build");
                return false;
#endif
            case ControlAction::StartGap:
                return StartGap();
            case ControlAction::BuildCropResult:
                return SendCrop(CropHostAction::BuildResult);
            case ControlAction::SetCropPrimary:
                return SendCrop(CropHostAction::SetPrimaryResult);
            case ControlAction::RestoreCropSource:
                return SendCrop(CropHostAction::RestoreOriginal);
            default:
                return false;
            }
        }

        InteractionResult OnInput(
            const InteractionEvent& event)
        {
            if (event.eventKind == InteractionEventKind::KeyRelease) {
                bool wasDown = false;
                for (std::size_t index = 0; index < m_keys.size(); ++index) {
                    if (GetKeyMatched(event, m_keys[index].keyCode)
                        || (!m_keys[index].keySym.empty() && event.keySym == m_keys[index].keySym)) {
                        wasDown = wasDown || m_isKeyDown[index];
                        m_isKeyDown[index] = false;
                    }
                }
                return wasDown ? InteractionResult{true, true} : InteractionResult{};
            }
            const auto action = GetAction(event);
            if (!action) return {};
            const auto index = static_cast<std::size_t>(*action);

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
        HostViewTarget m_volumeTarget;
        HostViewTarget m_isoTarget;
        bool m_isDemoFitPending = false;
        HostViewTargets m_inputViews;
        std::weak_ptr<CropHostFeature> m_cropFeature;
        std::weak_ptr<GapHostFeature> m_gapFeature;
        GapHostStartParams m_gapStart;
        std::array<HostKeyChord, actionCount> m_keys;
        std::array<bool, actionCount> m_isKeyDown{};
        std::shared_ptr<FeatureHostControl> m_host;
        std::shared_ptr<TrustedDataReadPort> m_data;
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        std::weak_ptr<SurfaceDeterminationHostFeature> m_surfaceFeature;
        SurfaceDeterminationMethod m_surfaceMethod = SurfaceDeterminationMethod::AutomaticIso50;
        SurfaceDeterminationMethod m_surfaceRunningMethod = SurfaceDeterminationMethod::AutomaticIso50;
        SurfaceDeterminationStage m_surfaceStage = SurfaceDeterminationStage::Idle;
        int m_surfaceProgress = -1;
#endif
        bool m_isAttached = false;
        QualityAuditPhase m_qualityAuditPhase = QualityAuditPhase::None;
        std::chrono::steady_clock::time_point m_qualityAuditDeadline{};
        bool m_isQualityAuditPassed = false;
        bool m_isHighApplied = false;
        bool m_isXHighApplied = false;
        bool m_isUltraApplied = false;
    };

#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    class PartControlFeature final
        : public HostFeature,
        public std::enable_shared_from_this<PartControlFeature> {
    public:
        PartControlFeature(
            VtkAppHostSession& session,
            HostViewTarget statusTarget,
            HostViewTargets inputViews,
            std::weak_ptr<PartSegmentationHostFeature> partFeature,
            PartSegmentationStartParams partStart)
            : m_session(session), m_statusTarget(std::move(statusTarget)),
            m_inputViews(std::move(inputViews)),
            m_partFeature(std::move(partFeature)),
            m_partStart(std::move(partStart)),
            m_keys{
                HostKeyChord{ 'b' },
                HostKeyChord{ 'b', {}, false, false, true },
                HostKeyChord{ 'b', {}, true },
                HostKeyChord{ 'b', {}, false, true },
                HostKeyChord{ 'n' },
                HostKeyChord{ 'n', {}, false, false, true },
                HostKeyChord{ 'h', {}, true },
                HostKeyChord{ 'r', {}, true }
            }
        {
        }

        bool OnHostTick() override
        {
            const auto feature = m_partFeature.lock();
            if (!feature) return true;
            const auto state = feature->GetState();
            const int progress = static_cast<int>(state.progress * 100.0);
            if (state.status == PartSegmentationStatus::Running && progress != m_progress) {
                m_progress = progress;
                (void)SetPartStatus("Part: " + std::to_string(progress) + "% | Alt+B cancels");
            }
            return true;
        }

        std::string_view GetFeatureId() const noexcept override
        {
            return featureId;
        }

        bool AttachHost(const HostFeatureContext& context) override
        {
            if (m_isAttached || !context.host || m_partFeature.expired()) {
                return false;
            }
            const auto weakOwner = weak_from_this();
            if (weakOwner.expired()) return false;

            m_host = context.host;
            HostInputBinding binding;
            binding.featureId = std::string(featureId);
            binding.targetViews = m_inputViews;
            binding.onInput = [weakOwner](const InteractionEvent& event) {
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
            if (m_host && !m_host->DetachInput(featureId)) return false;
            m_isKeyDown.fill(false);
            m_host.reset();
            m_isAttached = false;
            return true;
        }

    private:
        enum class ControlAction : std::uint8_t {
            Start,
            Toggle,
            Clear, Stop, Next, Previous, TogglePart, Review,
            Count
        };

        static constexpr std::string_view featureId =
            "main.part-controls";
        static constexpr std::size_t actionCount =
            static_cast<std::size_t>(ControlAction::Count);

        std::optional<ControlAction> GetAction(
            const InteractionEvent& event) const
        {
            for (std::size_t index = 0; index < m_keys.size(); ++index) {
                if (GetChordMatched(event, m_keys[index])) {
                    return static_cast<ControlAction>(index);
                }
            }
            return std::nullopt;
        }

        bool SetPartStatus(const std::string& status)
        {
            if (!m_host || m_statusTarget.viewId.empty()) return false;
            return m_host->SetViewStatus(
                { m_statusTarget.viewId }, status);
        }

        static std::string_view GetPendingStatus(const ControlAction action)
        {
            if (action == ControlAction::Start) return "Part: running";
            if (action == ControlAction::Toggle) {
                return "Part: visibility updating";
            }
            return action == ControlAction::Stop ? "Part: cancelling" : "Part: clearing";
        }

        bool SendPartRequest(
            PartSegmentationRequest request,
            const ControlAction action,
            const std::optional<bool> nextVisibility = std::nullopt)
        {
            const auto partFeature = m_partFeature.lock();
            if (!partFeature) {
                (void)SetPartStatus("Part: unavailable");
                std::cerr
                    << "[PartSegmentation] request rejected: feature unavailable\n"
                    << std::flush;
                return false;
            }

            (void)SetPartStatus(std::string(GetPendingStatus(action)));
            const auto partOwner = m_partFeature;
            const auto controlOwner = weak_from_this();
            const auto admission = partFeature->SendRequest(
                std::move(request),
                [partOwner, controlOwner, action, nextVisibility](
                    PartSegmentationResult result) {
                        const auto completedFeature = partOwner.lock();
                        std::ostringstream status;
                        status << "Part: ";
                        if (result.status == PartResultStatus::Succeeded) {
                            if (action == ControlAction::Start) {
                                const auto state = completedFeature
                                    ? completedFeature->GetState()
                                    : PartSegmentationState{};
                                status << "succeeded | parts=" << result.partCount
                                    << " | result_generation="
                                    << result.resultSet.generation
                                    << " | commit=" << result.commitId
                                    << " | visible=" << state.isOverlayVisible;
                            }
                            else if (action == ControlAction::Toggle) {
                                status << (nextVisibility.value_or(false)
                                    ? "visible" : "hidden");
                            }
                            else {
                                status << (action == ControlAction::Stop ? "cancellation requested" : "cleared");
                            }
                        }
                        else if (result.status == PartResultStatus::SucceededWithDisplayFailure) {
                            status << "data ready; display failed | parts=" << result.partCount;
                        }
                        else {
                            status
                                << (result.status == PartResultStatus::Cancelled
                                    ? "cancelled" : "failed")
                                << " | reason="
                                << static_cast<int>(result.failureReason);
                        }
                        if (const auto owner = controlOwner.lock()) {
                            (void)owner->SetPartStatus(status.str());
                        }
                        std::cerr
                            << "[PartSegmentation] " << status.str()
                            << " | source_generation="
                            << result.sourceRevision.generation
                            << " | label_generation="
                            << result.labelMap.generation
                            << " | table_generation="
                            << result.partTable.generation
                            << " | result_generation="
                            << result.resultSet.generation
                            << " | commit=" << result.commitId
                            << " | parts=" << result.partCount
                            << " | message=" << result.message << '\n'
                            << std::flush;
                });
            const bool isAccepted =
                admission.status == PartAdmissionStatus::Accepted;
            if (!isAccepted) {
                std::ostringstream status;
                status << "Part: rejected | admission="
                    << static_cast<int>(admission.status);
                (void)SetPartStatus(status.str());
            }
            std::cerr
                << "[PartSegmentation] B request "
                << (isAccepted ? "accepted" : "rejected")
                << " | action=" << static_cast<int>(action)
                << " | admission=" << static_cast<int>(admission.status)
                << '\n' << std::flush;
            return isAccepted;
        }

        bool SetPartSelection(ControlAction action)
        {
            const auto feature = m_partFeature.lock();
            const auto snapshot = feature ? feature->GetPartSetSnapshot() : nullptr;
            if (!snapshot || snapshot->isStale || snapshot->parts.empty()) {
                (void)SetPartStatus("Part: press B to create a current result first");
                return false;
            }
            const auto selected = std::find_if(snapshot->parts.begin(), snapshot->parts.end(),
                [](const PartSnapshot& part) { return part.presentation.isSelected; });
            std::size_t index = selected == snapshot->parts.end() ? 0 :
                static_cast<std::size_t>(selected - snapshot->parts.begin());
            if (action == ControlAction::Next && selected != snapshot->parts.end())
                index = (index + 1) % snapshot->parts.size();
            const auto& part = snapshot->parts[index];
            PartStatePatch patch;
            if (action == ControlAction::TogglePart) patch.isVisible = !part.presentation.isVisible;
            else if (action == ControlAction::Review) patch.isReviewed = !part.userState.isReviewed;
            else patch.isSelected = true;
            const auto result = action == ControlAction::Previous
                ? feature->SetPreviousPart(snapshot->catalogRevision)
                : feature->SetPartState(part.binding, patch, snapshot->catalogRevision);
            if (result.status != PartMutationStatus::Succeeded) {
                (void)SetPartStatus("Part edit rejected: " + std::to_string(static_cast<int>(result.status)));
                return false;
            }
            const auto current = feature->GetPartSetSnapshot();
            if (!current) return false;
            const auto updated = std::find_if(current->parts.begin(), current->parts.end(),
                [&part, action](const PartSnapshot& value) {
                    return action == ControlAction::Previous
                        ? value.presentation.isSelected : value.binding == part.binding;
                });
            if (updated == current->parts.end()) return false;
            std::ostringstream status;
            status << "Part " << updated->labelId << " | selected=" << updated->presentation.isSelected
                << " visible=" << updated->presentation.isVisible << " reviewed=" << updated->userState.isReviewed
                << " volume=" << updated->metrics.physicalVolumeMM3;
            (void)SetPartStatus(status.str());
            std::cout << "[Part] " << status.str() << " | stable_id="
                << updated->binding.object.objectId.high << ':' << updated->binding.object.objectId.low
                << " | catalog=" << current->catalogRevision << '\n';
            return true;
        }

        bool SendControl(const ControlAction action)
        {
            if (action == ControlAction::Next || action == ControlAction::Previous
                || action == ControlAction::TogglePart || action == ControlAction::Review) return SetPartSelection(action);
            PartSegmentationRequest request;
            if (action == ControlAction::Stop) {
                request.action = PartSegmentationAction::Stop;
                return SendPartRequest(std::move(request), action);
            }
            if (action == ControlAction::Start) {
                request.action = PartSegmentationAction::Start;
                auto start = m_partStart;
                const auto view = m_session.GetRenderViewState({"primary-3d"});
                if (!view) return false;
                start.threshold = view->isoThreshold;
#if defined(_WIN32)
                const auto image = m_session.GetImageDescriptor();
                MEMORYSTATUSEX memory{};
                memory.dwLength = sizeof(memory);
                if (image && GlobalMemoryStatusEx(&memory)) {
                    std::uint64_t voxelCount = 1;
                    for (const auto dimension : image->dims) voxelCount *= static_cast<std::uint64_t>(dimension);
                    const auto newLabelBytes = voxelCount * (2U * sizeof(std::uint32_t));
                    const auto reserveBytes = std::min<ULONGLONG>(std::uint64_t{8} << 30U,
                        memory.ullTotalPhys / 8U);
                    if (memory.ullAvailPhys < newLabelBytes + reserveBytes) {
                        (void)SetPartStatus("Part: insufficient available RAM for another full-resolution result");
                        std::cerr << "[Part] memory admission rejected: newLabelBytes=" << newLabelBytes
                            << " availableBytes=" << memory.ullAvailPhys << '\n';
                        return false;
                    }
                }
#endif
                m_progress = -1;
                request.start = start;
                std::cout << "[Part] using current isovalue=" << start.threshold << '\n' << std::flush;
                return SendPartRequest(std::move(request), action);
            }
            if (action == ControlAction::Toggle) {
                const auto partFeature = m_partFeature.lock();
                if (!partFeature) return false;
                const bool isVisible =
                    !partFeature->GetState().isOverlayVisible;
                request.action = PartSegmentationAction::SetVisibility;
                request.isVisible = isVisible;
                return SendPartRequest(
                    std::move(request), action, isVisible);
            }
            if (action == ControlAction::Clear) {
                request.action = PartSegmentationAction::Clear;
                return SendPartRequest(std::move(request), action);
            }
            return false;
        }

        InteractionResult OnInput(const InteractionEvent& event)
        {
            if (event.eventKind == InteractionEventKind::KeyRelease) {
                bool wasDown = false;
                for (std::size_t index = 0; index < m_keys.size(); ++index) {
                    if (GetKeyMatched(event, m_keys[index].keyCode)) {
                        wasDown = wasDown || m_isKeyDown[index];
                        m_isKeyDown[index] = false;
                    }
                }
                return wasDown ? InteractionResult{true, true} : InteractionResult{};
            }

            const auto action = GetAction(event);
            if (!action) return {};
            const auto index = static_cast<std::size_t>(*action);

            if (event.eventKind == InteractionEventKind::TextInput) {
                return m_isKeyDown[index]
                    ? InteractionResult{ true, true }
                : InteractionResult{};
            }
            if (event.eventKind != InteractionEventKind::KeyPress) return {};
            if (std::any_of(
                m_isKeyDown.begin(), m_isKeyDown.end(),
                [](const bool isDown) { return isDown; })) {
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

        HostViewTarget m_statusTarget;
        HostViewTargets m_inputViews;
        std::weak_ptr<PartSegmentationHostFeature> m_partFeature;
        VtkAppHostSession& m_session;
        int m_progress = -1;
        PartSegmentationStartParams m_partStart;
        std::array<HostKeyChord, actionCount> m_keys;
        std::array<bool, actionCount> m_isKeyDown{};
        std::shared_ptr<FeatureHostControl> m_host;
        bool m_isAttached = false;
    };
#endif

    // 通过与手动演示相同的 Host 输入路径验证快捷键，不直接调用业务动作。
    class DemoAuditFeature final : public HostFeature,
        public std::enable_shared_from_this<DemoAuditFeature> {
    public:
        explicit DemoAuditFeature(VtkAppHostSession& session) : m_session(session) {}
        std::string_view GetFeatureId() const noexcept override { return "main.demo-audit"; }
        bool AttachHost(const HostFeatureContext& context) override { m_host = context.host; return m_host != nullptr; }
        bool DetachHost() override { m_isActive = false; m_host.reset(); return true; }
        void Start() { m_isActive = true; m_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300); }
        bool GetPassed() const { return m_isDone && m_isPassed; }
        bool OnHostTick() override
        {
            if (!m_isActive) return true;
            const auto now = std::chrono::steady_clock::now();
            if (m_lastTick != std::chrono::steady_clock::time_point{})
                m_tickGapsMs.push_back(std::chrono::duration<double, std::milli>(now - m_lastTick).count());
            m_lastTick = now;
            if (m_isQueued) return true;
            const auto weak = weak_from_this();
            m_isQueued = true;
            return m_host && m_host->SendOwnerComplete([weak] {
                if (const auto owner = weak.lock()) {
                    owner->m_isQueued = false;
                    if (owner->m_isActive) owner->SendStep();
                }
            });
        }
        void SetFailureCheck(std::function<std::string()> getFailure) { m_getFailure = std::move(getFailure); }
        void AddStep(std::string name, HostKeyChord key, std::function<bool()> ready)
        { m_steps.push_back({std::move(name), std::move(key), std::move(ready)}); }
    private:
        struct Step { std::string name; HostKeyChord key; std::function<bool()> ready; };
        bool SendKey(const HostKeyChord& key)
        {
            auto* input = m_session.GetInputEndpoint();
            if (!input) return false;
            HostInputEvent event;
            event.viewId = "primary-3d";
            event.keyCode = key.keyCode;
            event.keySym = key.keySym.empty() ? std::string(1, key.keyCode) : key.keySym;
            event.isCtrlDown = key.isCtrlDown;
            event.isAltDown = key.isAltDown;
            event.isShiftDown = key.isShiftDown;
            event.kind = HostInputKind::KeyPress;
            const auto first = input->SendInput(event);
            const auto repeated = input->SendInput(event);
            event.kind = HostInputKind::KeyRelease;
            // 覆盖先松修饰键、后松字符键的真实键序列。
            event.isCtrlDown = false;
            event.isAltDown = false;
            event.isShiftDown = false;
            const auto released = input->SendInput(event);
            return first.isHandled && first.isSucceeded && repeated.isHandled && released.isHandled;
        }
        void Finish(bool passed)
        {
            m_isPassed = passed;
            m_isDone = true;
            m_isActive = false;
            if (!m_tickGapsMs.empty()) {
                std::sort(m_tickGapsMs.begin(), m_tickGapsMs.end());
                std::cout << "[DemoAudit] owner_tick_p95_ms=" << m_tickGapsMs[m_tickGapsMs.size() * 95 / 100]
                    << " owner_tick_max_ms=" << m_tickGapsMs.back() << " samples=" << m_tickGapsMs.size() << '\n';
            }
            std::cout << "AUDIT_DEMO: passed=" << passed << " completed=" << m_index
                << '/' << m_steps.size() << '\n' << std::flush;
            (void)StopEventLoop(m_session);
        }
        bool SaveView(const std::string& name)
        {
            const auto* endpoint = m_session.GetRenderViewEndpoint("primary-3d");
            if (!endpoint || !endpoint->renderWindow) return false;
            std::error_code error;
            std::filesystem::create_directories("out/build", error);
            if (error) return false;
            endpoint->renderWindow->Render();
            endpoint->renderWindow->WaitForCompletion();
            vtkNew<vtkWindowToImageFilter> capture;
            capture->SetInput(endpoint->renderWindow);
            capture->ReadFrontBufferOff();
            capture->SetInputBufferTypeToRGB();
            capture->ShouldRerenderOff();
            vtkNew<vtkPNGWriter> writer;
            const auto path = "out/build/demo-" + name + ".png";
            writer->SetFileName(path.c_str());
            writer->SetInputConnection(capture->GetOutputPort());
            writer->Write();
            return writer->GetErrorCode() == 0;
        }
        void SendStep()
        {
            if (m_index == m_steps.size()) { Finish(true); return; }
            if (m_getFailure) {
                const auto failure = m_getFailure();
                if (!failure.empty()) {
                    std::cerr << "[DemoAudit] " << failure << '\n';
                    Finish(false);
                    return;
                }
            }
            auto& step = m_steps[m_index];
            if (!m_isSent) {
                std::cout << "[DemoAudit] " << step.name << '\n' << std::flush;
                m_stepStarted = std::chrono::steady_clock::now();
                if (!SendKey(step.key)) { Finish(false); return; }
                m_isSent = true;
                m_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
                return;
            }
            if (!step.ready || step.ready()) {
                std::cout << "[DemoAudit] completed=" << step.name << " elapsed_ms="
                    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_stepStarted).count() << '\n';
                if ((step.name == "part select" || step.name == "surface threshold")
                    && !SaveView(step.name == "part select" ? "parts" : "surface")) { Finish(false); return; }
                ++m_index;
                m_isSent = false;
                return;
            }
            if (std::chrono::steady_clock::now() > m_deadline) Finish(false);
        }
        VtkAppHostSession& m_session;
        std::shared_ptr<FeatureHostControl> m_host;
        std::vector<Step> m_steps;
        std::function<std::string()> m_getFailure;
        std::size_t m_index = 0;
        std::chrono::steady_clock::time_point m_lastTick{};
        std::chrono::steady_clock::time_point m_stepStarted{};
        std::vector<double> m_tickGapsMs;
        std::chrono::steady_clock::time_point m_deadline{};
        bool m_isActive = false;
        bool m_isQueued = false;
        bool m_isSent = false;
        bool m_isDone = false;
        bool m_isPassed = false;
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
    const bool isDemo = GetArgFound(argc, argv, "--demo");
    const bool isDemoAudit = GetArgFound(argc, argv, "--demo-audit");
    const bool isRealAudit = GetArgFound(argc, argv, "--real-audit");
    // 后端切换和初始化都不是线程安全 API；必须在任何 Feature worker 启动前完成。
    // 构建若未包含 STDThread，则显式回退 Sequential，保持功能可用。
    const bool isThreaded =
        vtkSMPTools::SetBackend("STDThread");
    if (!isThreaded) {
        (void)vtkSMPTools::SetBackend("Sequential");
    }
    vtkSMPTools::Initialize(std::max(1, std::min(8, vtkSMPTools::GetEstimatedDefaultNumberOfThreads() / 2)));

    auto renderViews = BuildViews();
    if (isDemo || isDemoAudit) {
        for (auto& view : renderViews) {
            view.window.viewInit.hasIso = true;
            view.window.viewInit.isoThreshold = 0.5;
            view.window.viewInit.hasVolumeTransferFunction = true;
            view.window.viewInit.volumeTransferFunction = {
                {{0.0, 0.0, 0.0, 0.0}, {1.0, 0.85, 0.85, 0.9}},
                {{0.0, 0.0}, {1.0, 0.8}}
            };
        }
    }
    if (isDemoAudit || isRealAudit) {
        for (auto& view : renderViews) view.inputMode = HostInputMode::HostInjected;
    }
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
    primaryRequest.volumeQuality = isDemoAudit ? HostVolumeQuality::Low : HostVolumeQuality::Auto;
    primaryRequest.visibility = planeVisibility;
    primaryRequest.iso = 0.5; // 本例初始值；U 将用当前原始数据的自动 ISO50 更新它。
    if (!session.SendRequest(std::move(primaryRequest))) {
        return 1;
    }

    HostViewSetRequest volumeRequest;
    volumeRequest.targetView = volumeTarget;
    volumeRequest.volumeQuality = isDemoAudit ? HostVolumeQuality::Low : HostVolumeQuality::Auto;
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
    auto cropFeature = std::make_shared<CropHostFeature>(
        BuildCrop(allViews));
    features.push_back(cropFeature);
    auto gapConfig = GetGapConfig(allViews);
    if (isDemo || isDemoAudit) {
        gapConfig.defaultStart.surface.absoluteIsoValue = 0.5;
        gapConfig.defaultStart.surface.backgroundMean = 0.0F;
        gapConfig.defaultStart.surface.materialMean = 1.0F;
    }
    auto gapStart = gapConfig.defaultStart;
    auto gapFeature = std::make_shared<GapHostFeature>(
        std::move(gapConfig));
    features.push_back(gapFeature);
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    auto partConfig = GetPartConfig();
    auto partStart = partConfig.defaultStart;
    auto partFeature = std::make_shared<PartSegmentationHostFeature>(
        std::move(partConfig));
    features.push_back(partFeature);
    auto partControlViews = allViews;
    auto partControlFeature = std::make_shared<PartControlFeature>(
        session,
        volumeTarget,
        std::move(partControlViews),
        partFeature,
        std::move(partStart));
    features.push_back(partControlFeature);
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    SurfaceDeterminationConfig surfaceConfig;
    surfaceConfig.defaultStart.targetViews.viewIds = { primaryTarget.viewId };
    surfaceConfig.defaultStart.method = SurfaceDeterminationMethod::AutomaticIso50;
    surfaceConfig.maxWorkingBytes = 64U * 1024U * 1024U;
    auto surfaceFeature = std::make_shared<SurfaceDeterminationHostFeature>(surfaceConfig);
    features.push_back(surfaceFeature);
#endif
    // G 应在任一 MVVCVTK 窗口获得焦点时都能启动；状态统一显示在 composite-volume 标题栏。
    auto controlViews = allViews;
    auto controlFeature = std::make_shared<MainControlFeature>(
        session,
        volumeTarget,
        primaryTarget,
        std::move(controlViews),
        cropFeature,
        gapFeature,
        std::move(gapStart));
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    controlFeature->SetSurfaceFeature(surfaceFeature);
#endif
    features.push_back(controlFeature);
    auto demoAudit = std::make_shared<DemoAuditFeature>(session);
    demoAudit->SetFailureCheck([&]() -> std::string {
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        if (surfaceFeature->GetState().stage == SurfaceDeterminationStage::Failed)
            return surfaceFeature->GetState().errorMessage;
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        if (partFeature->GetState().status == PartSegmentationStatus::Failed)
            return "Part failed; see callback reason and message.";
#endif
        return {};
    });
    if (isDemoAudit) {
        const auto ready = [] { return true; };
        demoAudit->AddStep("help", {0, "F1"}, ready);
        demoAudit->AddStep("image and graph", {0, "F2"}, [&session] { return session.GetImageDescriptor().has_value(); });
        demoAudit->AddStep("frame snapshot", {0, "F4"}, [&session] { return session.GetSceneViewStates().size() == 5; });
        demoAudit->AddStep("volume quality", {'l'}, [&session, volumeTarget] {
            const auto state = session.GetRenderViewState(volumeTarget);
            return state && state->volumeQuality == HostVolumeQuality::High;
        });
        demoAudit->AddStep("iso quality", {'i'}, [&session, primaryTarget] {
            const auto state = session.GetRenderViewState(primaryTarget);
            return state && state->volumeQuality == HostVolumeQuality::High;
        });
        demoAudit->AddStep("fit views", {0, "F5"}, [&session, primaryTarget] {
            const auto scene = session.GetSceneViewState(primaryTarget);
            return scene && scene->camera && scene->camera->parallelScale > 1.0;
        });
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        demoAudit->AddStep("part start", {'b'}, [partFeature] {
            const auto state = partFeature->GetState();
            return state.status == PartSegmentationStatus::Succeeded && state.partCount == 2;
        });
        const auto selectedPart = [partFeature]() -> std::optional<PartSnapshot> {
            const auto set = partFeature->GetPartSetSnapshot();
            if (!set) return {};
            const auto found = std::find_if(set->parts.begin(), set->parts.end(),
                [](const PartSnapshot& part) { return part.presentation.isSelected; });
            return found == set->parts.end() ? std::nullopt : std::optional<PartSnapshot>{*found};
        };
        demoAudit->AddStep("part select", {'n'}, [selectedPart] { return selectedPart().has_value(); });
        demoAudit->AddStep("part previous wraps", {'n', {}, false, false, true}, [selectedPart] {
            const auto part = selectedPart(); return part && part->labelId == 2;
        });
        demoAudit->AddStep("part previous", {'n', {}, false, false, true}, [selectedPart] {
            const auto part = selectedPart(); return part && part->labelId == 1;
        });
        demoAudit->AddStep("part review", {'r', {}, true}, [selectedPart] {
            const auto part = selectedPart(); return part && part->userState.isReviewed;
        });
        demoAudit->AddStep("part hide", {'h', {}, true}, [selectedPart] {
            const auto part = selectedPart(); return part && !part->presentation.isVisible;
        });
        demoAudit->AddStep("part show", {'h', {}, true}, [selectedPart] {
            const auto part = selectedPart(); return part && part->presentation.isVisible;
        });
        demoAudit->AddStep("part hide all", {'b', {}, false, false, true}, [partFeature] { return !partFeature->GetState().isOverlayVisible; });
        demoAudit->AddStep("part show all", {'b', {}, false, false, true}, [partFeature] { return partFeature->GetState().isOverlayVisible; });
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        demoAudit->AddStep("surface threshold", {'u'}, [surfaceFeature, &session, primaryTarget] {
            const auto snapshot = surfaceFeature->GetSurfaceSnapshot();
            const auto view = session.GetRenderViewState(primaryTarget);
            return surfaceFeature->GetState().stage == SurfaceDeterminationStage::Ready
                && snapshot && snapshot->isoEstimate && view
                && view->isoThreshold == snapshot->isoEstimate->isoValue;
        });
        demoAudit->AddStep("surface clear", {'u', {}, true}, [surfaceFeature] { return !surfaceFeature->GetSurfaceSnapshot(); });
#endif
        demoAudit->AddStep("gap start", {'g'}, [gapFeature] {
            const auto state = gapFeature->GetState();
            return state.analysisState == GapAnalysisState::Succeeded && GetDataRevisionRefValid(state.labelMap);
        });
        demoAudit->AddStep("label maps", {0, "F3"}, [&session] {
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
            return session.GetLabelMapDescriptors().size() == 2;
#else
            return session.GetLabelMapDescriptors().size() == 1;
#endif
        });
        demoAudit->AddStep("crop box", {'o'}, [cropFeature] { return cropFeature->GetState().isActive; });
        demoAudit->AddStep("crop plane", {'p'}, [cropFeature] { return cropFeature->GetState().isActive; });
        demoAudit->AddStep("final graph", {0, "F2"}, ready);
        features.push_back(demoAudit);
    }

    if (isRealAudit) {
        demoAudit->AddStep("real image", {0, "F2"}, [&session] { return session.GetImageDescriptor().has_value(); });
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        demoAudit->AddStep("surface threshold", {'u'}, [surfaceFeature, &session, primaryTarget] {
            const auto snapshot = surfaceFeature->GetSurfaceSnapshot();
            const auto view = session.GetRenderViewState(primaryTarget);
            return surfaceFeature->GetState().stage == SurfaceDeterminationStage::Ready
                && snapshot && snapshot->isoEstimate && view
                && view->isoThreshold == snapshot->isoEstimate->isoValue;
        });
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        demoAudit->AddStep("part start", {'b'}, [partFeature] {
            return partFeature->GetState().status == PartSegmentationStatus::Succeeded
                && partFeature->GetState().partCount > 0;
        });
        demoAudit->AddStep("part select", {'n'}, [partFeature] {
            const auto snapshot = partFeature->GetPartSetSnapshot();
            return snapshot && std::any_of(snapshot->parts.begin(), snapshot->parts.end(),
                [](const PartSnapshot& part) { return part.presentation.isSelected; });
        });
        demoAudit->AddStep("real labels", {0, "F3"}, [&session] { return !session.GetLabelMapDescriptors().empty(); });
        demoAudit->AddStep("part restart", {'b'}, [partFeature] {
            return partFeature->GetState().status == PartSegmentationStatus::Running;
        });
        demoAudit->AddStep("part cancel preserves result", {'b', {}, false, true}, [partFeature] {
            const auto state = partFeature->GetState();
            return state.status == PartSegmentationStatus::Succeeded && state.resultRevision == 1
                && partFeature->GetPartSetSnapshot() && state.partCount > 0;
        });
#endif
        demoAudit->AddStep("real frames", {0, "F4"}, [] { return true; });
        features.push_back(demoAudit);
    }


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
    const bool isPartAuto = GetArgFound(
        argc, argv, "--part-auto");
    const bool isPartManual = GetArgFound(
        argc, argv, "--part-manual");
    const bool isPartProfile = GetArgFound(
        argc, argv, "--part-profile");
    const int runModeCount = static_cast<int>(isDragAudit)
        + static_cast<int>(isQualityAudit)
        + static_cast<int>(isGapAuto)
        + static_cast<int>(isPartAuto)
        + static_cast<int>(isPartManual)
        + static_cast<int>(isPartProfile)
        + static_cast<int>(isDemo) + static_cast<int>(isDemoAudit) + static_cast<int>(isRealAudit);
    if (runModeCount > 1) {
        std::cerr
            << "--drag-audit, --quality-audit, --gap-auto and "
            "PartSegmentation / demo run modes "
            "are mutually exclusive\n";
        if (!clearAttached()) return 25;
        features.clear();
        return 8;
    }
#if !defined(MVVCVTK_HAS_PART_SEGMENTATION)
    if (isPartAuto || isPartManual || isPartProfile) {
        std::cerr
            << "PartSegmentation run modes require "
            "MVVCVTK_BUILD_PART_SEGMENTATION=ON\n";
        if (!clearAttached()) return 25;
        features.clear();
        return 8;
    }
#endif
    bool isAuditComplete = false;
    bool isAuditPassed = false;
    bool isPartComplete = false;
    bool isPartPassed = false;
    bool isPartManualReady = false;
    bool isDemoReady = false;

    HostResultCallback onDataReady =
        [&](HostResult result) {
        if (result.isSucceeded) controlFeature->StartDemoFit();
        if (isDemo || isDemoAudit || isRealAudit) {
            if (!result.isSucceeded) {
                std::cerr << "[Demo] data load failed: " << result.message << '\n';
                (void)StopEventLoop(session);
                return;
            }
            std::cout << "[Demo] data ready. Press B / G / U to show features; F1 for all keys.\n" << std::flush;
            isDemoReady = true;
            if (isDemoAudit || isRealAudit) demoAudit->Start();
            return;
        }
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
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        else if (isPartAuto) {
            if (!result.isSucceeded) {
                isPartComplete = true;
                isPartPassed = false;
                std::cerr
                    << "QT_PART_RESULT: data reload failed; "
                    "request skipped\n"
                    << std::flush;
                (void)StopEventLoop(session);
                return;
            }
            (void)StartPartQtSim(
                session, partFeature,
                std::size_t{ 2 },
                isPartComplete, isPartPassed);
            return;
        }
        else if (isPartProfile) {
            if (!result.isSucceeded) {
                isPartComplete = true;
                isPartPassed = false;
                std::cerr
                    << "QT_PART_RESULT: real data load failed; "
                    "request skipped\n"
                    << std::flush;
                (void)StopEventLoop(session);
                return;
            }
            (void)StartPartQtSim(
                session, partFeature,
                std::nullopt,
                isPartComplete, isPartPassed);
            return;
        }
        else if (isPartManual) {
            isPartManualReady = result.isSucceeded;
            if (!isPartManualReady) {
                std::cerr
                    << "[PartSegmentation] synthetic data reload failed\n"
                    << std::flush;
                (void)StopEventLoop(session);
                return;
            }
            std::cout
                << "[PartSegmentation] synthetic data ready; "
                "press B to start\n"
                << std::flush;
            return;
        }
#endif
        else {
            return;
        }
        (void)StopEventLoop(session);
        };

    bool isDataAccepted = false;
    if (isDemo || isDemoAudit || isPartAuto || isPartManual) {
        auto reload = BuildDemoReload(isDemo || isDemoAudit);
        isDataAccepted = session.SendRequestResult(
            std::move(reload), onDataReady);
    }
    else
    {
        HostLoadRequest load;
        load.filePath = "F:\\data\\ct\\1536x1536x1536_1440.raw";
        load.geometry.dimensions = { 1536, 1536, 1536 };
        load.geometry.spacing = {
            0.1537f, 0.1537f, 0.1537f };
        load.geometry.origin = { 0.0f, 0.0f, 0.0f };
        load.metadata.identity.datasetId =
            "standalone-ct-1536x1536x1536";
        load.metadata.source.kind = ImageSourceKind::RawFile;
        load.metadata.source.uri = load.filePath;
        isDataAccepted = session.SendRequestResult(
            std::move(load), onDataReady);
    }
    if (!isDataAccepted) {
        if (!clearAttached()) {
            return 23;
        }
        return 5;
    }

    PrintDemoHelp();

    const bool isStarted = session.Start();
    if (isQualityAudit) {
        isAuditComplete = controlFeature->GetQualityAuditDone();
        isAuditPassed = controlFeature->GetQualityAuditPassed();
    }
    const bool isDemoPassed = !(isDemoAudit || isRealAudit) || demoAudit->GetPassed();
    const bool isCleared = clearAttached();
    if (!isCleared) {
        return 24;
    }
    features.clear();
    const bool isStopped = session.Stop();
    if (!isStarted) return 6;
    if (!isStopped) return 26;
    if ((isDragAudit || isQualityAudit)
        && (!isAuditComplete || !isAuditPassed)) {
        return 7;
    }
    if ((isPartAuto || isPartProfile)
        && (!isPartComplete || !isPartPassed)) {
        return 9;
    }
    if (isPartManual && !isPartManualReady) return 10;
    if (!isDemoPassed) return 11;
    if (isDemo && !isDemoReady) return 12;
    return 0;
}
