// 测试用途：通过孔隙页面测试分析请求、统计结果、叠加显示与退出收口。
#include "ModuleFactories.h"
#include "Host/GapHostFeature.h"
#include <QPointer>
namespace Manual {
namespace {
QJsonObject GetStatistics(const GapStatistics& value)
{
    return {{"objectVoxels", QString::number(value.objectVoxelCount)}, {"voidVoxels", QString::number(value.voidVoxelCount)},
        {"objectVolumeMM3", value.objectVolumeMM3}, {"voidVolumeMM3", value.voidVolumeMM3}, {"porosityRatio", value.porosityRatio}};
}
}
ModulePanel* CreateGapTest(TestContext context, std::shared_ptr<GapHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Gap", parent);
    panel->SetNotice("切换结果叠加显示可隐藏或显示分析结果；退出分析需等待当前处理结束。结果区显示孔隙体积、孔隙率等统计与数据修订。");
    panel->AttachAction("Start", GetJson(R"({"isoMode":"AbsoluteValue","dataRangeRatio":0.5,"absoluteIsoValue":0.5,"backgroundMean":0,"materialMean":1,"filter":false,"minVolumeMM3":0})"),
        [panel, feature](auto id, const auto& params) {
            GapHostStartParams start;
            start.targetViews = GetAllViews();
            start.surface.isoMode = GetEnum<GapIsoMode>(params, "isoMode", {{"AbsoluteValue", GapIsoMode::AbsoluteValue}, {"DataRangeRatio", GapIsoMode::DataRangeRatio}});
            start.surface.absoluteIsoValue = GetNumber(params, "absoluteIsoValue");
            start.surface.dataRangeRatio = GetNumber(params, "dataRangeRatio");
            start.surface.backgroundMean = static_cast<float>(GetNumber(params, "backgroundMean"));
            start.surface.materialMean = static_cast<float>(GetNumber(params, "materialMean"));
            start.voidParams.isFilterEnabled = GetBool(params, "filter");
            start.voidParams.minVolumeMM3 = GetNumber(params, "minVolumeMM3");
            GapHostRequest request; request.action = GapHostAction::Start; request.start = start;
            const QPointer<ModulePanel> owner(panel);
            const auto accepted = feature->SendRequest(std::move(request), [owner, id](GapHostResult result) {
                if (!owner) return;
                auto summary = GetStatistics(result.statistics);
                summary["status"] = static_cast<int>(result.status);
                summary["message"] = QString::fromStdString(result.message);
                summary["source"] = GetRefText(result.sourceRevision);
                summary["voidTable"] = GetRefText(result.voidTable);
                summary["resultSet"] = GetRefText(result.resultSet);
                summary["commitId"] = QString::number(result.commitId);
                const auto status = result.status == GapResultStatus::Succeeded ? "Succeeded"
                    : result.status == GapResultStatus::SucceededWithDisplayFailure ? "SucceededWithDisplayFailure"
                    : result.status == GapResultStatus::SourceChanged ? "SourceChanged" : "Failed";
                owner->SetComplete(id, status, summary);
            });
            panel->SetAdmission(id, accepted);
        }, TestPolicy::Compute, true);
    auto pendingExit = std::make_shared<std::uint64_t>(0);
    panel->AttachAction("SetDisplay", GetJson(R"({"palette":"Rainbow","rangeMode":"Result","range":[0,1],
        "constantColor":null,"lowColor":null,"highColor":null,"belowColor":null,"aboveColor":null})"),
        [panel, feature](auto id, const auto& p) {
            GapDisplayParams display;
            display.mode = GetEnum<GapColorMode>(p, "palette", {{"Constant", GapColorMode::Constant},
                {"Gradient", GapColorMode::Gradient}, {"Rainbow", GapColorMode::Rainbow},
                {"InverseRainbow", GapColorMode::InverseRainbow}, {"HueLoop", GapColorMode::HueLoop}});
            display.rangeMode = GetEnum<GapRangeMode>(p, "rangeMode", {
                {"Result", GapRangeMode::Result}, {"SelectedInterval", GapRangeMode::SelectedInterval}});
            if (p.contains("range")) display.range = GetArray<double, 2>(p["range"]);
            if (!p["constantColor"].isNull()) display.constantColor = GetArray<double, 3>(p["constantColor"]);
            if (!p["lowColor"].isNull()) display.lowColor = GetArray<double, 3>(p["lowColor"]);
            if (!p["highColor"].isNull()) display.highColor = GetArray<double, 3>(p["highColor"]);
            if (!p["belowColor"].isNull()) display.belowColor = GetArray<double, 3>(p["belowColor"]);
            if (!p["aboveColor"].isNull()) display.aboveColor = GetArray<double, 3>(p["aboveColor"]);
            GapHostRequest request; request.action = GapHostAction::SetDisplay; request.display = display;
            const bool isAccepted = feature->SendRequest(std::move(request));
            const auto state = feature->GetState();
            panel->SetComplete(id, isAccepted ? "Succeeded" : "Rejected",
                {{"isOverlayVisible", state.isOverlayVisible}, {"range", GetValues(state.display.range)},
                    {"palette", static_cast<int>(state.display.mode)}, {"resultSet", GetRefText(state.resultSet)}});
        }, TestPolicy::View);
    for (const bool exit : {false, true}) panel->AttachAction(exit ? "Exit" : "Overlay", {},
        [panel, feature, pendingExit, exit](auto id, const auto&) {
            GapHostRequest request; request.action = exit ? GapHostAction::Exit : GapHostAction::Overlay;
            const bool accepted = feature->SendRequest(std::move(request));
            if (accepted && exit && feature->GetState().isExitPending) {
                *pendingExit = id; panel->SetAdmission(id, true);
            } else panel->SetComplete(id, accepted ? "Succeeded" : "Rejected",
                {{"isOverlayVisible", feature->GetState().isOverlayVisible}});
        }, exit ? TestPolicy::Stop : TestPolicy::View);
    panel->onStop = [panel] { panel->SendAction("Exit", {}); };
    panel->onObserve = [panel, feature, pendingExit] {
        const auto state = feature->GetState();
        if (*pendingExit && !state.isExitPending) {
            panel->SetComplete(*pendingExit, "Exited"); *pendingExit = 0;
        }
        auto summary = GetStatistics(state.statistics);
        summary["analysisState"] = static_cast<int>(state.analysisState);
        summary["isExitPending"] = state.isExitPending;
        summary["isViewActive"] = state.isViewActive;
        summary["isOverlayVisible"] = state.isOverlayVisible;
        summary["range"] = GetValues(state.display.range);
        summary["palette"] = static_cast<int>(state.display.mode);
        summary["source"] = GetRefText(state.sourceRevision);
        summary["resultSet"] = GetRefText(state.resultSet);
        const auto input = panel->GetSession()->GetImageDescriptor();
        summary["isBusy"] = state.analysisState == GapAnalysisState::Running || state.isExitPending;
        summary["hasResult"] = GetDataRevisionRefValid(state.resultSet);
        summary["isCurrent"] = input && input->dataRevision == state.sourceRevision && state.analysisState == GapAnalysisState::Succeeded;
        panel->SetState(summary);
    };
    return panel;
}
}
