// 测试用途：通过数据页面测试体数据加载、精确输入选择、导出、标签读取和掩码准备。
#include "ModuleFactories.h"
#include "Support/ReferenceDataSource.h"
#include <QFileInfo>
#include "../../../Host/FeatureInput.h"
namespace Manual {
ModulePanel* CreateDataTest(TestContext context, std::shared_ptr<ReferenceDataSource> reference, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Data", parent);
    panel->SetNotice("RAW 使用原生字节序的 32 位浮点数据，X 轴变化最快。几何输入采用 LPS，数据描述采用 RAS；修订编号使用字符串。");
    panel->AttachAction("Load", GetJson(R"({"filePath":"","datasetId":"","dimensions":[1,1,1],"spacingLPS":[1,1,1],"originLPS":[0,0,0],"directionLPS":[1,0,0,0,1,0,0,0,1],"sourceDigest":"","evidenceKind":"real-data"})"),
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
            HostSliceExportRequest request;
            request.outputDir = GetText(params, "outputDir").toUtf8().toStdString();
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
        QJsonArray labels;
        for (const auto& label : panel->GetSession()->GetLabelMapDescriptors()) labels.append(QJsonObject{{"id", QString::fromStdString(label.id)}, {"revision", GetRefText(label.dataRevision)}, {"source", GetRefText(label.sourceRevision)}});
        panel->SetState({{"labels", labels}});
    };
    return panel;
}
}
