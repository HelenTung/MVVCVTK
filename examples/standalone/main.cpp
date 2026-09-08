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

#include "FeatureTestControls.h"
#include "Host/CropHostFeature.h"
#include "Host/GapHostFeature.h"
#include "Host/HostFeature.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/VtkAppHostSession.h"
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
#include "Host/ModelRotationHostFeature.h"
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
#include "Host/PartSegmentationHostFeature.h"
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
#include "Host/SurfaceDeterminationHostFeature.h"
#endif

#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT)
#include "Host/MetrologyAlignmentHostFeature.h"
#endif
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
#include "Host/ArtifactReductionHostFeature.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
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
#include <thread>
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
        if (!GetDataRevisionRefValid(revision)) return "无";
        std::ostringstream text;
        text << std::hex << std::setfill('0');
        for (const auto value : revision.entityId.bytes) text << std::setw(2) << static_cast<unsigned int>(value);
        text << ':' << std::dec << revision.generation;
        return text.str();
    }

    void PrintDemoHelp()
    {
        std::cout << "\n========== main 操作帮助 ==========\n"
            << "完整帮助和详细结果输出到启动程序的终端；窗口标题只显示简短状态。\n"
            << "运行 MVVCVTK.exe --help 可只查看帮助，不加载体数据、不创建视图窗口。\n"
            << "操作前先点击任一视图窗口。字母键使用英文输入状态；有拼音候选框时按 Shift 切换。\n"
            << "下文的大写字母表示键名，单按 B 即可；只有明确写出 Shift+B 时才需要按住 Shift。\n"
            << "F1 或 Ctrl+F1：重新输出本帮助；F1 被其他软件占用时使用 Ctrl+F1。\n"
            << "\n【先完成一条真实数据流程】\n"
            << "  1. 用 --input=路径 和 --dimensions=X,Y,Z 启动，等待“真实数据已就绪”。\n";
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        std::cout << "  2. 按 U，等待阈值估计完成；A 窗口会应用估计出的等值面阈值。\n";
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        std::cout << "  3. 按 B，等待“零件分割：成功”；再用 N 选择零件，按 F7 准备默认合并候选。\n"
            << "  4. 等待“候选结果已就绪”，核对终端中的零件数，再按 Ctrl+F7 确认。\n";
#endif
        std::cout << "  裁切、伪影校正和网格提取可分别按下面的流程使用。任务已请求不等于任务已完成。\n"
            << "\n【窗口、查看状态与渲染】\n"
            << "  A：三维等值面；E：体渲染；B/C/D：上下、前后、左右方向切片。\n"
            << "  F2：输出当前图像的尺寸、间距、数据版本和数据图信息。\n"
            << "  F3：输出标签数据与采样值；F4：输出场景、渲染帧及就绪状态。\n"
            << "  F5：重置所有视图，使模型重新适配窗口。\n"
            << "  L / Shift+L：E 窗口体渲染质量升/降一档。I / Shift+I：A 窗口等值面质量升/降一档。\n"
            << "  C / Shift+C：调整显示颜色；V / Shift+V：提高/降低不透明度。\n"
            << "  显示质量档位只调整渲染，不会把算法输入改成较小的体数据。\n"
            << "\n【裁切：先预览，再生成并选择算法输入】\n"
            << "  预览：O 方框、P 平面、Shift+O 有限圆柱、Shift+P 球体；按 1 保留内部，按 2 移除内部，然后拖动控件。\n"
            << "  0：将当前裁切编辑模式设为不移除；它不会清空已提交的裁切历史。\n"
            << "  4 / 5：翻阅历史树的上一页/下一页；Alt+1..9：预览终端本页列出的明确节点。\n"
            << "  Alt+0：只高亮 Root，不改预览或结果；Ctrl+9：返回源数据，等待结果实际释放。\n"
            << "  Ctrl+Delete：修剪高亮子树；Alt+Delete：修剪后代；Shift+Delete：仅保留高亮路径。受保护节点会拒绝。\n"
            << "  Escape：退出裁切控件，已生效的裁切仍保留；退出控件不等于恢复完整输入。\n"
            << "  Ctrl+7：对高亮的正式节点生成结果；选择保留/移除模式，拖动后松开鼠标并等待提交。\n"
            << "  Ctrl+8：生成成功后，将裁切结果设为当前输入，后续分割/网格/校正才会读取该结果。\n"
            << "  Ctrl+O：从当前输入创建独立文档；Ctrl+Tab：切换文档；Ctrl+Shift+Delete：关闭文档。\n"
            << "  兼容快捷键：Ctrl+3 等同 Ctrl+7，普通数字 6 等同 Ctrl+9。\n"
            << "  例：O → 1 → 拖动右侧控件 → Ctrl+7 → 等待完成 → Ctrl+8 → U → B。\n"
            << "  裁切结果保留原网格尺寸，并用有效性掩码记录保留区域；不会按框大小减少整卷内存。\n"
            << "\n【孔隙分析】\n"
            << "  G：对当前输入启动分析；进度和孔隙统计显示在 E 窗口标题及终端中。\n"
            << "  H：隐藏/显示孔隙覆盖层，保留分析数据。\n"
            << "  孔隙交互状态下按 Escape：退出孔隙显示、清除当前孔隙结果选择，并请求停止运行中的任务。\n"
            << "  G 使用 main 预设参数，真实数据的绝对阈值为 0.172；U 不会自动修改这个阈值。\n";
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        std::cout << "\n【零件分割与选择】\n"
            << "  B：按 A 窗口当前等值面阈值重新分割当前输入；建议先按 U 并等待阈值估计完成。\n"
            << "  N / Shift+N：选择下一个/上一个零件；未选择时从第一个零件开始。\n"
            << "  --part-picking：启动时额外启用鼠标点击选件；键盘 N 选择无需此选项。\n"
            << "  Ctrl+H：切换所选零件的可见性；Ctrl+R：切换所选零件的已审核标记。\n"
            << "  Shift+B：隐藏/显示全部零件覆盖层；Alt+B：请求停止正在运行的零件任务。\n"
            << "  Ctrl+B：清除当前零件功能结果和显示。隐藏、清除均不保证数据图历史立即释放内存。\n";
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        std::cout << "\n【自动阈值】\n"
            << "  U：估计 ISO50 等值面阈值，成功后应用到 A 窗口；此操作只估计阈值。\n"
            << "  Ctrl+U：清除表面确定功能结果和网格覆盖层，保留已应用的视图阈值；Alt+U：取消当前任务。\n"
            << "  需要网格时，在阈值就绪后按 K；详见下方“表面网格”。VS 调试时用 K 避开系统 F12 中断。\n";
#endif
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
        std::cout << "\n【模型旋转】\n"
            << "  J：启用/退出旋转工具；启用后拖动旋转，Shift+拖动平移，Ctrl+Shift+拖动缩放。\n"
            << "  Shift+J / Ctrl+J：绕 Z 轴旋转 +15 / -15 度；Alt+J：撤销上一次模型变换。\n"
            << "  Escape：取消尚未结束的拖动。旋转入口会先关闭裁切控件，保留已经提交的裁切。\n";
#endif
        std::cout << "\n【其他操作】\n"
            << "  M：切换当前视图的交互模式。S：导出数据（PLY）；T：导出切片。导出目标按 main 配置为 F:\\data。\n"
            << "  Escape 优先交给当前工具处理；无活动工具时将当前视图切回导航模式。\n"
            << "  Escape 不用于关闭程序；关闭程序使用窗口关闭按钮。\n";
        PrintFeatureTestHelp();
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
            GapHostStartParams gapStart,
            HostHotkeyConfig appHotkeys)
            : m_session(session),
            m_volumeTarget(std::move(volumeTarget)),
            m_isoTarget(std::move(isoTarget)),
            m_inputViews(std::move(inputViews)),
            m_cropFeature(std::move(cropFeature)),
            m_gapFeature(std::move(gapFeature)),
            m_gapStart(std::move(gapStart)),
            m_appHotkeys(std::move(appHotkeys)),
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
                HostKeyChord{ 'u', {}, false, true },
                HostKeyChord{ 'o' }, HostKeyChord{ 'p' },
                HostKeyChord{ 'o', {}, false, false, true }, HostKeyChord{ 'p', {}, false, false, true },
                HostKeyChord{ '0' }, HostKeyChord{ '1' }, HostKeyChord{ '2' },
                HostKeyChord{ '4' }, HostKeyChord{ '5' },
                HostKeyChord{ '3', {}, true }, HostKeyChord{ '6' },
                HostKeyChord{ 0, "Escape" },
                HostKeyChord{ '0', {}, false, true },
                HostKeyChord{ '1', {}, false, true },
                HostKeyChord{ '2', {}, false, true },
                HostKeyChord{ '3', {}, false, true },
                HostKeyChord{ '4', {}, false, true },
                HostKeyChord{ '5', {}, false, true },
                HostKeyChord{ '6', {}, false, true },
                HostKeyChord{ '7', {}, false, true },
                HostKeyChord{ '8', {}, false, true },
                HostKeyChord{ '9', {}, false, true },
                HostKeyChord{ 'o', {}, true }, HostKeyChord{ 0, "Tab", true },
                HostKeyChord{ 0, "Delete", true, false, true },
                HostKeyChord{ 0, "Delete", true }, HostKeyChord{ 0, "Delete", false, true },
                HostKeyChord{ 0, "Delete", false, false, true }
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
                , HostKeyChord{ 'j' }
                , HostKeyChord{ 'j', {}, false, false, true }
                , HostKeyChord{ 'j', {}, true }
                , HostKeyChord{ 'j', {}, false, true }
#endif
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
            m_appKeyDown.fill(false);
            m_cropStatus.clear();m_cropRequestPending=false;m_cropUiDocument=0;m_cropHighlighted=0;
            m_cropPageNodes.clear();m_cropPageStarts.clear();
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
            SendCropStatus();
            return SendQualityAudit();
        }

        void StartDemoFit() { m_isDemoFitPending = true; }
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
        void SetRotationFeature(std::weak_ptr<ModelRotationHostFeature> feature)
        { m_rotationFeature = std::move(feature); }
#endif

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
            CropBox, CropPlane, CropCylinder, CropSphere, CropNoMode, CropKeepMode, CropRemoveMode,
            CropPagePrevious, CropPageNext, CropBuildAlias, CropRestoreAlias, CropExit,
            CropNode0, CropNode1, CropNode2, CropNode3, CropNode4,
            CropNode5, CropNode6, CropNode7, CropNode8, CropNode9,
            CropCreate, CropDocumentNext, CropClose, CropPruneSubtree, CropPruneDescendants, CropPruneOutside,
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
            RotationTool, RotationPlus, RotationMinus, RotationUndo,
#endif
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
            if (event.keySym == "F1" && event.isCtrlDown
                && !event.isAltDown && !event.isShiftDown) return ControlAction::Help;
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
                valueName = "颜色红色分量";
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
                valueName = "不透明度";
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
            std::cout << "[传递函数] " << valueName
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
                "自动", "低", "高", "超高", "极高"
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

            std::cout << "[渲染质量] 视图=" << target.viewId << ' '
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
                (void)SetDemoStatus("孔隙分析：功能不可用");
                std::cerr
                    << "[孔隙分析] G 请求被拒绝：功能不可用\n"
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
                    status << "孔隙分析："
                        << (isSuccess ? "成功" : "失败");
                    if (isSuccess && completedFeature) {
                        const auto statistics =
                            completedFeature->GetState().statistics;
                        status << " | 孔隙体素数="
                            << statistics.voidVoxelCount
                            << " | 对象体素数="
                            << statistics.objectVoxelCount
                            << " | 孔隙率="
                            << statistics.porosityRatio;
                    }
                    if (const auto owner = controlOwner.lock()) {
                        (void)owner->SetDemoStatus(status.str());
                    }
                    std::cerr << "[孔隙分析] "
                        << status.str()
                        << " | 提交编号=" << result.commitId
                        << " | 源数据版本="
                        << result.sourceRevision.generation
                        << " | 标签版本="
                        << result.labelMap.generation
                        << " | 孔隙表版本="
                        << result.voidTable.generation
                        << " | 网格版本="
                        << result.voidMesh.generation
                        << " | 统计版本="
                        << result.statisticsData.generation
                        << " | 结果版本="
                        << result.resultSet.generation
                        << '\n' << std::flush;
                });
            const std::string requestStatus = isAccepted
                ? "孔隙分析：计算中"
                : "孔隙分析：请求被拒绝";
            (void)SetDemoStatus(requestStatus);
            std::cerr << "[孔隙分析] G 请求"
                << (isAccepted ? "已接受；计算已开始"
                    : "被拒绝；计算未开始")
                << '\n' << std::flush;
            return isAccepted;
        }

        static const char* GetCropFailureText(CropFailure failure)
        {
            switch(failure) {
            case CropFailure::None:return "完成";
            case CropFailure::Busy:return "请等待当前任务完成";
            case CropFailure::StateVersionMismatch:return "历史已更新，请重新选择";
            case CropFailure::PublishedResultDependency:return "节点仍被结果保护";
            case CropFailure::ReturnToSourceRequired:return "请先返回源数据并释放结果";
            case CropFailure::ResultInUse:return "结果仍被其他任务或读者使用";
            case CropFailure::ResultReleasing:return "正在等待结果资源释放";
            case CropFailure::ResourceLimit:return "达到资源预算上限";
            case CropFailure::PrecisionNotMet:return "无法满足指定精度";
            case CropFailure::NoCropOperations:return "请选择一个裁切节点";
            case CropFailure::EmptyResult:return "裁切结果为空，原结果保留";
            case CropFailure::Cancelled:return "已取消";
            default:return "请求失败，请检查所选文档和节点";
            }
        }

        CropHostTarget GetCropTarget() const
        {
            CropHostTarget target;target.inputBinding=std::string(primaryVolumeBinding);
            target.referenceView=m_isoTarget;target.targetViews=m_inputViews;return target;
        }

        void SendCropStatus(bool printPage=false)
        {
            const auto crop=m_cropFeature.lock();if(!m_isAttached||!crop||!m_host)return;
            const auto state=crop->GetState();const auto history=crop->GetHistory(0,0,1);
            if(!history.documentId){m_cropUiDocument=0;m_cropHighlighted=0;m_cropPageNodes.clear();m_cropStatus.clear();return;}
            if(m_cropUiDocument!=history.documentId||m_cropUiRevision!=history.stateRevision) {
                if(m_cropUiDocument!=history.documentId||m_cropHighlighted==m_cropLastApplied)m_cropHighlighted=history.appliedHead;
                m_cropLastApplied=history.appliedHead;
                m_cropUiDocument=history.documentId;m_cropUiRevision=history.stateRevision;
                m_cropPageStarts={history.rootNodeId};m_cropPageIndex=0;printPage=true;
                if(m_cropHighlighted) {
                    const auto selected=crop->GetHistory(history.documentId,m_cropHighlighted-1,1);
                    if(selected.nodes.empty()||selected.nodes.front().nodeId!=m_cropHighlighted)m_cropHighlighted=history.appliedHead;
                }
            }
            const auto page=crop->GetHistory(history.documentId,m_cropPageStarts[m_cropPageIndex],9);
            m_cropPageNodes=page.nodes;m_cropPageNext=page.nextPageAfter;
            std::ostringstream status;
            status<<"裁切文档 "<<history.documentId<<" | 高亮 "<<m_cropHighlighted<<" | 预览 "<<history.appliedHead;
            for(const auto& result:history.results)if(result.status==CropResultStatus::Published)status<<" | 结果节点 "<<result.nodeId;
            if(state.documentStatus==CropDocumentStatus::Building)status<<" | 生成中";
            if(state.documentStatus==CropDocumentStatus::Returning||state.documentStatus==CropDocumentStatus::Releasing||state.documentStatus==CropDocumentStatus::Closing)
                status<<" | 等待释放，阻塞 "<<state.blockers.size();
            status<<" | 历史页 "<<(m_cropPageIndex+1)<<"，4/5 翻页，Alt+数字选节点";
            if(status.str()!=m_cropStatus&&SetDemoStatus(status.str()))m_cropStatus=status.str();
            if(!printPage)return;
            std::cout<<"\n[裁切历史] 文档 "<<history.documentId<<"，共 "<<history.totalNodeCount<<" 节点，第 "<<(m_cropPageIndex+1)<<" 页\n"
                <<"  Alt+0  Root #"<<history.rootNodeId<<"（仅高亮；Ctrl+9 才返回源数据）\n";
            for(std::size_t index=0;index<page.nodes.size();++index) {
                const auto& node=page.nodes[index];const char* shape="Root";
                if(node.operation){switch(node.operation->geometryType){case CropShape::Box:shape="盒";break;case CropShape::Plane:shape="平面";break;
                    case CropShape::Cylinder:shape="圆柱";break;case CropShape::Sphere:shape="球";break;}}
                std::cout<<"  Alt+"<<(index+1)<<"  #"<<node.nodeId<<" <- #"<<node.parentNodeId<<"  "<<shape
                    <<(node.nodeId==m_cropHighlighted?" [高亮]":"")<<(node.nodeId==history.appliedHead?" [预览]":"")<<'\n';
            }
            std::cout<<"  每行列出父节点；所有分支均可翻页选择。Ctrl+Tab 切换文档。\n"<<std::flush;
        }

        bool ChangeCropPage(bool next)
        {
            SendCropStatus();if(!m_cropUiDocument)return false;
            if(next){if(!m_cropPageNext)return false;
                if(m_cropPageStarts.size()==m_cropPageIndex+1)m_cropPageStarts.push_back(m_cropPageNext);
                else m_cropPageStarts[m_cropPageIndex+1]=m_cropPageNext;++m_cropPageIndex;}
            else {if(!m_cropPageIndex)return false;--m_cropPageIndex;}
            SendCropStatus(true);return true;
        }

        bool SelectCropRow(std::size_t row)
        {
            const auto crop=m_cropFeature.lock();if(!crop||m_cropRequestPending)return false;
            SendCropStatus();const auto history=crop->GetHistory(0,0,1);if(!history.documentId)return false;
            if(!row){m_cropHighlighted=history.rootNodeId;SendCropStatus(true);return true;}
            if(row>m_cropPageNodes.size())return false;
            const auto node=m_cropPageNodes[row-1].nodeId;
            CropEditRequest request;request.documentId=history.documentId;request.nodeId=node;request.kind=CropEditKind::Select;
            request.requestId=CropHostFeature::CreateRequestId();request.expectedRevision=history.stateRevision;
            const auto weak=weak_from_this();const auto accepted=crop->SendRequest(request,[weak,node](auto outcome) {
                if(const auto owner=weak.lock()){owner->m_cropRequestPending=false;
                    if(outcome.status==CropEditStatus::Succeeded)owner->m_cropHighlighted=node;
                    owner->SendCropStatus(true);(void)owner->SetDemoStatus(GetCropFailureText(outcome.failureReason));}
            });
            m_cropRequestPending=accepted.isAccepted;
            if(!accepted)(void)SetDemoStatus(GetCropFailureText(accepted.failureReason));return accepted.isAccepted;
        }

        bool SendCropDocument(CropDocumentAction action,std::optional<CropHostAction> after={},std::optional<CropRemovalMode> afterMode={})
        {
            const auto crop=m_cropFeature.lock();if(!crop||!m_data||m_cropRequestPending)return false;
            const auto history=crop->GetHistory(0,0,1);
            CropDocumentRequest request;request.action=action;request.requestId=CropHostFeature::CreateRequestId();
            if(action==CropDocumentAction::CreateDocument) {
                const auto binding=m_data->GetDataBinding(m_data->GetDataGraph(),primaryVolumeBinding);
                if(!binding||!binding->target)return false;request.sourceRevision=*binding->target;request.target=GetCropTarget();
            } else {
                request.documentId=history.documentId;request.expectedRevision=history.stateRevision;
                if(action==CropDocumentAction::ActivateDocument) {
                    const auto docs=crop->GetDocuments();if(docs.empty()||(docs.size()==1&&docs.front()==history.documentId))return false;
                    const auto found=std::find(docs.begin(),docs.end(),history.documentId);
                    request.documentId=found==docs.end()||found+1==docs.end()?docs.front():*(found+1);
                    request.expectedRevision=crop->GetHistory(request.documentId,0,1).stateRevision;request.target=GetCropTarget();
                }
            }
            const auto weak=weak_from_this();const auto accepted=crop->SendRequest(request,[weak,after,afterMode](auto outcome) {
                if(const auto owner=weak.lock()) {owner->m_cropRequestPending=false;
                    if(!owner->m_isAttached)return;owner->SendCropStatus(true);
                    (void)owner->SetDemoStatus(GetCropFailureText(outcome.failureReason));
                    if(after&&outcome.status==CropEditStatus::Succeeded)(void)owner->SendCrop(*after,afterMode);
                }
            });
            m_cropRequestPending=accepted.isAccepted;
            if(!accepted)(void)SetDemoStatus(GetCropFailureText(accepted.failureReason));return accepted.isAccepted;
        }

        bool PruneCrop(CropPruneScope scope)
        {
            const auto crop=m_cropFeature.lock();if(!crop||m_cropRequestPending)return false;SendCropStatus();
            const auto history=crop->GetHistory(0,0,1);if(!history.documentId||!m_cropHighlighted)return false;
            CropEditRequest request;request.kind=CropEditKind::Prune;request.documentId=history.documentId;
            request.requestId=CropHostFeature::CreateRequestId();request.expectedRevision=history.stateRevision;
            request.prune.scope=scope;request.prune.nodeIds={m_cropHighlighted};request.prune.fallback=CropPruneFallback::NearestSurvivingAncestor;
            const auto impact=crop->GetPruneImpact(history.documentId,request.prune);
            if(impact.failureReason!=CropFailure::None){(void)SetDemoStatus(GetCropFailureText(impact.failureReason));return false;}
            const auto weak=weak_from_this();const auto accepted=crop->SendRequest(request,[weak](auto result) {
                if(const auto owner=weak.lock()){owner->m_cropRequestPending=false;owner->SendCropStatus(true);
                    (void)owner->SetDemoStatus(GetCropFailureText(result.failureReason));}
            });
            m_cropRequestPending=accepted.isAccepted;if(!accepted)(void)SetDemoStatus(GetCropFailureText(accepted.failureReason));return accepted.isAccepted;
        }

        bool SetCropData(bool useResult)
        {
            if(!useResult)return SendCropDocument(CropDocumentAction::ReturnToSource);
            const auto crop=m_cropFeature.lock();const auto descriptor=m_session.GetImageDescriptor();
            if(!crop||!descriptor||m_cropRequestPending)return false;
            HostDataSelectRequest request;request.dataRevision=crop->GetState().outputRevision;request.expectedBindingRevision=descriptor->bindingRevision;
            const bool accepted=m_session.SendRequest(std::move(request));
            (void)SetDemoStatus(accepted?"已选择裁切结果":"数据选择被拒绝");return accepted;
        }

        bool SendCrop(
            const CropHostAction action,
            std::optional<CropRemovalMode> removalMode = {})
        {
            const auto crop = m_cropFeature.lock();
            if (!crop || m_cropRequestPending) return false;
            const auto history=crop->GetHistory(0,0,1);
            if(!history.documentId&&action!=CropHostAction::Exit)
                return SendCropDocument(CropDocumentAction::CreateDocument,action,removalMode);
            if(action!=CropHostAction::Exit&&action!=CropHostAction::BuildResult&&m_data) {
                const auto binding=m_data->GetDataBinding(m_data->GetDataGraph(),primaryVolumeBinding);
                if(binding&&binding->target!=std::optional<DataRevisionRef>{history.sourceRevision}) {
                    if(history.appliedHead==history.rootNodeId)return SendCropDocument(CropDocumentAction::ReturnToSource,action,removalMode);
                    CropEditRequest select;select.documentId=history.documentId;select.nodeId=history.appliedHead;
                    select.requestId=CropHostFeature::CreateRequestId();select.expectedRevision=history.stateRevision;
                    const auto weak=weak_from_this();const auto accepted=crop->SendRequest(select,[weak,action,removalMode](auto outcome) {
                        if(const auto owner=weak.lock()){owner->m_cropRequestPending=false;if(!owner->m_isAttached)return;
                            if(outcome.status==CropEditStatus::Succeeded)(void)owner->SendCrop(action,removalMode);
                            else (void)owner->SetDemoStatus(GetCropFailureText(outcome.failureReason));}
                    });m_cropRequestPending=accepted.isAccepted;return accepted.isAccepted;
                }
            }
            CropHostRequest request;
            request.action = action;
            request.removalMode = removalMode;
            if (action == CropHostAction::Start || action == CropHostAction::Box
                || action == CropHostAction::Plane || action == CropHostAction::Cylinder || action == CropHostAction::Sphere || action == CropHostAction::Mode
                || action == CropHostAction::BuildResult) {
                CropHostTarget target;
                target.inputBinding = std::string(primaryVolumeBinding);
                target.referenceView = m_isoTarget;
                target.targetViews = m_inputViews;
                request.target = std::move(target);
            }
            CropBuildCallback onComplete;
            if (action == CropHostAction::BuildResult) {
                const auto weakOwner = weak_from_this();
                const auto controlRevision = m_controlRevision;
                onComplete = [weakOwner, controlRevision](CropBuildResult result) {
                    const auto owner = weakOwner.lock();
                    if (!owner || !owner->m_isAttached) return;
                    std::ostringstream status;
                    status << "裁剪结果" << (result.isSucceeded ? "已就绪" : "失败")
                        << " | 提交编号=" << result.commitId
                        << " | 源数据版本=" << result.sourceRevision.generation
                        << " | 裁剪方案版本=" << result.recipeRevision.generation
                        << " | 输出版本=" << result.outputRevision.generation
                        << " | " << result.message;
                    const auto crop = owner->m_cropFeature.lock();
                    if (owner->m_controlRevision == controlRevision
                        && crop && crop->GetState().isActive) {
                        (void)owner->SetDemoStatus(status.str());
                    }
                    std::cout << "[正交裁剪] " << status.str() << '\n';
                };
            }
            bool isAccepted=false;
            if(action==CropHostAction::BuildResult) {
                SendCropStatus();CropBuildRequest build;build.documentId=history.documentId;
                build.nodeId=m_cropHighlighted?m_cropHighlighted:history.appliedHead;
                build.requestId=CropHostFeature::CreateRequestId();build.expectedRevision=history.stateRevision;
                const auto accepted=crop->SendRequest(build,std::move(onComplete));isAccepted=accepted.isAccepted;
                if(!accepted)(void)SetDemoStatus(GetCropFailureText(accepted.failureReason));
            } else isAccepted=crop->SendRequest(std::move(request),std::move(onComplete));
            std::cout << "[正交裁剪] 操作=" << static_cast<int>(action)
                << " 已接受=" << isAccepted << '\n';
            if (!isAccepted) (void)SetDemoStatus("裁剪请求被拒绝");
            else if (action == CropHostAction::Exit) (void)SetDemoStatus("裁剪编辑已结束");
            else SendCropStatus();
            return isAccepted;
        }

    private:
        bool PrintData()
        {
            const auto descriptor = m_session.GetImageDescriptor();
            if (!descriptor || !m_data) {
                (void)SetDemoStatus("演示：等待图像加载");
                return false;
            }
            const auto graph = m_data->GetDataGraph();
            if (!graph.view) return false;
            std::cout << "[图像] 数据集=" << descriptor->metadata.identity.datasetId
                << " 尺寸=" << descriptor->dims[0] << 'x' << descriptor->dims[1] << 'x' << descriptor->dims[2]
                << " 体素间距=" << descriptor->spacing[0] << ',' << descriptor->spacing[1] << ',' << descriptor->spacing[2]
                << " 数值范围=" << descriptor->scalarRange[0] << ',' << descriptor->scalarRange[1]
                << " 物理量=" << descriptor->metadata.scalar.quantity << " 单位=" << descriptor->metadata.scalar.unit
                << " 来源=" << descriptor->metadata.source.uri
                << " 版本=" << GetRevisionText(descriptor->dataRevision) << '\n';
            const auto bindings = graph.view->GetDataBindings();
            std::cout << "[数据图] 提交编号=" << graph.commitId << " 绑定数=" << bindings.size() << '\n';
            for (const auto& binding : bindings) {
                if (!binding.target) continue;
                const auto data = graph.view->GetData(*binding.target);
                std::cout << "  " << binding.name << " -> " << GetRevisionText(*binding.target)
                    << " 绑定版本=" << binding.revision;
                if (data) {
                    std::cout << " 类型=" << data->type.name;
                    for (const auto& input : data->inputs)
                        std::cout << " | " << input.role << "=" << GetRevisionText(input.source);
                }
                std::cout << '\n';
            }
            std::ostringstream status;
            status << descriptor->metadata.identity.datasetId << " | " << descriptor->dims[0] << 'x'
                << descriptor->dims[1] << 'x' << descriptor->dims[2] << " | 数据图提交编号=" << graph.commitId;
            (void)SetDemoStatus(status.str());
            return true;
        }

        bool PrintLabels()
        {
            const auto labels = m_session.GetLabelMapDescriptors();
            std::cout << "[标签图] 当前数量=" << labels.size() << "（请先按 B 或 G 运行分析）\n";
            bool isPassed = true;
            for (const auto& label : labels) {
                LabelMapReadRequest request;
                request.id = label.id;
                request.expectedRevision = label.dataRevision;
                request.maxBytes = 8 * label.componentBytes;
                const auto chunk = m_session.GetLabelMapReadChunk(request, 0);
                std::cout << "  " << label.id << " 数据集=" << label.datasetId
                    << " 来源=" << GetRevisionText(label.sourceRevision)
                    << " 版本=" << GetRevisionText(label.dataRevision) << " 采样值=";
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
                std::cout << "读取状态=" << static_cast<int>(chunk.error) << '\n';
            }
            (void)SetDemoStatus("标签图：" + std::to_string(labels.size()) + " | 详情见控制台（F3）");
            return isPassed;
        }

        bool PrintScenes()
        {
            const auto scenes = m_session.GetSceneViewStates();
            for (const auto& scene : scenes) {
                std::cout << "[场景] " << scene.id << " 已提交帧版本=" << scene.sceneEpoch
                    << " 已渲染帧版本=" << scene.renderedEpoch << " 功能=";
                for (const auto& id : scene.activeFeatureIds) std::cout << id << ' ';
                if (scene.presentation) std::cout << " 渲染质量=" << static_cast<int>(scene.presentation->volumeQuality)
                    << " 正在交互=" << scene.presentation->isInteracting
                    << " 输入=" << GetRevisionText(scene.presentation->dataRevision);
                std::cout << '\n';
            }
            (void)SetDemoStatus("场景/帧快照：" + std::to_string(scenes.size()) + " 个视图 | 详情见控制台（F4）");
            return !scenes.empty();
        }

#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        static std::string_view GetMethodName(SurfaceDeterminationMethod method)
        {
            switch (method) {
            case SurfaceDeterminationMethod::GlobalIsoPreview: return "全局等值面预览";
            case SurfaceDeterminationMethod::LocalAdaptiveIso50: return "局部自适应 ISO50";
            case SurfaceDeterminationMethod::GradientPeak: return "梯度峰值";
            case SurfaceDeterminationMethod::AutomaticIso50: return "自动 ISO50";
            }
            return "未知";
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
                        ? (result.status == SurfaceResultStatus::Cancelled ? "已取消" : "失败")
                        : action == ControlAction::SurfaceClear ? "已清除"
                        : "已就绪";
                    text << "表面确定：" << outcome;
                    if (action == ControlAction::SurfaceStart && result.status == SurfaceResultStatus::Succeeded
                        && result.isoEstimate) {
                        const auto image = owner->m_session.GetImageDescriptor();
                        HostViewSetRequest request;
                        request.targetView = owner->m_isoTarget;
                        request.iso = result.isoEstimate->isoValue;
                        const bool isApplied = image && image->dataRevision == result.sourceRevision
                            && owner->m_session.SendRequest(std::move(request));
                        text << " | 等值面阈值=" << result.isoEstimate->isoValue
                            << (isApplied ? " 已应用" : " 应用被拒绝")
                            << " | 采样数=" << result.isoEstimate->sampleCount;
                    }
                    (void)owner->SetDemoStatus(text.str());
                    std::cout << "[表面确定] " << text.str() << " | 原因=" << static_cast<int>(result.failureReason)
                        << " | " << result.message << '\n' << std::flush;
                }
            });
            if (admission.status != SurfaceAdmissionStatus::Accepted) {
                (void)SetDemoStatus("表面确定请求被拒绝：" + std::to_string(static_cast<int>(admission.status)));
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
            status << "表面确定：" << GetMethodName(m_surfaceRunningMethod) << " | " << progress << "%";
            const auto generation = feature->GetSurfaceSnapshot();
            if (generation && generation->isoEstimate)
                status << " | 等值面阈值=" << generation->isoEstimate->isoValue;
            else if (m_surfaceRunningMethod != SurfaceDeterminationMethod::AutomaticIso50)
                status << " | 点数=" << state.pointCount << " | 对象数=" << state.objectCount;
            if (!state.errorMessage.empty()) status << " | " << state.errorMessage;
            (void)SetDemoStatus(status.str());
        }
#endif

#if defined(MVVCVTK_HAS_MODEL_ROTATION)
        bool SetRotationMode(bool isEnabled)
        {
            const auto rotation = m_rotationFeature.lock();
            if (!rotation) return false;
            const auto crop = m_cropFeature.lock();
            if (isEnabled && crop && crop->GetState().isActive) {
                CropHostRequest exit;
                exit.action = CropHostAction::Exit;
                if (!crop->SendRequest(std::move(exit))) return false;
            }
            ModelRotationRequest request;
            request.action = ModelRotationAction::SetEnabled;
            request.isEnabled = false;
            if (!rotation->SendRequest(request)) return false;
            for (const auto& view : m_session.GetRenderViewStates()) {
                HostToolSetRequest tool;
                tool.targetView.viewId = view.id;
                tool.toolMode = isEnabled ? HostToolMode::ModelTransform : HostToolMode::Navigation;
                if (!m_session.SendRequest(std::move(tool))) return false;
            }
            request.isEnabled = isEnabled;
            return rotation->SendRequest(request)
                && SetDemoStatus(isEnabled ? "模型旋转：拖动旋转 / Escape 取消 / J 退出" : "模型旋转：空闲");
        }
#endif

        bool SendControl(const ControlAction action)
        {
            ++m_controlRevision;
            switch (action) {
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
            case ControlAction::RotationTool:
            case ControlAction::RotationPlus:
            case ControlAction::RotationMinus:
            case ControlAction::RotationUndo: {
                const auto rotation = m_rotationFeature.lock();
                if (!rotation) return false;
                if (action == ControlAction::RotationTool) {
                    if (m_isRotationQueued || !m_host) return false;
                    const bool isEnabled = !rotation->GetState().isEnabled;
                    const auto weak = weak_from_this();
                    m_isRotationQueued = true;
                    // Router 正在派发按键时不能替换自身 style；只排一个 owner 控制边界。
                    if (m_host->SendOwnerComplete([weak,isEnabled] {
                        if (const auto owner = weak.lock()) {
                            owner->m_isRotationQueued = false;
                            if (!owner->SetRotationMode(isEnabled))
                                (void)owner->SetDemoStatus("模型旋转：工具切换被拒绝");
                        }
                    })) return true;
                    m_isRotationQueued = false;
                    return false;
                }
                // 应用负责工具组合：任何旋转入口先结束 Crop 的 widget 编辑，历史仍保留。
                const auto crop = m_cropFeature.lock();
                if (crop && crop->GetState().isActive) {
                    CropHostRequest exit;
                    exit.action = CropHostAction::Exit;
                    if (!crop->SendRequest(std::move(exit))) return false;
                }
                ModelRotationRequest request;
                request.action = action == ControlAction::RotationUndo
                    ? ModelRotationAction::Undo : ModelRotationAction::Rotate;
                if (request.action == ModelRotationAction::Rotate)
                    request.angleDeg = action == ControlAction::RotationPlus ? 15 : -15;
                return rotation->SendRequest(request);
            }
#endif
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
                (void)SetDemoStatus("完整操作帮助已输出到启动终端；包含操作顺序、候选确认、裁切与内存说明");
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
                (void)SetDemoStatus("当前构建未启用表面确定功能");
                return false;
#endif
            case ControlAction::StartGap:
                return StartGap();
            case ControlAction::CropBuildAlias:
            case ControlAction::BuildCropResult:
                return SendCrop(CropHostAction::BuildResult);
            case ControlAction::SetCropPrimary:
                return SetCropData(true);
            case ControlAction::CropRestoreAlias:
            case ControlAction::RestoreCropSource:
                return SetCropData(false);
            case ControlAction::CropBox: return SendCrop(CropHostAction::Box);
            case ControlAction::CropPlane: return SendCrop(CropHostAction::Plane);
            case ControlAction::CropCylinder: return SendCrop(CropHostAction::Cylinder);
            case ControlAction::CropSphere: return SendCrop(CropHostAction::Sphere);
            case ControlAction::CropNoMode: return SendCrop(CropHostAction::Mode, CropRemovalMode::None);
            case ControlAction::CropKeepMode: return SendCrop(CropHostAction::Mode, CropRemovalMode::KeepInside);
            case ControlAction::CropRemoveMode: return SendCrop(CropHostAction::Mode, CropRemovalMode::RemoveInside);
            case ControlAction::CropPagePrevious: return ChangeCropPage(false);
            case ControlAction::CropPageNext: return ChangeCropPage(true);
            case ControlAction::CropExit: return SendCrop(CropHostAction::Exit);
            case ControlAction::CropCreate: return SendCropDocument(CropDocumentAction::CreateDocument);
            case ControlAction::CropDocumentNext: return SendCropDocument(CropDocumentAction::ActivateDocument);
            case ControlAction::CropClose: return SendCropDocument(CropDocumentAction::CloseDocument);
            case ControlAction::CropPruneSubtree: return PruneCrop(CropPruneScope::Subtrees);
            case ControlAction::CropPruneDescendants: return PruneCrop(CropPruneScope::Descendants);
            case ControlAction::CropPruneOutside: return PruneCrop(CropPruneScope::OutsidePaths);
            case ControlAction::CropNode0: case ControlAction::CropNode1:
            case ControlAction::CropNode2: case ControlAction::CropNode3:
            case ControlAction::CropNode4: case ControlAction::CropNode5:
            case ControlAction::CropNode6: case ControlAction::CropNode7:
            case ControlAction::CropNode8: case ControlAction::CropNode9:
                return SelectCropRow(
                    static_cast<std::size_t>(action) - static_cast<std::size_t>(ControlAction::CropNode0));
            default:
                return false;
            }
        }

        InteractionResult OnInput(
            const InteractionEvent& event)
        {
            if (event.eventKind == InteractionEventKind::Cancel) {
                m_isKeyDown.fill(false);
                m_appKeyDown.fill(false);
                return {};
            }
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
            // 应用层在接纳 Crop 命令前结束旋转手势。
            if (event.eventKind == InteractionEventKind::KeyPress
                && (GetKeyMatched(event,'o') || GetKeyMatched(event,'p'))) {
                const auto rotation = m_rotationFeature.lock();
                if (rotation && rotation->GetState().isEnabled) {
                    ModelRotationRequest exit;
                    exit.action = ModelRotationAction::SetEnabled;
                    exit.isEnabled = false;
                    if (!rotation->SendRequest(exit))
                        return {true,true,false,InteractionFailureReason::CleanupRejected};
                }
            }
#endif
            if (event.eventKind == InteractionEventKind::KeyRelease) {
                bool wasDown = false;
                const std::array<bool, 4> appMatches{
                    GetKeyMatched(event, m_appHotkeys.modelSwitchKey),
                    GetKeyMatched(event, m_appHotkeys.dataExportKey),
                    GetKeyMatched(event, m_appHotkeys.sliceExportKey),
                    event.keySym == m_appHotkeys.exitKeySym };
                for (std::size_t index = 0; index < appMatches.size(); ++index) {
                    if (appMatches[index]) {
                        wasDown = wasDown || m_appKeyDown[index];
                        m_appKeyDown[index] = false;
                    }
                }
                for (std::size_t index = 0; index < m_keys.size(); ++index) {
                    if (GetKeyMatched(event, m_keys[index].keyCode)
                        || (!m_keys[index].keySym.empty() && event.keySym == m_keys[index].keySym)) {
                        wasDown = wasDown || m_isKeyDown[index];
                        m_isKeyDown[index] = false;
                    }
                }
                return wasDown ? InteractionResult{true, true} : InteractionResult{};
            }
            int appAction = -1;
            if (m_appHotkeys.isContextInputEnabled
                && GetKeyMatched(event, m_appHotkeys.modelSwitchKey)) appAction = 0;
            if (m_appHotkeys.isCommandInputEnabled) {
                if (GetKeyMatched(event, m_appHotkeys.dataExportKey)) appAction = 1;
                if (GetKeyMatched(event, m_appHotkeys.sliceExportKey)) appAction = 2;
                if (event.keySym == m_appHotkeys.exitKeySym) appAction = 3;
            }
            if (appAction == 3) {
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
                const auto rotation = m_rotationFeature.lock();
                if (rotation && rotation->GetState().status == ModelRotationStatus::Dragging)
                    return {}; // The rotation input binding owns Escape during its gesture.
#endif
                const auto crop = m_cropFeature.lock();
                if (crop && crop->GetState().isActive) appAction = -1;
            }
            if (appAction >= 0 && !event.isCtrlDown
                && !event.isAltDown && !event.isShiftDown) {
                if (event.eventKind == InteractionEventKind::TextInput)
                    return {true, true};
                if (event.eventKind == InteractionEventKind::KeyPress) {
                    if (m_appKeyDown[appAction]) return {true, true};
                    m_appKeyDown[appAction] = true;
                    bool isSent = false;
                    if (appAction == 0) {
                        HostToolSwitchRequest request;
                        request.targetView.viewId = event.viewId;
                        isSent = m_session.SendRequest(std::move(request));
                    }
                    else if (appAction == 1) {
                        HostDataExportRequest request;
                        request.outputPath = m_appHotkeys.dataExportPath;
                        request.format = m_appHotkeys.dataExportFormat;
                        request.sourceView = m_appHotkeys.dataSourceView;
                        isSent = m_session.SendRequest(std::move(request));
                    }
                    else if (appAction == 2) {
                        HostSliceExportRequest request;
                        request.outputDir = m_appHotkeys.sliceExportDir;
                        request.sourceView.viewId = event.viewId;
                        request.angleDeg = m_appHotkeys.sliceAngleDeg;
                        isSent = m_session.SendRequest(std::move(request));
                    }
                    else {
                        HostToolSetRequest request;
                        request.targetView.viewId = event.viewId;
                        request.toolMode = HostToolMode::Navigation;
                        isSent = m_session.SendRequest(std::move(request));
                    }
                    return {true, true, isSent, isSent
                        ? InteractionFailureReason::None
                        : InteractionFailureReason::StateRejected};
                }
            }
            const auto action = GetAction(event);
            if (!action) return {};
            if (*action == ControlAction::CropExit) {
                const auto crop = m_cropFeature.lock();
                if (!crop || !crop->GetState().isActive) return {};
            }
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
        std::string m_cropStatus;
        CropDocumentId m_cropUiDocument=0;
        CropNodeId m_cropHighlighted=0,m_cropPageNext=0,m_cropLastApplied=0;
        std::uint64_t m_cropUiRevision=0;
        std::size_t m_cropPageIndex=0;
        std::vector<CropNodeId> m_cropPageStarts;
        std::vector<CropNodeSnapshot> m_cropPageNodes;
        bool m_cropRequestPending=false;
        std::uint64_t m_controlRevision = 0;
        std::weak_ptr<GapHostFeature> m_gapFeature;
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
        std::weak_ptr<ModelRotationHostFeature> m_rotationFeature;
        bool m_isRotationQueued = false;
#endif
        GapHostStartParams m_gapStart;
        HostHotkeyConfig m_appHotkeys;
        std::array<bool, 4> m_appKeyDown{};
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
                (void)SetPartStatus("零件分割：" + std::to_string(progress) + "% | Alt+B 取消");
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
            if (action == ControlAction::Start) return "零件分割：运行中";
            if (action == ControlAction::Toggle) {
                return "零件分割：正在更新显示状态";
            }
            return action == ControlAction::Stop ? "零件分割：正在取消" : "零件分割：正在清除";
        }

        bool SendPartRequest(
            PartSegmentationRequest request,
            const ControlAction action,
            const std::optional<bool> nextVisibility = std::nullopt)
        {
            const auto partFeature = m_partFeature.lock();
            if (!partFeature) {
                (void)SetPartStatus("零件分割：功能不可用");
                std::cerr
                    << "[零件分割] 请求被拒绝：功能不可用\n"
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
                        status << "零件分割：";
                        if (result.status == PartResultStatus::Succeeded) {
                            if (action == ControlAction::Start) {
                                const auto state = completedFeature
                                    ? completedFeature->GetState()
                                    : PartSegmentationState{};
                                status << "成功 | 零件数=" << result.partCount
                                    << " | 结果版本="
                                    << result.resultSet.generation
                                    << " | 提交编号=" << result.commitId
                                    << " | 可见=" << state.isOverlayVisible;
                            }
                            else if (action == ControlAction::Toggle) {
                                status << (nextVisibility.value_or(false)
                                    ? "已显示" : "已隐藏");
                            }
                            else {
                                status << (action == ControlAction::Stop ? "已请求取消" : "已清除");
                            }
                        }
                        else if (result.status == PartResultStatus::SucceededWithDisplayFailure) {
                            status << "数据已就绪；显示失败 | 零件数=" << result.partCount;
                        }
                        else {
                            status
                                << (result.status == PartResultStatus::Cancelled
                                    ? "已取消" : "失败")
                                << " | 原因="
                                << static_cast<int>(result.failureReason);
                        }
                        if (const auto owner = controlOwner.lock()) {
                            (void)owner->SetPartStatus(status.str());
                        }
                        std::cerr
                            << "[零件分割] " << status.str()
                            << " | 源数据版本="
                            << result.sourceRevision.generation
                            << " | 标签版本="
                            << result.labelMap.generation
                            << " | 零件表版本="
                            << result.partTable.generation
                            << " | 结果版本="
                            << result.resultSet.generation
                            << " | 提交编号=" << result.commitId
                            << " | 零件数=" << result.partCount
                            << " | 消息=" << result.message << '\n'
                            << std::flush;
                });
            const bool isAccepted =
                admission.status == PartAdmissionStatus::Accepted;
            if (!isAccepted) {
                std::ostringstream status;
                status << "零件分割：请求被拒绝 | 接纳状态="
                    << static_cast<int>(admission.status);
                (void)SetPartStatus(status.str());
            }
            std::cerr
                << "[零件分割] B 请求"
                << (isAccepted ? "已接受" : "被拒绝")
                << " | 操作=" << static_cast<int>(action)
                << " | 接纳状态=" << static_cast<int>(admission.status)
                << '\n' << std::flush;
            return isAccepted;
        }

        bool SetPartSelection(ControlAction action)
        {
            const auto feature = m_partFeature.lock();
            const auto snapshot = feature ? feature->GetPartSetSnapshot() : nullptr;
            if (!snapshot || snapshot->isStale || snapshot->parts.empty()) {
                (void)SetPartStatus("零件分割：请先按 B 生成当前结果");
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
                (void)SetPartStatus("零件编辑被拒绝：" + std::to_string(static_cast<int>(result.status)));
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
            status << "零件 " << updated->labelId << " | 已选中=" << updated->presentation.isSelected
                << " 可见=" << updated->presentation.isVisible << " 已审核=" << updated->userState.isReviewed
                << " 体积=" << updated->metrics.physicalVolumeMM3;
            (void)SetPartStatus(status.str());
            std::cout << "[零件] " << status.str() << " | 稳定标识="
                << updated->binding.object.objectId.high << ':' << updated->binding.object.objectId.low
                << " | 目录版本=" << current->catalogRevision << '\n';
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
                        (void)SetPartStatus("零件分割：可用内存不足，无法生成另一份全分辨率结果");
                        std::cerr << "[零件分割] 内存准入被拒绝：新标签所需字节数=" << newLabelBytes
                            << " 可用字节数=" << memory.ullAvailPhys << '\n';
                        return false;
                    }
                }
#endif
                m_progress = -1;
                request.start = start;
                std::cout << "[零件分割] 使用当前等值面阈值=" << start.threshold << '\n' << std::flush;
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
                    std::cerr << "[演示验证] " << failure << '\n';
                    Finish(false);
                    return;
                }
            }
            auto& step = m_steps[m_index];
            if (!m_isSent) {
                std::cout << "[演示验证] " << step.name << '\n' << std::flush;
                m_stepStarted = std::chrono::steady_clock::now();
                if (!SendKey(step.key)) { Finish(false); return; }
                m_isSent = true;
                m_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
                return;
            }
            if (!step.ready || step.ready()) {
                std::cout << "[演示验证] 已完成=" << step.name << " 耗时（毫秒）="
                    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_stepStarted).count() << '\n';
                if ((step.name == "选择零件" || step.name == "表面阈值")
                    && !SaveView(step.name == "选择零件" ? "parts" : "surface")) { Finish(false); return; }
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
        composite.title = "窗口 E：组合体渲染";
        composite.width = 600;
        composite.height = 600;
        composite.posX = 660;
        composite.posY = 50;
        composite.viewInit.viewMode =
            HostRenderMode::CompositeVolume;
        composite.viewInit.background = { 0.08, 0.08, 0.12 };
        composite.viewInit.hasBackground = true;

        HostWindowConfig topDown;
        topDown.title = "窗口 B：上下方向切片";
        topDown.width = 400;
        topDown.height = 400;
        topDown.posX = 50;
        topDown.posY = 660;
        topDown.viewInit.viewMode =
            HostRenderMode::SliceTopDown;
        topDown.viewInit.background = { 0.0, 0.0, 0.0 };
        topDown.viewInit.hasBackground = true;

        HostWindowConfig frontBack = topDown;
        frontBack.title = "窗口 C：前后方向切片";
        frontBack.posX = 460;
        frontBack.viewInit.viewMode =
            HostRenderMode::SliceFrontBack;

        HostWindowConfig leftRight = topDown;
        leftRight.title = "窗口 D：左右方向切片";
        leftRight.posX = 870;
        leftRight.viewInit.viewMode =
            HostRenderMode::SliceLeftRight;

        HostWindowConfig primary;
        primary.title = "窗口 A：组合等值面";
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
        config.keys.switchOverlay.keyCode = 'h';
        config.keys.exit.keySym = "Escape";
        return config;
    }


    bool StartDrivenSession(VtkAppHostSession& session,
        const std::shared_ptr<std::atomic<bool>>& hasWork,
        const bool isContinuous)
    {
        const auto* endpoint = session.GetRenderViewEndpoint("slice-top-down");
        if (!endpoint || !endpoint->interactor) return false;
        vtkSmartPointer<vtkRenderWindowInteractor> interactor = endpoint->interactor;
        std::vector<std::string> pendingViews;
        auto nextFrame = std::chrono::steady_clock::now();
        constexpr auto framePeriod = std::chrono::microseconds(16667);
        std::function<void()> onTick = [&] {
            if (hasWork->exchange(false) || isContinuous) {
                const auto update = session.SendUpdates();
                if (update.status == HostUpdateStatus::Completed)
                    pendingViews = update.renderViewIds;
                else if (update.status == HostUpdateStatus::Failed)
                    hasWork->store(true); // 示例侧按原生时钟重试，SDK 不自启 timer。
            }
            const auto now = std::chrono::steady_clock::now();
            if (pendingViews.empty() || now < nextFrame) return;
            nextFrame = now + framePeriod; // 以开始时刻限频，不叠加绘制耗时。
            HostRenderRequest request;
            request.viewIds = pendingViews;
            const auto states = session.GetRenderViewStates();
            const bool isInteracting = std::any_of(states.begin(), states.end(),
                [](const HostRenderViewState& state) { return state.isInteracting; });
            request.desiredUpdateRate = isInteracting ? 15.0 : 0.001;
            const auto rendered = session.SendRender(request);
            for (const auto& view : rendered.views) {
                if (view.status == HostRenderStatus::Rendered
                    || view.status == HostRenderStatus::Unchanged)
                    pendingViews.erase(std::remove(pendingViews.begin(),
                        pendingViews.end(), view.viewId), pendingViews.end());
            }
        };
        onTick();
        vtkNew<vtkCallbackCommand> callback;
        callback->SetClientData(&onTick);
        callback->SetCallback([](vtkObject*, unsigned long, void* data, void*) {
            try { (*static_cast<std::function<void()>*>(data))(); }
            catch (...) {}
        });
        const auto tag = interactor->AddObserver(vtkCommand::TimerEvent, callback);
        const int timerId = interactor->CreateRepeatingTimer(16);
        if (tag == 0 || timerId == 0) {
            if (tag != 0) interactor->RemoveObserver(tag);
            if (timerId != 0) (void)interactor->DestroyTimer(timerId);
            return false;
        }
        bool isStarted = true;
        try { interactor->Start(); }
        catch (...) { isStarted = false; }
        interactor->RemoveObserver(tag);
        return interactor->DestroyTimer(timerId) != 0 && isStarted;
    }

} // namespace

int main(int argc, char* argv[])
{
#if defined(_WIN32)
    // 源码与窄字符串均使用 UTF-8，控制台采用相同编码显示中文。
    (void)SetConsoleOutputCP(CP_UTF8);
#endif
    if (GetArgFound(argc, argv, "--help")) {
        PrintDemoHelp();
        return 0;
    }
    FeatureTestOptions toolOptions;
    try { toolOptions = GetFeatureTestOptions(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        PrintFeatureTestHelp();
        return 2;
    }
    if (toolOptions.budgetBytes == 0) {
        toolOptions.budgetBytes = std::size_t{1} << 30U;
#if defined(_WIN32)
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) {
            toolOptions.budgetBytes = static_cast<std::size_t>(std::min<ULONGLONG>(
                std::uint64_t{64} << 30U, memory.ullAvailPhys / 2U));
        }
#endif
    }
    std::cout << "[运行配置] 工具内存预算（MiB）="
        << toolOptions.budgetBytes / (1024U * 1024U)
        << " 编辑/伪影超时（毫秒）=" << toolOptions.timeoutMs << '\n' << std::flush;
    const bool isFeatureAudit = GetArgFound(argc, argv, "--feature-audit");

    const bool isHostDriven = GetArgFound(argc, argv, "--host-driven");
    const auto hasHostWork = std::make_shared<std::atomic<bool>>(true);
    const bool isDemo = GetArgFound(argc, argv, "--demo");
    const bool isDemoAudit = GetArgFound(argc, argv, "--demo-audit") || isFeatureAudit;
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
    if (isHostDriven) {
        sessionConfig.driveMode = HostDriveMode::HostDriven;
        sessionConfig.onWorkAvailable = [hasHostWork] { hasHostWork->store(true); };
    }
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
    auto cropFeature = std::make_shared<CropHostFeature>();
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
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
    ModelRotationConfig rotationConfig;
    rotationConfig.targetViews = allViews;
    auto rotationFeature = std::make_shared<ModelRotationHostFeature>(rotationConfig);
    features.push_back(rotationFeature);
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    auto partConfig = GetPartConfig();
    partConfig.maxHistoryBytes = toolOptions.budgetBytes;
    partConfig.maxWorkingBytes = toolOptions.budgetBytes;
    partConfig.editTimeoutMs = toolOptions.timeoutMs;
    partConfig.isSelectionEnabled = GetArgFound(argc, argv, "--part-picking");
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
    surfaceConfig.maxWorkingBytes = toolOptions.budgetBytes;
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
        std::move(gapStart),
        isHostDriven ? GetHotkeys(allViews) : HostHotkeyConfig{});
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    controlFeature->SetSurfaceFeature(surfaceFeature);
#endif
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
    features.insert(features.begin(), controlFeature);
    controlFeature->SetRotationFeature(rotationFeature);
#else
    features.push_back(controlFeature);
#endif
    FeatureTestBindings toolBindings;
    toolBindings.crop = cropFeature;
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    toolBindings.parts = partFeature;
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    toolBindings.surface = surfaceFeature;
#endif
#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT)
    AlignmentConfig alignmentConfig;
    alignmentConfig.targetViews.viewIds = {primaryTarget.viewId};
    auto alignmentFeature = std::make_shared<MetrologyAlignmentHostFeature>(alignmentConfig);
    toolBindings.alignment = alignmentFeature;
    features.push_back(alignmentFeature);
#endif
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    ArtifactConfig artifactConfig;
    artifactConfig.memoryBudgetBytes = toolOptions.budgetBytes;
    artifactConfig.publishBudgetBytes = toolOptions.budgetBytes;
    auto artifactFeature = std::make_shared<ArtifactReductionHostFeature>(artifactConfig);
    toolBindings.artifact = artifactFeature;
    features.push_back(artifactFeature);
#endif
    auto featureTools = std::make_shared<FeatureTestControls>(session, toolBindings, toolOptions, allViews);
    features.push_back(featureTools);
    std::string cropAuditFailure;
    auto demoAudit = std::make_shared<DemoAuditFeature>(session);
    demoAudit->SetFailureCheck([&]() -> std::string {
        if(!cropAuditFailure.empty())return cropAuditFailure;
        if (isFeatureAudit && !featureTools->GetFailure().empty()) return featureTools->GetFailure();
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        if (surfaceFeature->GetState().stage == SurfaceDeterminationStage::Failed)
            return surfaceFeature->GetState().errorMessage;
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        if (partFeature->GetState().status == PartSegmentationStatus::Failed)
            return "零件分割失败；请查看回调中的原因和消息。";
#endif
        return {};
    });
    if (isDemoAudit) {
        const auto ready = [] { return true; };
        demoAudit->AddStep("帮助", {0, "F1"}, ready);
        demoAudit->AddStep("图像与数据图", {0, "F2"}, [&session] { return session.GetImageDescriptor().has_value(); });
        demoAudit->AddStep("帧快照", {0, "F4"}, [&session] { return session.GetSceneViewStates().size() == 5; });
        demoAudit->AddStep("体渲染质量", {'l'}, [&session, volumeTarget] {
            const auto state = session.GetRenderViewState(volumeTarget);
            return state && state->volumeQuality == HostVolumeQuality::High;
        });
        demoAudit->AddStep("等值面质量", {'i'}, [&session, primaryTarget] {
            const auto state = session.GetRenderViewState(primaryTarget);
            return state && state->volumeQuality == HostVolumeQuality::High;
        });
        demoAudit->AddStep("适配视图", {0, "F5"}, [&session, primaryTarget] {
            const auto scene = session.GetSceneViewState(primaryTarget);
            return scene && scene->camera && scene->camera->parallelScale > 1.0;
        });
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        demoAudit->AddStep("开始零件分割", {'b'}, [partFeature] {
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
        demoAudit->AddStep("选择零件", {'n'}, [selectedPart] { return selectedPart().has_value(); });
        demoAudit->AddStep("循环选择上一个零件", {'n', {}, false, false, true}, [selectedPart] {
            const auto part = selectedPart(); return part && part->labelId == 2;
        });
        demoAudit->AddStep("选择上一个零件", {'n', {}, false, false, true}, [selectedPart] {
            const auto part = selectedPart(); return part && part->labelId == 1;
        });
        demoAudit->AddStep("审核零件", {'r', {}, true}, [selectedPart] {
            const auto part = selectedPart(); return part && part->userState.isReviewed;
        });
        demoAudit->AddStep("隐藏零件", {'h', {}, true}, [selectedPart] {
            const auto part = selectedPart(); return part && !part->presentation.isVisible;
        });
        demoAudit->AddStep("显示零件", {'h', {}, true}, [selectedPart] {
            const auto part = selectedPart(); return part && part->presentation.isVisible;
        });
        demoAudit->AddStep("隐藏全部零件", {'b', {}, false, false, true}, [partFeature] { return !partFeature->GetState().isOverlayVisible; });
        demoAudit->AddStep("显示全部零件", {'b', {}, false, false, true}, [partFeature] { return partFeature->GetState().isOverlayVisible; });
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        demoAudit->AddStep("表面阈值", {'u'}, [surfaceFeature, &session, primaryTarget] {
            const auto snapshot = surfaceFeature->GetSurfaceSnapshot();
            const auto view = session.GetRenderViewState(primaryTarget);
            return surfaceFeature->GetState().stage == SurfaceDeterminationStage::Ready
                && snapshot && snapshot->isoEstimate && view
                && view->isoThreshold == snapshot->isoEstimate->isoValue;
        });
        demoAudit->AddStep("清除表面估计", {'u', {}, true}, [surfaceFeature] { return !surfaceFeature->GetSurfaceSnapshot(); });
#endif
        demoAudit->AddStep("开始孔隙分析", {'g'}, [gapFeature] {
            const auto state = gapFeature->GetState();
            return state.analysisState == GapAnalysisState::Succeeded && GetDataRevisionRefValid(state.labelMap);
        });
        demoAudit->AddStep("标签图", {0, "F3"}, [&session] {
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
            return session.GetLabelMapDescriptors().size() == 2;
#else
            return session.GetLabelMapDescriptors().size() == 1;
#endif
        });
        demoAudit->AddStep("方框裁剪", {'o'}, [cropFeature] { return cropFeature->GetState().isActive; });
        demoAudit->AddStep("裁剪保留模式", {'1'}, [cropFeature] {
            return cropFeature->GetState().history.editMode == CropRemovalMode::KeepInside;
        });
        demoAudit->AddStep("平面裁剪", {'p'}, [cropFeature] { return cropFeature->GetState().isActive; });
        demoAudit->AddStep("裁剪移除模式", {'2'}, [cropFeature] {
            return cropFeature->GetState().history.editMode == CropRemovalMode::RemoveInside;
        });
        struct CropUiAudit {
            CropDocumentId document=0;CropNodeId root=0;std::vector<CropNodeId> nodes;
            CropRequestId pending=0;std::uint64_t revision=0;CropNodeId applied=0;int polls=0;
            CropVectorDouble3Array center{};
        };
        const auto cropUi=std::make_shared<CropUiAudit>();
        demoAudit->AddStep("准备裁切分支", {0,"F2"}, [cropFeature,cropUi,&cropAuditFailure] {
            const auto history=cropFeature->GetHistory(0,0,1);
            if(++cropUi->polls==1||cropUi->polls==100)std::cout<<"[CropAudit] setup doc="<<history.documentId<<" revision="<<history.stateRevision
                <<" pending="<<cropFeature->GetState().history.pendingRequestCount<<" request="<<cropUi->pending<<" nodes="<<cropUi->nodes.size()<<std::endl;
            if(!cropUi->document) {
                const auto archive=cropFeature->GetArchive(history.documentId);if(!archive||!archive->imageGeometry)return false;
                cropUi->document=history.documentId;cropUi->root=history.rootNodeId;const auto& g=*archive->imageGeometry;cropUi->center=g.origin;
                for(int row=0;row<3;++row)for(int column=0;column<3;++column)
                    cropUi->center[row]+=g.direction[row*3+column]*g.spacing[column]*(0.5*g.extent[2*column]+0.5*g.extent[2*column+1]);
            }
            if(cropUi->pending) {
                const auto out=cropFeature->GetOutcome(cropUi->document,cropUi->pending);
                if(!out||out->status==CropEditStatus::Queued)return false;
                if(out->status!=CropEditStatus::Succeeded){cropAuditFailure="裁切分支准备失败："+std::to_string(static_cast<int>(out->failureReason));return false;}
                cropUi->nodes.push_back(out->nodeId);cropUi->pending=0;
            }
            if(cropUi->nodes.size()==12){cropUi->revision=history.stateRevision;cropUi->applied=history.appliedHead;return true;}
            CropEditRequest append;append.documentId=cropUi->document;append.requestId=CropHostFeature::CreateRequestId();
            append.expectedRevision=history.stateRevision;append.kind=CropEditKind::Append;append.nodeId=cropUi->root;
            append.operation.geometryType=CropShape::Plane;append.operation.planeNormalInInputModel={1,0,0};
            append.operation.planeCenterInInputModel=cropUi->center;append.operation.planeCenterInInputModel[0]+=cropUi->nodes.size()*0.01;
            const auto admitted=cropFeature->SendRequest(append);
            if(admitted)cropUi->pending=append.requestId;
            else cropAuditFailure="裁切分支接纳失败："+std::to_string(static_cast<int>(admitted.failureReason));return false;
        });
        demoAudit->AddStep("Root 仅高亮", {'0',{},false,true}, [cropFeature,cropUi] {
            const auto history=cropFeature->GetHistory(0,0,1);
            return history.documentId==cropUi->document&&history.stateRevision==cropUi->revision&&history.appliedHead==cropUi->applied;
        });
        demoAudit->AddStep("历史下一页", {'5'}, ready);
        demoAudit->AddStep("选择另一分支", {'1',{},false,true}, [cropFeature,cropUi] {
            return cropFeature->GetHistory(0,0,1).appliedHead==cropUi->nodes[9];
        });
        demoAudit->AddStep("物化明确节点", {'7',{},true}, [cropFeature,cropUi] {
            const auto history=cropFeature->GetHistory(0,0,1);
            if(history.results.size()!=1||history.results.front().status!=CropResultStatus::Published||history.results.front().nodeId!=cropUi->nodes[9])return false;
            CropPruneRequest prune;prune.scope=CropPruneScope::Descendants;prune.nodeIds={cropUi->root};
            if(cropFeature->GetPruneImpact(cropUi->document,prune).blockers.empty())return false;
            cropUi->revision=history.stateRevision;cropUi->applied=history.appliedHead;return true;
        });
        demoAudit->AddStep("结果存在时 Root 仅高亮", {'0',{},false,true}, [cropFeature,cropUi] {
            const auto history=cropFeature->GetHistory(0,0,1);return history.stateRevision==cropUi->revision
                &&history.appliedHead==cropUi->applied&&history.results.size()==1;
        });
        demoAudit->AddStep("返回并释放裁切结果", {'9',{},true}, [cropFeature,cropUi] {
            const auto history=cropFeature->GetHistory(0,0,1);return history.results.empty()&&history.appliedHead==cropUi->root
                &&cropFeature->GetState().documentStatus==CropDocumentStatus::Ready;
        });
        demoAudit->AddStep("修剪 Root 后代", {0,"Delete",false,true}, [cropFeature] {return cropFeature->GetHistory(0,0,1).totalNodeCount==1;});
        demoAudit->AddStep("新建独立裁切文档", {'o',{},true}, [cropFeature,cropUi] {
            return cropFeature->GetDocuments().size()==2&&cropFeature->GetHistory(0,0,1).documentId!=cropUi->document;
        });
        demoAudit->AddStep("切换回原裁切文档", {0,"Tab",true}, [cropFeature,cropUi] {return cropFeature->GetHistory(0,0,1).documentId==cropUi->document;});
        demoAudit->AddStep("关闭原裁切文档", {0,"Delete",true,false,true}, [cropFeature] {return cropFeature->GetDocuments().size()==1&&!cropFeature->GetHistory().documentId;});
        demoAudit->AddStep("激活唯一剩余文档", {0,"Tab",true}, [cropFeature,cropUi] {
            const auto history=cropFeature->GetHistory(0,0,1);return history.documentId&&history.documentId!=cropUi->document;
        });
#if defined(MVVCVTK_HAS_MODEL_ROTATION)
        demoAudit->AddStep("数值旋转", {'j', {}, false, false, true}, [rotationFeature, cropFeature] {
            return rotationFeature->GetState().status == ModelRotationStatus::Succeeded
                && rotationFeature->GetState().undoCount == 1 && !cropFeature->GetState().isActive;
        });
        demoAudit->AddStep("撤销旋转", {'j', {}, false, true}, [rotationFeature] {
            return rotationFeature->GetState().status == ModelRotationStatus::Succeeded
                && rotationFeature->GetState().undoCount == 0;
        });
        demoAudit->AddStep("旋转工具", {'j'}, [rotationFeature] {
            return rotationFeature->GetState().isEnabled;
        });
        demoAudit->AddStep("从旋转切换到裁剪", {'o'}, [rotationFeature, cropFeature] {
            return !rotationFeature->GetState().isEnabled && cropFeature->GetState().isActive;
        });
#endif
        if (isFeatureAudit) {
            for (auto& step : featureTools->GetAuditSteps())
                demoAudit->AddStep(std::move(step.name), std::move(step.key), std::move(step.ready));
        }
        demoAudit->AddStep("最终数据图", {0, "F2"}, ready);
        features.push_back(demoAudit);
    }

    if (isRealAudit) {
        demoAudit->AddStep("真实图像", {0, "F2"}, [&session] { return session.GetImageDescriptor().has_value(); });
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        demoAudit->AddStep("表面阈值", {'u'}, [surfaceFeature, &session, primaryTarget] {
            const auto snapshot = surfaceFeature->GetSurfaceSnapshot();
            const auto view = session.GetRenderViewState(primaryTarget);
            return surfaceFeature->GetState().stage == SurfaceDeterminationStage::Ready
                && snapshot && snapshot->isoEstimate && view
                && view->isoThreshold == snapshot->isoEstimate->isoValue;
        });
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        demoAudit->AddStep("开始零件分割", {'b'}, [partFeature] {
            return partFeature->GetState().status == PartSegmentationStatus::Succeeded
                && partFeature->GetState().partCount > 0;
        });
        demoAudit->AddStep("选择零件", {'n'}, [partFeature] {
            const auto snapshot = partFeature->GetPartSetSnapshot();
            return snapshot && std::any_of(snapshot->parts.begin(), snapshot->parts.end(),
                [](const PartSnapshot& part) { return part.presentation.isSelected; });
        });
        demoAudit->AddStep("真实标签图", {0, "F3"}, [&session] { return !session.GetLabelMapDescriptors().empty(); });
        demoAudit->AddStep("重新开始零件分割", {'b'}, [partFeature] {
            return partFeature->GetState().status == PartSegmentationStatus::Running;
        });
        demoAudit->AddStep("取消零件分割并保留结果", {'b', {}, false, true}, [partFeature] {
            const auto state = partFeature->GetState();
            return state.status == PartSegmentationStatus::Succeeded && state.resultRevision == 1
                && partFeature->GetPartSetSnapshot() && state.partCount > 0;
        });
#endif
        demoAudit->AddStep("真实数据帧状态", {0, "F4"}, [] { return true; });
        features.push_back(demoAudit);
    }


    std::size_t attachedCount = 0;
    bool isTimerAttached = false;
    bool isHotkeyAttached = false;

    const auto clearAttached = [&]() {
        // Stop owns the dependency order and keeps its render drain alive until
        // every Feature and result resource has actually finished releasing.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(toolOptions.timeoutMs);
        do {
            if(session.Stop()){attachedCount=0;isTimerAttached=false;isHotkeyAttached=false;return true;}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }while(std::chrono::steady_clock::now()<deadline);
        std::cerr<<"[退出] 资源释放仍未完成，Session 保持 StopPending。\n";return false;
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
    if (!isHostDriven && !session.AttachTimer(timer)) {
        if (!clearAttached()) {
            return 21;
        }
        return 3;
    }
    isTimerAttached = !isHostDriven;

    if (!isHostDriven && !session.AttachHotkeys(GetHotkeys(allViews))) {
        if (!clearAttached()) {
            return 22;
        }
        return 4;
    }
    isHotkeyAttached = !isHostDriven;

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
            << "--drag-audit、--quality-audit、--gap-auto 与"
            "零件分割/演示运行模式"
            "互斥，只能选择一种\n";
        if (!clearAttached()) return 25;
        features.clear();
        return 8;
    }
#if !defined(MVVCVTK_HAS_PART_SEGMENTATION)
    if (isPartAuto || isPartManual || isPartProfile) {
        std::cerr
            << "零件分割运行模式要求启用 "
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
    bool isLoadFailed = false;

    HostResultCallback onDataReady =
        [&](HostResult result) {
        isLoadFailed = !result.isSucceeded;
        if (result.isSucceeded) controlFeature->StartDemoFit();
        if (isDemo || isDemoAudit || isRealAudit) {
            if (!result.isSucceeded) {
                std::cerr << "[演示] 数据加载失败：" << result.message << '\n';
                (void)StopEventLoop(session);
                return;
            }
            std::cout << "[演示] 数据已就绪。按 B / G / U 体验功能；按 F1 查看全部快捷键。\n" << std::flush;
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
                    << "[孔隙分析] 数据加载失败；"
                    "已跳过自动请求\n"
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
                    << "QT_PART_RESULT: 数据重新加载失败；"
                    "已跳过请求\n"
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
                    << "QT_PART_RESULT: 真实数据加载失败；"
                    "已跳过请求\n"
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
                    << "[零件分割] 合成数据重新加载失败\n"
                    << std::flush;
                (void)StopEventLoop(session);
                return;
            }
            std::cout
                << "[零件分割] 合成数据已就绪；"
                "按 B 开始\n"
                << std::flush;
            return;
        }
#endif
        else {
            if (!result.isSucceeded) {
                std::cerr << "[数据加载] 失败：" << result.message << '\n' << std::flush;
                (void)StopEventLoop(session);
            }
            else {
                std::cout << "[数据加载] 真实数据已就绪；按 U 自动估计表面阈值。\n" << std::flush;
            }
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
        load.filePath = toolOptions.inputPath;
        load.geometry.dimensions = toolOptions.dimensions;
        load.geometry.spacing = {
            0.1537f, 0.1537f, 0.1537f };
        load.geometry.origin = { 0.0f, 0.0f, 0.0f };
        load.metadata.identity.datasetId = "standalone-ct-"
            + std::to_string(toolOptions.dimensions[0]) + "x"
            + std::to_string(toolOptions.dimensions[1]) + "x"
            + std::to_string(toolOptions.dimensions[2]);
        load.metadata.source.kind = ImageSourceKind::RawFile;
        load.metadata.source.uri = load.filePath;
        std::cout << "[运行配置] 真实输入=" << load.filePath
            << " 尺寸=" << toolOptions.dimensions[0] << 'x'
            << toolOptions.dimensions[1] << 'x' << toolOptions.dimensions[2]
            << '\n' << std::flush;
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

    const bool isStarted = isHostDriven
        ? StartDrivenSession(session, hasHostWork,
            isDemoAudit || isRealAudit || isQualityAudit)
        : session.Start();
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
    if (isLoadFailed) return 5;
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
