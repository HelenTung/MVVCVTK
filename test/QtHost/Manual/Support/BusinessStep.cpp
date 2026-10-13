#include "BusinessStep.h"
#include "UiText.h"
#include <QJsonArray>
namespace Manual {
BusinessStep GetBusinessStep(const QString& module, const QJsonObject& s,
    const QJsonObject& input, const QString& parameterAction)
{
    const bool readsInspection=module=="Alignment" && QStringList{"InspectionResult","DeviationResult","VoxelVolumeResult","ExportReport"}.contains(parameterAction);
    const bool readsWall=module=="Wall" && s["hasReadableResult"].toBool() && parameterAction=="Result";
    if (!input["available"].toBool() && module != "Data" && !readsInspection && !readsWall)
        return {"尚无当前输入", "先在数据输入页面加载体数据。", "", "等待输入", {"OpenData"}};
    if (s["isBusy"].toBool())
        return {"任务处理中", "等待真实终态；取消被接纳后仍需等待任务退出。", "", "处理中", {"Cancel", "Stop", "Exit"}};
    const auto current = input["dataRevision"].toString();
    if (module == "Data") {
        const auto accepted = s["acceptedRevision"].toString();
        if (!accepted.isEmpty() && accepted != s["activeRevision"].toString())
            return {"数据已接纳，采用尚未完成", "保留已加载数据，重试显示组激活；顶部仍显示实际当前输入。", "ActivateAccepted", "重试使用已加载数据", {"Load", "Descriptor"}};
        return {input["available"].toBool() ? "当前输入已就绪" : "待加载体数据",
            "核对 RAW 尺寸与 LPS 几何，加载完成后可进入显示或处理业务。", "Load", "加载并使用体数据", {"Descriptor", "ExportData", "OpenCrop"}};
    }
    if (module == "View") return {"显示参数草稿", "选择目标视图和显示模式，再明确提交；修改草稿不执行算法。",
        QStringList{"Visibility", "Cursor"}.contains(parameterAction) ? parameterAction : "Set", "提交显示设置", {"Reset", "State"}};
    if(module=="Rotation" && !s["isEnabled"].toBool())return {"模型旋转尚未启用","先启用模型旋转，再按世界轴、中心和角度执行。","SetEnabled","启用模型旋转",{"Undo"}};
    if (module == "Rotation") return {s["isInvalidated"].toBool() ? "源数据已变化" : "模型姿态调整",
        "使用世界轴、世界中心与角度旋转模型；相机导航与计量对齐独立。", "Rotate", "旋转模型", {"SetEnabled", "Undo", "Cancel"}};
    if (module == "Artifact") {
        if (s["isOutputCurrent"].toBool()) return {"校正结果已采用", "当前输入已切换为校正结果，可定义区域或继续裁剪。", "", "校正结果已使用", {"OpenRoi", "OpenCrop", "RestoreSource"}};
        if (s["canSelectOutput"].toBool()) return {"校正结果已发布，尚未采用", "直接使用已发布修订；采用失败可再次重试，无需重新校正。", "SelectOutput", "使用已发布校正结果", {"RestoreSource", "OpenCrop"}};
        if (s["hasCandidate"].toBool()) return {s["isCandidateCurrent"].toBool() ? "校正候选待确认" : "候选来源与当前输入不一致",
            "查看质量与候选来源，发布后使用精确输出修订。", "Commit", "发布并使用校正结果", {"Discard"}, true};
        const auto action = QStringList{"Ring", "Diffusion", "Combined"}.contains(parameterAction) ? parameterAction : "Ring";
        return {"待生成校正候选", "选择校正方法和作用范围，先生成候选并核对质量。", action, "生成校正候选", {"OpenRoi"}};
    }
    if (module == "Roi") {
        if (s["hasDraft"].toBool()) return {"区域草稿", "调整区域后保存正式 ROI；隐藏区域不会删除它。", "Commit", "保存感兴趣区域", {"SetDraft", "Cancel", "Visibility"}};
        const auto action=QStringList{"Edit","SetMetadata","Copy"}.contains(parameterAction)?parameterAction:QString("Begin");
        return {"区域管理", "保存区域后，在各业务的范围中直接选用；区域不需要生成或采用数据结果。", action,
            action=="Begin"?"新建区域":GetActionText(module,action), {"Begin","Edit","SetMetadata","Copy","Visibility"}};
    }
    if (module == "Crop") {
        const auto output=s["output"].toString();
        const bool hasOutput=!output.isEmpty() && !output.startsWith("00000000000000000000000000000000:");
        if (hasOutput && output == current)
            return {"裁剪结果已采用", "有效域已改变；体素网格可以保持原尺寸。继续裁剪会为当前输入建立新文档。", "Start", "继续裁剪当前输入", {"OpenPart", "OpenSurface", "RestoreSource"}};
        if (hasOutput && !s["isConfirmed"].toBool())
            return {"裁剪结果已发布，尚未采用", "使用现有输出修订；采用失败保留结果并允许重试。", "SelectOutput", "使用已生成裁剪结果", {"Start", "RestoreSource"}};
        if (s["isConfirmed"].toBool())
            return {"裁剪预览已确认，待生成", "确认绑定当前文档、节点和源修订。生成成功后使用该精确输出。", "BuildResult", "生成并使用裁剪结果", {"Start", "SaveRoi"}, true};
        if (s["isActive"].toBool() || s["operationCount"].toString().toULongLong())
            return {"裁剪预览待确认", "拖动控件并等待各视图完成，再确认当前节点；选择历史后需要重新确认。", "FinishEditing", "确认当前裁剪预览", {"Previous", "Next", "Start", "Exit"}};
        return {"待开始裁剪", "选择形状和保留方式，开始后拖动控件定义有效域。", "Start", "开始裁剪", {"OpenRoi"}};
    }
    if (module == "Part") {
        if (s["hasPreview"].toBool()) return {"零件编辑候选待确认", "确认候选才更新正式标签和目录；高亮与编辑目标分别显示。", "Commit", "确认零件编辑", {"Discard"}};
        if(s["hasCurrentParts"].toBool() && !s["editingParts"].toArray().isEmpty()) {
            const auto action=QStringList{"Paint","Erase","Fill","Island","Grow","Split","Merge","Undo","Redo"}.contains(parameterAction) ? parameterAction : "Paint";
            return {"编辑目标已确定","核对编辑对象与作用范围，生成候选后再确认正式标签。",action,"生成零件编辑候选",{"SetState","EditSelected","OpenSurface"}};
        }
        if (s["hasCurrentParts"].toBool()) return {"当前输入的零件目录已生成", "选择唯一编辑对象，或使用同源标签继续壁厚分析；标签不替换灰度输入。", "EditSelected", "编辑选中零件", {"SetState", "OpenSurface", "OpenWall", "Start"}};
        return {"待分割当前输入", "核对材料阈值、范围及连接规则，再生成正式标签目录。", "Start", "开始零件分割", {"OpenSurface"}};
    }
    if (module == "Surface") {
        if(s["parametersChanged"].toBool())return {"参数草稿已改变，现有表面对应原参数","原结果仍保留其来源和质量；按新参数重新生成，草稿不自动成为测量结果。","Determine","按新参数重新测定",{"Preview","GlobalAutomatic"}};
        if (s["hasMeasurement"].toBool()) return {"正式测量表面已生成", "有效测量网格可进入壁厚或计量；壁厚还需同源标签，计量还需名义参考。", "OpenAlignment", "进入计量对齐", {"OpenWall", "MeasurementSelection", "Determine", "Preview"}};
        if (s["hasPreview"].toBool()) return {"表面预览可查看", "预览不自动具有测量资格；核对参数后生成正式测量表面。", "Determine", "生成正式测量表面", {"Preview", "ClearPreview"}};
        return {s["hasMesh"].toBool() ? "表面可显示，测量资格不足" : "待测定表面", "检查灰度估计与质量条件，生成可检查的预览或正式表面。", "Determine", "测定正式表面", {"Preview", "CopyIsoToPart", "CopyIsoToGap"}};
    }
    if (module == "Alignment") {
        const QStringList inspections{"InspectGeometry", "InspectSection", "EvaluateSurface", "EvaluateVoxelVolume", "ExportReport","InspectionResult","DeviationResult","VoxelVolumeResult"};
        if (inspections.contains(parameterAction)) return {parameterAction == "EvaluateVoxelVolume" ? "独立体素体积统计" : "检测参数待提交",
            "核对本次输入、选区和量值定义；计算完成、显示状态与公差结论分别报告。", parameterAction, parameterAction == "ExportReport" ? "导出所选检测报告" : "执行当前检测", {"Result", "Deactivate"}};
        if (!s["isReferenceCurrent"].toBool()) return {"待准备名义参考", "导入与当前正式测量表面匹配的名义参考；体素体积可独立执行。", "ImportReference", "导入名义参考", {"EvaluateVoxelVolume", "OpenSurface"}};
        if (!s["hasRecipe"].toBool()) return {"参考已就绪，待保存方案", "选择真实约束、基准或最佳拟合配置，并保存本次方案。", "SaveRecipe", "保存对齐方案", {"ImportReference", "EvaluateVoxelVolume"}};
        if (!s["hasResult"].toBool() || !s["isResultCurrent"].toBool()) return {"对齐方案已准备", "求解产生独立结果；应用模型姿态是后续显式动作。", "Start", "求解对齐", {"SaveRecipe", "EvaluateVoxelVolume"}};
        return {s["isApplied"].toBool() ? "对齐结果已应用" : "对齐结果已求解", "查看约束残差，再选择几何、截面或表面偏差检测。配准 RMS 不等于测量精度。", "Result", "查看对齐结果", {"Activate", "InspectGeometry", "InspectSection", "EvaluateSurface", "EvaluateVoxelVolume", "ExportReport"}};
    }
    if(module=="Wall" && s["hasResult"].toBool() && !s["isCurrent"].toBool())return {"旧输入的壁厚结果可查看","保留历史结果的面积统计；开始新测量前准备当前输入的同源标签和测量表面。","Start","测量当前输入壁厚",{"Result","OpenPart","OpenSurface"}};
    if (module == "Wall") return {s["hasResult"].toBool() ? "壁厚结果可查看" : "待准备壁厚输入",
        s["hasResult"].toBool() ? "面积分布与有效覆盖分别查看；调评价或颜色不重新测厚。" : "需要同源灰度体、标签和正式测量网格，再开始测量。",
        s["hasResult"].toBool() ? "LocateMinimum" : "Start", s["hasResult"].toBool() ? "定位最薄有效区域" : "开始壁厚测量", {"SetEvaluation", "SetDisplay", "Result", "OpenPart", "OpenSurface"}};
    if (module == "Gap") return {s["hasResult"].toBool() ? (!s["isCurrent"].toBool() ? "旧输入的孔隙结果可查看" : s["parametersChanged"].toBool() ? "算法参数已改变，结果待更新" : "孔隙结果已生成") : "待分析孔隙",
        s["hasResult"].toBool() ? "缺陷表和统计保留独立来源；列表筛选不改变算法结果。零孔隙可以是有效结果。" : "核对材料定义、ISO 与算法最小体积过滤条件。",
        s["isCurrent"].toBool() && !s["parametersChanged"].toBool() ? "Defects" : "Start", s["isCurrent"].toBool() && !s["parametersChanged"].toBool() ? "查看缺陷目录" : s["hasResult"].toBool() ? "重新分析孔隙" : "开始孔隙分析", {"NextDefectPage", "ResetDefectFilter", "SetDisplay", "Overlay", "Start"}};
    return {"功能尚未启用", "使用启用对应 Feature 的完整项目配置。", "", "不可用", {}};
}
}
