// 测试用途：执行前校验当前输入、候选和结果的业务前置条件。
#include "ActionPolicy.h"
#include "UiText.h"
namespace Manual {
QString GetActionRequirement(const QString& module, const QString& action, const QJsonObject& s, bool hasInput)
{
    if (module == "Data" && action == "ActivateAccepted") return s["acceptedRevision"].toString().isEmpty() ? "没有已接纳的数据可激活。" : QString();
    if (action == "GraphInfo") return {};
    if (action == "KeepResult" || action == "ReleaseResult" || action == "UnloadResult") return {};
    if (action == "UseData") return hasInput ? QString() : "请先加载输入数据。";
    const bool readableResult = (module == "Wall" || module == "Alignment")
        && (action == "Result" || action == "ResultEvidence" || action == "ExportArchive")
        && s["selectedResults"].toObject()[action].toBool();
    const bool inspectionRead = module == "Alignment" && (action == "InspectionResult" || action == "DeviationResult" || action == "VoxelVolumeResult");
    const bool objectRead=(module=="Surface"&&QStringList{"ReadObjects","NextObjectPage","ReadObjectPoints","ReadObjectTriangles"}.contains(action))
        ||(module=="Gap"&&QStringList{"Defects","NextDefectPage","ReadDefectGeometry"}.contains(action));
    if (!hasInput && !readableResult && !inspectionRead && !objectRead && module != "Data" && !(module == "Alignment" && action.startsWith("Export"))) return "请先加载输入数据。";
    if (module == "Data" && !hasInput && action != "Load" && action != "Select" && action != "Descriptor" && action != "LabelDescriptors") return "此操作需要当前输入数据。";
    if (module == "Crop") return s["disabled"].toObject()[action].toString();
    if((module=="Surface"||module=="Gap")&&QStringList{"SelectObject","IsolateObject","ClearObjectSelection","ObjectPicking"}.contains(action)
        &&!(module=="Surface"?s["isFormalCurrent"].toBool():s["isCurrent"].toBool()))return "请先生成当前输入的正式结果。";
    if(module=="Roi" && action=="Commit" && s["isDragging"].toBool())return "请先结束区域拖动，再保存正式 ROI。";
    if (module == "Gap" && (action == "Overlay" || action == "SetDisplay") && !s["isCurrent"].toBool()) return "先完成当前输入的孔隙分析，才能切换结果显示。";
    if (module == "Gap" && action == "NextDefectPage" && !s["hasNextDefectPage"].toBool())return "当前已经是缺陷列表末页。";
    if (module == "Gap" && action == "ResetDefectFilter" && !s["isListFiltered"].toBool())return "当前列表没有区间筛选。";
    if (module == "Part" && (action == "SetState" || action == "Highlight" || action == "ClearHighlight" || action == "EditSelected" || action == "Visibility") && !s["hasCurrentParts"].toBool()) return "请先完成当前输入的零件分割。";
    if (module == "Part" && IsPartEditAction(action)) {
        if (!s["hasCurrentParts"].toBool()) return "请先分割生成当前输入的有效零件目录。";
        if ((action == "Commit" || action == "Discard") && !s["hasPreview"].toBool()) return "当前没有编辑候选，请先执行编辑或撤销/重做。";
        if (action != "Commit" && action != "Discard" && s["hasPreview"].toBool()) return "先确认或丢弃已有候选，再开始下一次编辑。";
    }
    if (module == "Surface") {
        if (action.startsWith("CopyIsoTo") && !s["hasIso"].toBool()) return "当前输入还没有有效阈值估计。";
        if (action == "CopyIsoToDisplay" && !s["canApplyIsoToDisplay"].toBool()) return "主三维当前不是等值面模式，请先切换显示模式。";
        if (action == "Visibility" && !s["hasMesh"].toBool()) return "当前没有可显示的表面网格。";
        if ((action == "SamplePoints" || action == "MeasurementSelection") && !s["hasMesh"].toBool()) return "请先生成当前输入的表面网格。";
        if ((action == "OpenAlignment" || action == "OpenWall") && !s["hasMeasurement"].toBool()) return "请先生成当前输入的有效测量表面。";
    }
    if (module == "Artifact") {
        if(action=="Recompute" && !s["hasCandidate"].toBool())return "当前没有待丢弃并重新计算的候选。";
        if (action == "Commit" && !s["isCandidateCurrent"].toBool()) return "候选的冻结来源/绑定已失效，请丢弃后重新计算；切回同名输入不会恢复绑定版本。";
        if ((action == "Ring" || action == "Diffusion" || action == "Combined") && s["hasCandidate"].toBool()) return "先发布或丢弃已有校正候选。";
        if ((action == "Commit" || action == "Discard") && !s["hasCandidate"].toBool()) return "当前没有可发布或丢弃的校正候选。";
        if (action == "SelectOutput" && !s["canSelectOutput"].toBool()) return "当前没有匹配输入的已发布校正结果。";
        if (action == "RestoreSource" && !s["isOutputCurrent"].toBool()) return "仅在使用本次校正结果时可以恢复其源数据。";
    }
    if (module == "Wall" && action != "Start" && action != "Cancel" && action != "Clear"
        && !s["hasResult"].toBool() && !s["hasReadableResult"].toBool())
        return "请选择已有壁厚结果，或提交所需输入开始计算。";
    if ((module == "Wall" || module == "Alignment") && s["selectedResults"].toObject().contains(action)
        && !s["selectedResults"].toObject()[action].toBool()) return "请在输入与结果中选择可用的业务结果。";
    if(module=="Wall" && action=="Start")return s["preparationProblem"].toString();
    if(module=="Wall" && QStringList{"LocateMinimum","SetEvaluation","SetDisplay","SelectSample"}.contains(action) && !s["viewedIsActive"].toBool())return "先明确激活正在查看的壁厚结果；历史阅读不会自动改变当前激活对象。";
    if (module == "Rotation" && action == "Undo" && s["undoCount"].toString().toULongLong() == 0) return "当前没有可撤销的旋转。";
    if (module == "Alignment") {
        if(QStringList{"InspectGeometry","InspectSection","InspectPair"}.contains(action))return s["inspectionProblem"].toString();
        if ((action == "ImportReference" || action == "ImportNominalMesh" || action == "SaveRecipe" || action == "Start" || action == "Restore") && s["isApplied"].toBool()) return "请先停用当前对齐结果，再准备或求解新方案。";
        if ((action == "SaveRecipe" || action == "Start" || action == "Restore") && !s["isReferenceCurrent"].toBool()) return "请先导入当前输入的名义参考。";
        if (action == "Start" && !s["hasRecipe"].toBool()) return "请先保存对齐方案。";
        if ((action == "Activate" || action == "Result" || action == "ExportArchive")
            && !s["hasResult"].toBool() && !s["hasReadableResult"].toBool()) return "请选择已有对齐结果，或提供参考与配方进行求解。";
        if (action == "Activate" && !s["isResultCurrent"].toBool()) return "旧输入的对齐结果不能应用到当前数据。";
        if (action == "Deactivate" && !s["isApplied"].toBool()) return "当前没有已应用的对齐结果。";
    }
    return {};
}
}
