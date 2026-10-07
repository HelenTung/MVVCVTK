// 测试用途：通过数据页面测试体数据加载、精确输入选择、导出、标签读取和掩码准备。
#include "ModuleFactories.h"
#include "Support/ReferenceDataSource.h"
#include <QFileInfo>
#if defined(MANUAL_ROI)
#include "Host/RoiEditingHostFeature.h"
#endif
#include "../../../Host/FeatureInput.h"
namespace Manual {
ModulePanel* CreateDataTest(TestContext context, std::shared_ptr<ReferenceDataSource> reference, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Data", parent);
#if defined(MANUAL_ROI)
    RoiEditingConfig roiConfig;roiConfig.referenceView.viewId="primary-3d";roiConfig.targetViews=GetAllViews();
    auto editor=std::make_shared<RoiEditingHostFeature>(roiConfig);
    if(!context.runtime.AttachFeature(editor))throw std::runtime_error("ROI editor attach failed");
    panel->AttachAction("EditRoiBox",{{"matrix",GetValues(roiIdentityMatrix)}},[panel,editor](auto id,const auto& p){
        const auto source=panel->GetSession()->GetImageDescriptor();if(!source)throw std::runtime_error("source unavailable");
        RoiRequest draft;draft.definition.source=source->dataRevision;draft.metadata.name="comparison-half-box";
        RoiNode box;box.primitive.localToSource=GetArray<double,16>(p["matrix"]);draft.definition.nodes.push_back(box);
        const auto catalog=panel->GetSession()->GetRoiDescriptors(true);draft.expectedCatalogRevision=catalog.empty()?0:catalog.front().catalogRevision;
        RoiEditingRequest begin;begin.action=RoiEditingAction::Begin;begin.draft=draft;
        const auto first=editor->SendRequest(begin);
        if(first.error!=RoiError::None)throw std::runtime_error("ROI Begin failed");
        const bool hadDraft=editor->GetState().hasDraft;
        RoiEditingRequest commit;commit.action=RoiEditingAction::Commit;const auto result=editor->SendRequest(commit);
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"draftBeforeCommit",hadDraft},{"draftAfterCommit",editor->GetState().hasDraft},{"roi",result.roi?GetRefText(result.roi->revision):QString()},{"error",int(result.error)}});
    },TestPolicy::Compute);
#endif
    panel->SetNotice("RAW 使用原生字节序的 32 位浮点数据，X 轴变化最快。几何输入采用 LPS，数据描述采用 RAS；修订编号使用字符串。");
    panel->AttachAction("Load", GetJson(R"({"filePath":"F:/data/ct/1536x1536x1536_1440.raw","datasetId":"1","dimensions":[1536,1536,1536],"spacingLPS":[0.1537,0.1537,0.1537],"originLPS":[0,0,0],"directionLPS":[1,0,0,0,1,0,0,0,1],"sourceDigest":"","evidenceKind":"real-data"})"),
        [panel](auto id, const auto& params) {
            FeatureTestOptions options;
            const auto path = GetText(params, "filePath");
            if (!QFileInfo::exists(path)) throw std::invalid_argument("输入文件不存在");
            options.inputPath = path.toUtf8().toStdString();
            options.hasDimensions = true;
            options.dimensions = GetArray<int, 3>(params["dimensions"]);
            options.spacing = GetArray<float, 3>(params["spacingLPS"]);
            options.origin = GetArray<float, 3>(params["originLPS"]);
            options.direction = GetArray<double, 9>(params["directionLPS"]);
            options.datasetId = GetText(params, "datasetId").toStdString();
            options.inputDigest = GetText(params, "sourceDigest").toStdString();
            options.inputFrame = "LPS";
            options.inputUnit = "mm";
            options.inputFormat = "float32-le-xfastest";
            auto request = GetFeatureLoadRequest(options, false);
            request.metadata.attributes.push_back({"manual.evidence", GetText(params, "evidenceKind").toStdString()});
            panel->SendHost(id, std::move(request));
        }, TestPolicy::Input, true);
    panel->AttachAction("ActivateAccepted", {}, [panel](auto id, const auto&) {
        const auto snapshot = panel->GetSession()->GetStateSnapshot();
        if (!snapshot || !GetDataRevisionRefValid(snapshot->load.acceptedRevision))
            throw std::invalid_argument("没有已接纳的数据可激活");
        HostLoadActivationRequest request;
        request.dataRevision = snapshot->load.acceptedRevision;
        request.expectedBindingRevision = snapshot->load.activeBindingRevision;
        panel->SendHost(id, std::move(request));
    }, TestPolicy::Input, true);
    panel->AttachAction("Descriptor", {}, [panel](auto id, const auto&) {
        panel->SetComplete(id, "Observed", GetDescriptor(panel->GetSession()->GetImageDescriptor()));
    }, TestPolicy::Read);
    panel->AttachAction("Select", {{"revision", ""}, {"expectedBindingRevision", "current"}},
        [panel](auto id, const auto& params) {
            const auto ref = GetRef(params["revision"]);
            if (GetText(params, "expectedBindingRevision") == "current") panel->SetInput(id, ref);
            else {
                HostDataSelectRequest request;
                request.dataRevision = ref;
                request.expectedBindingRevision = GetId(params["expectedBindingRevision"]);
                panel->SendHost(id, std::move(request));
            }
        }, TestPolicy::Input, true);
    panel->AttachAction("ExportData", {{"outputPath", ""}, {"viewId", "primary-3d"}, {"format", "Ply"}},
        [panel](auto id, const auto& params) {
            HostDataExportRequest request;
            request.outputPath = GetText(params, "outputPath").toUtf8().toStdString();
            request.sourceView.viewId = GetText(params, "viewId").toStdString();
            request.format = GetEnum<HostDataExportFormat>(params, "format", {{"Raw", HostDataExportFormat::Raw},
                {"Ply", HostDataExportFormat::Ply}, {"Stl", HostDataExportFormat::Stl}, {"Obj", HostDataExportFormat::Obj}});
            panel->SendHost(id, std::move(request));
        }, TestPolicy::Compute);
    panel->AttachAction("ExportSlices", {{"outputDir", ""}, {"viewId", "slice-top-down"}, {"angleDeg", QJsonValue()}},
        [panel](auto id, const auto& params) {
            HostSliceExportRequest request; request.outputDir = GetText(params, "outputDir").toUtf8().toStdString();
            request.sourceView.viewId = GetText(params, "viewId").toStdString();
            if (!params["angleDeg"].isNull()) request.angleDeg = GetNumber(params, "angleDeg");
            panel->SendHost(id, std::move(request));
        }, TestPolicy::Compute);
    panel->AttachAction("LabelDescriptors", {}, [panel](auto id, const auto&) {
        QJsonArray labels;
        for (const auto& value : panel->GetSession()->GetLabelMapDescriptors())
            labels.append(QJsonObject{{"id", QString::fromStdString(value.id)},
                {"revision", GetRefText(value.dataRevision)}, {"source", GetRefText(value.sourceRevision)},
                {"dims", GetValues(value.dims)}});
        panel->SetComplete(id, "Observed", {{"labels", labels}});
    }, TestPolicy::Read);
    panel->AttachAction("ReadLabelRegion", GetJson(R"({"id":"","revision":"","offset":[0,0,0],"size":[1,1,1]})"),
        [panel](auto id, const auto& params) {
            LabelMapReadRequest request;
            request.id = GetText(params, "id").toStdString();
            request.expectedRevision = GetRef(params["revision"]);
            request.region = ImageReadRegion{GetArray<std::size_t, 3>(params["offset"]), GetArray<std::size_t, 3>(params["size"])};
            request.maxBytes = 1024 * 1024;
            const auto result = panel->GetSession()->GetLabelMapReadResult(request);
            QByteArray bytes;
            if (result.state && result.state->values) {
                const auto& values = *result.state->values;
                bytes = QByteArray(reinterpret_cast<const char*>(values.data()), static_cast<int>(values.size()));
            }
            panel->SetComplete(id, result.error == LabelMapError::None ? "Succeeded" : "Failed",
                {{"error", static_cast<int>(result.error)}, {"requiredBytes", QString::number(result.requiredBytes)},
                    {"nativeBytesHex", QString::fromLatin1(bytes.toHex())}});
        }, TestPolicy::Read);
    panel->AttachAction("CreateMask", GetJson(R"({"defaultValue":false,"boxValue":true,"indexBoxes":[[0,0,0,0,0,0]],"purpose":"作用区域或保护掩码测试"})"),
        [panel, reference](auto id, const auto& params) {
            const auto descriptor = panel->GetSession()->GetImageDescriptor();
            if (!descriptor) throw std::invalid_argument("未加载掩码源");
            const auto mask = reference->CreateMask(descriptor->dataRevision, params);
            panel->SetComplete(id, "MaskPublished", {{"mask", GetRefText(mask)},
                {"source", GetRefText(descriptor->dataRevision)}, {"validationRoute", "trusted-input"}});
        }, TestPolicy::Compute, true);
    panel->onObserve = [panel] {
        // Comparison exports below read immutable results through the public ports.
        QJsonArray labels;
        for (const auto& label : panel->GetSession()->GetLabelMapDescriptors()) labels.append(QJsonObject{{"id", QString::fromStdString(label.id)}, {"revision", GetRefText(label.dataRevision)}, {"source", GetRefText(label.sourceRevision)}});
        QJsonObject state{{"labels", labels}};
        if (const auto snapshot = panel->GetSession()->GetStateSnapshot()) {
            state["acceptedRevision"] = GetRefText(snapshot->load.acceptedRevision);
            state["activeRevision"] = GetRefText(snapshot->load.activeRevision);
            state["activationPhase"] = static_cast<int>(snapshot->load.status);
        }
        panel->SetState(state);
    };
    panel->AttachAction("ReadTransform", {}, [panel, reference](auto id,const auto&) {
        panel->SetComplete(id,"Observed",reference->ReadTransform());
    },TestPolicy::Read);
    panel->AttachAction("ReadRoi", {{"roi","latest"}}, [panel, reference](auto id,const auto& p) {
        const auto input=panel->GetSession()->GetImageDescriptor();
        if(!input)throw std::runtime_error("source unavailable");
        const auto catalog=panel->GetSession()->GetRoiDescriptors();
        if(catalog.empty())throw std::runtime_error("ROI unavailable");
        const auto ref=GetText(p,"roi")=="latest"?catalog.back().revision:GetRef(p["roi"]);
        panel->SetComplete(id,"Observed",reference->ReadRoi(ref,input->dataRevision));
    },TestPolicy::Read);
    return panel;
}
}
