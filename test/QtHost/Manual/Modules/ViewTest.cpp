// 测试用途：通过视图页面测试显示模式、质量、阈值、游标与视图重置。
#include "ModuleFactories.h"
#include "Support/UiText.h"
#include "Support/ParameterEditor.h"
#include <QPointer>
#include <algorithm>
namespace Manual {
namespace {
// HostRenderViewState 的三个可见性位，分别对应 HostVisibilityParams 的三个字段。
constexpr std::uint32_t planesBit = 1U, crosshairBit = 2U, rulerBit = 4U;
HostRenderMode GetMode(const QJsonObject& params)
{
    return GetEnum<HostRenderMode>(params, "mode", {
        {"Volume", HostRenderMode::Volume}, {"IsoSurface", HostRenderMode::IsoSurface},
        {"CompositeVolume", HostRenderMode::CompositeVolume}, {"CompositeIsoSurface", HostRenderMode::CompositeIsoSurface},
        {"SliceTopDown", HostRenderMode::SliceTopDown}, {"SliceFrontBack", HostRenderMode::SliceFrontBack}, {"SliceLeftRight", HostRenderMode::SliceLeftRight}});
}
bool IsDisplayFieldApplicable(HostRenderMode mode, const QString& key, bool primary)
{
    const bool volume = mode == HostRenderMode::Volume || mode == HostRenderMode::CompositeVolume;
    const bool iso = mode == HostRenderMode::IsoSurface || mode == HostRenderMode::CompositeIsoSurface;
    if (key == "mode") return primary;
    if (key == "iso") return iso;
    if (key == "transfer") return volume;
    if (key == "quality" || key == "opacity") return volume || iso;
    if (key == "windowLevel") return !volume && !iso;
    return true;
}
bool IsApplicable(HostRenderMode mode, std::uint32_t bit)
{
    const bool slice = mode == HostRenderMode::SliceTopDown || mode == HostRenderMode::SliceFrontBack || mode == HostRenderMode::SliceLeftRight;
    if (bit == crosshairBit) return slice;
    if (bit == planesBit) return mode == HostRenderMode::CompositeVolume || mode == HostRenderMode::CompositeIsoSurface;
    return !slice;
}
std::vector<HostRenderViewState> GetScope(ModulePanel* panel, const QString& scope)
{
    std::vector<HostRenderViewState> result;
    for (const auto& view : panel->GetSession()->GetRenderViewStates())
        if (scope == "all" || (scope == "slices" && QString::fromStdString(view.id).startsWith("slice-")) || view.id == scope.toStdString()) result.push_back(view);
    return result;
}
QJsonValue GetVisibility(const std::vector<HostRenderViewState>& views, std::uint32_t bit)
{
    QJsonValue first;
    for (const auto& view : views) if (IsApplicable(view.viewMode, bit)) {
        const bool visible = (view.visibilityMask & bit) != 0;
        if (first.isNull()) first = visible;
        else if (first.toBool() != visible) return {};
    }
    return first;
}
}
ModulePanel* CreateViewTest(TestContext context, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "View", parent);
    panel->SetNotice("中央保留一个三维视窗和三个切片图。三维可切换等值面与体渲染；切片可用滚轮逐层查看，也可通过“切片位置”定位。");
    panel->AttachAction("Set", GetJson(R"({"viewId":"primary-3d","mode":null,"iso":null,"opacity":null,"quality":null,"axes":null,"windowLevel":null,"transfer":null})"),
        [panel](auto id, const auto& params) {
            HostViewSetRequest request;
            request.targetView.viewId = GetText(params, "viewId").toStdString();
            const auto current = panel->GetSession()->GetRenderViewState(request.targetView);
            if (!current) throw std::invalid_argument("当前没有可用视图");
            if (params.contains("mode") && !params["mode"].isNull()) request.mode = GetMode(params);
            const auto effectiveMode = request.mode.value_or(current->viewMode);
            for (const auto* key : {"mode", "iso", "opacity", "quality", "windowLevel", "transfer"}) {
                if (params.contains(key) && !params[key].isNull()
                    && !IsDisplayFieldApplicable(effectiveMode, key, request.targetView.viewId == "primary-3d"))
                    throw std::invalid_argument((GetParameterText(key) + "不适用于当前视图模式").toStdString());
            }
            if (params.contains("iso") && !params["iso"].isNull()) request.iso = GetNumber(params, "iso");
            if (params.contains("opacity") && !params["opacity"].isNull()) request.opacity = GetNumber(params, "opacity");
            if (params.contains("axes") && !params["axes"].isNull()) request.isAxesVisible = GetBool(params, "axes");
            if (params.contains("quality") && !params["quality"].isNull()) request.volumeQuality = GetEnum<HostVolumeQuality>(params, "quality", {
                {"Auto", HostVolumeQuality::Auto}, {"Low", HostVolumeQuality::Low}, {"High", HostVolumeQuality::High},
                {"XHigh", HostVolumeQuality::XHigh}, {"Ultra", HostVolumeQuality::Ultra}});
            if (params.contains("windowLevel") && !params["windowLevel"].isNull()) {
                const auto value = GetArray<double, 2>(params["windowLevel"]);
                request.windowLevel = HostWindowLevelParams{value[0], value[1]};
            }
            if (params.contains("transfer") && !params["transfer"].isNull()) {
                const auto value = params["transfer"].toObject();
                HostVolumeTransferFunction transfer;
                for (const auto node : value["colorNodes"].toArray()) {
                    const auto point = GetArray<double, 4>(node);
                    transfer.colorNodes.push_back({point[0], point[1], point[2], point[3]});
                }
                for (const auto node : value["opacityNodes"].toArray()) {
                    const auto point = GetArray<double, 2>(node);
                    transfer.opacityNodes.push_back({point[0], point[1]});
                }
                request.volumeTransferFunction = transfer;
            }
            if (request.mode) {
                const auto mode = *request.mode;
                const bool sliceMode = mode == HostRenderMode::SliceTopDown || mode == HostRenderMode::SliceFrontBack || mode == HostRenderMode::SliceLeftRight;
                if (request.targetView.viewId != "primary-3d" || sliceMode)
                    throw std::invalid_argument("渲染模式仅作用于三维视窗；三个切片窗口保持各自方向");
            }
            panel->SendHost(id, std::move(request));
        }, TestPolicy::View);
    panel->AttachAction("Visibility", {{"viewScope", "all"}, {"planes", QJsonValue()}, {"crosshair", QJsonValue()}, {"ruler", QJsonValue()}},
        [panel](auto id, const auto& params) {
            const auto views = GetScope(panel, GetText(params, "viewScope"));
            if (views.empty()) throw std::invalid_argument("辅助显示作用范围没有可用视图");
            HostVisibilityParams visibility;
            if (!params["planes"].isNull()) visibility.isPlanes3DVisible = GetBool(params, "planes");
            if (!params["crosshair"].isNull()) visibility.isCrosshairVisible = GetBool(params, "crosshair");
            if (!params["ruler"].isNull()) visibility.isRulerVisible = GetBool(params, "ruler");
            std::vector<HostViewSetRequest> requests;
            for (const auto& view : views) {
                auto applicable = visibility;
                if (!IsApplicable(view.viewMode, planesBit)) applicable.isPlanes3DVisible.reset();
                if (!IsApplicable(view.viewMode, crosshairBit)) applicable.isCrosshairVisible.reset();
                if (!IsApplicable(view.viewMode, rulerBit)) applicable.isRulerVisible.reset();
                if (!applicable.isPlanes3DVisible.has_value() && !applicable.isCrosshairVisible.has_value() && !applicable.isRulerVisible.has_value()) continue;
                HostViewSetRequest request; request.targetView.viewId = view.id; request.visibility = applicable;
                requests.push_back(std::move(request));
            }
            if (requests.empty()) throw std::invalid_argument("当前范围没有可应用的修改，请明确设置适用于当前视图模式的项");
            struct Batch { std::size_t remaining; QStringList errors; QJsonArray targets; };
            auto batch = std::make_shared<Batch>(); batch->remaining = requests.size();
            for (const auto& request : requests) batch->targets.append(QString::fromStdString(request.targetView.viewId));
            const QPointer<ModulePanel> owner(panel); bool accepted = false;
            for (auto& request : requests) {
                const auto viewId = request.targetView.viewId;
                accepted = panel->GetSession()->SendRequestResult(std::move(request), [owner, batch, id, viewId](HostResult result) {
                    if (!owner) return;
                    if (!result.isSucceeded) batch->errors.append(QString::fromStdString(viewId + ": " + result.message));
                    if (--batch->remaining == 0) owner->SetComplete(id, batch->errors.isEmpty() ? "Succeeded" : "Failed",
                        {{"views", batch->targets}, {"message", batch->errors.isEmpty() ? QString("已更新 %1 个视图的辅助显示").arg(batch->targets.size()) : batch->errors.join("；")}});
                }) || accepted;
            }
            panel->SetAdmission(id, accepted);
        }, TestPolicy::View);
    panel->AttachAction("Cursor", GetJson(R"({"world":[0,0,0],"axis":-1})"), [panel](auto id, const auto& params) {
        HostSessionSetRequest request;
        const auto axis = GetNumber(params, "axis");
        if (axis < -1 || axis > 2 || axis != std::trunc(axis)) throw std::invalid_argument("axis 必须为 -1..2 整数");
        request.cursor = HostCursorParams{GetArray<double, 3>(params["world"]), static_cast<int>(axis)};
        panel->SendHost(id, std::move(request));
    }, TestPolicy::View);
    panel->AttachAction("Reset", {{"viewId", "primary-3d"}}, [panel](auto id, const auto& params) {
        HostViewResetRequest request;
        request.targetView.viewId = GetText(params, "viewId").toStdString();
        panel->SendHost(id, std::move(request));
    }, TestPolicy::View);
    panel->AttachAction("State", {}, [panel](auto id, const auto&) {
        QJsonArray views;
        for (const auto& view : panel->GetSession()->GetSceneViewStates())
            views.append(QJsonObject{{"viewId", QString::fromStdString(view.id)},
                {"sceneEpoch", QString::number(view.sceneEpoch)}, {"renderedEpoch", QString::number(view.renderedEpoch)}});
        panel->SetComplete(id, "Observed", {{"views", views}});
    }, TestPolicy::Read);
    struct DisplayContext { QString viewId; std::optional<HostRenderMode> mode; };
    auto displayContext = std::make_shared<DisplayContext>();
    panel->onObserve = [panel, displayContext] {
        QJsonArray views;
        const auto labels = GetParameterChoices("View", "viewId");
        for (const auto& scene : panel->GetSession()->GetSceneViewStates()) {
            const auto id = QString::fromStdString(scene.id); QString name = id;
            for (const auto& label : labels) if (label.first == id) name = label.second;
            // 展示只关心这个视图的绘制需求；裁剪确认保留含 owner 队列的业务屏障。
            const auto pending = panel->GetContext().workflow.getViewRenderPending;
            const auto& applied = scene.presentation;
            views.append(QJsonObject{{"viewId", id}, {"name", name}, {"isCurrent", scene.isAvailable && (!pending || !pending(scene.id))},
                {"axes", applied ? QJsonValue(applied->isAxesVisible) : QJsonValue()},
                {"planes", applied && IsApplicable(applied->viewMode, planesBit) ? QJsonValue((applied->visibilityMask & planesBit) != 0) : QJsonValue()},
                {"crosshair", applied && IsApplicable(applied->viewMode, crosshairBit) ? QJsonValue((applied->visibilityMask & crosshairBit) != 0) : QJsonValue()},
                {"ruler", applied && IsApplicable(applied->viewMode, rulerBit) ? QJsonValue((applied->visibilityMask & rulerBit) != 0) : QJsonValue()}});
        }
        auto* display = panel->GetParameterEditor("Set");
        const auto viewId = display->GetField("viewId")->GetValue().toString();
        const auto applied = panel->GetSession()->GetRenderViewState({viewId.toStdString()});
        auto* modeField = display->GetField("mode");
        if (applied && (displayContext->viewId != viewId || displayContext->mode != applied->viewMode)) {
            // 外部工具栏切换或改选目标后，不把上一视图的模式草稿重新带回请求。
            modeField->SetValue(QJsonValue());
            displayContext->viewId = viewId; displayContext->mode = applied->viewMode;
        }
        auto effectiveMode = applied ? applied->viewMode : HostRenderMode::CompositeIsoSurface;
        if (!modeField->GetValue().isNull()) {
            try { effectiveMode = GetMode({{"mode", modeField->GetValue()}}); }
            catch (const std::invalid_argument&) { /* 非法导入草稿在提交时报告，观察仍使用当前模式。 */ }
        }
        QJsonObject applicability;
        for (const auto* key : {"mode", "iso", "opacity", "quality", "windowLevel", "transfer", "axes"})
            applicability[key] = applied && IsDisplayFieldApplicable(effectiveMode, key, viewId == "primary-3d");
        display->SetFieldApplicability(applicability);
        display->GetField("axes")->SetAppliedBoolean(applied ? QJsonValue(applied->isAxesVisible) : QJsonValue(), viewId, applied ? QString() : "当前没有可用视图");
        auto* visibility = panel->GetParameterEditor("Visibility");
        const auto scope = visibility->GetField("viewScope")->GetValue().toString();
        const auto targets = GetScope(panel, scope);
        QString context = scope;
        for (const auto& target : targets) context += QString("/%1:%2").arg(QString::fromStdString(target.id)).arg(static_cast<int>(target.viewMode));
        for (const auto& entry : {std::make_pair("planes", planesBit), std::make_pair("crosshair", crosshairBit), std::make_pair("ruler", rulerBit)}) {
            const bool available = std::any_of(targets.begin(), targets.end(), [&](const auto& target) { return IsApplicable(target.viewMode, entry.second); });
            visibility->GetField(entry.first)->SetAppliedBoolean(GetVisibility(targets, entry.second), context, available ? QString() : "当前视图模式不支持此项");
        }
        panel->SetState({{"views", views}, {"displayMode", int(effectiveMode)}, {"displayView", viewId}});
    };
    return panel;
}
}
