// 测试用途：执行前校验当前输入、候选和结果的业务前置条件。
#include "ActionPolicy.h"
namespace Manual {
QString GetActionRequirement(const QString& module, const QString& action, const QJsonObject& s, bool hasInput)
{
    if (action == "GraphInfo") return {};
    if (action == "UseData") return hasInput ? QString() : "请先加载输入数据。";
    if (!hasInput && module != "Data" && !(module == "Alignment" && action.startsWith("Export"))) return "请先加载输入数据。";
    if (module == "Data" && !hasInput && action != "Load" && action != "Select" && action != "Descriptor" && action != "LabelDescriptors") return "此操作需要当前输入数据。";
    if (module == "Crop") return s["disabled"].toObject()[action].toString();
    if (module == "Gap" && action == "Overlay" && !s["isCurrent"].toBool()) return "先完成当前输入的孔隙分析，才能切换结果显示。";
    if (module == "Part" && (action == "SetState" || action == "Highlight" || action == "ClearHighlight" || action == "EditSelected" || action == "Visibility") && !s["hasCurrentParts"].toBool()) return "请先完成当前输入的零件分割。";
    if (module == "PartEdit") {
        if (!s["hasCurrentParts"].toBool()) return "请先在零件分割页生成当前输入的有效目录。";
        if ((action == "Commit" || action == "Discard") && !s["hasPreview"].toBool()) return "当前没有编辑候选，请先执行编辑或撤销/重做。";
        if (action != "Commit" && action != "Discard" && s["hasPreview"].toBool()) return "先确认或丢弃已有候选，再开始下一次编辑。";
    }
    if (module == "Surface") {
        if (action.startsWith("CopyIsoTo") && !s["hasIso"].toBool()) return "当前输入还没有有效阈值估计。";
        if (action == "SamplePoints" && !s["hasMesh"].toBool()) return "请先生成当前输入的表面网格。";
        if (action == "OpenAlignment" && !s["hasMeasurement"].toBool()) return "请先生成局部自适应或梯度峰值测量表面。";
    }
    if (module == "Artifact") {
        if (action == "Commit" && !s["isCandidateCurrent"].toBool()) return "当前输入与校正候选不匹配，请恢复源输入或丢弃候选。";
        if ((action == "Ring" || action == "Diffusion" || action == "Combined") && s["hasCandidate"].toBool()) return "先发布或丢弃已有校正候选。";
        if ((action == "Commit" || action == "Discard") && !s["hasCandidate"].toBool()) return "当前没有可发布或丢弃的校正候选。";
        if (action == "SelectOutput" && !s["canSelectOutput"].toBool()) return "当前没有匹配输入的已发布校正结果。";
        if (action == "RestoreSource" && !s["isOutputCurrent"].toBool()) return "仅在使用本次校正结果时可以恢复其源数据。";
    }
    if (module == "Wall" && action != "Start" && action != "Cancel" && action != "Clear" && !s["hasResult"].toBool())
        return "请先完成壁厚计算。";
    if (module == "Rotation" && action == "Undo" && s["undoCount"].toString().toULongLong() == 0) return "当前没有可撤销的旋转。";
    if (module == "Alignment") {
        if ((action == "ImportReference" || action == "SaveRecipe" || action == "Start" || action == "Restore") && s["isApplied"].toBool()) return "请先停用当前对齐结果，再准备或求解新方案。";
        if ((action == "SaveRecipe" || action == "Start" || action == "Restore") && !s["isReferenceCurrent"].toBool()) return "请先导入当前输入的名义参考。";
        if (action == "Start" && !s["hasRecipe"].toBool()) return "请先保存对齐方案。";
        if ((action == "Activate" || action == "Result" || action == "ExportArchive") && !s["hasResult"].toBool()) return "请先求解得到对齐结果。";
        if (action == "Activate" && !s["isResultCurrent"].toBool()) return "旧输入的对齐结果不能应用到当前数据。";
        if (action == "Deactivate" && !s["isApplied"].toBool()) return "当前没有已应用的对齐结果。";
    }
    return {};
}
}
