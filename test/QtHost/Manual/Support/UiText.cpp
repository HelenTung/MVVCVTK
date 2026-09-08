// 测试用途：为功能测试界面提供中文业务名称、操作和参数说明，并格式化业务流程日志。
#include "UiText.h"
#include <QHash>
#include <QJsonArray>
#include <QStringList>

namespace Manual {
QString GetActionDescription(const QString& module, const QString& action)
{
    if (action == "UseData") return "将所选已发布体数据设为当前输入，然后执行所需业务；新的发布结果按真实来源形成分支，已有结果保留。";
    if (action == "GraphInfo") return "读取所选发布修订的真实类型、来源和业务记录；该记录不修改数据。";
    if (module == "View" && action == "Visibility") return "按辅助显示作用范围更新十字线、三维切片平面和标尺；所有视图会逐个接收请求，完成后回读实际状态。";
    static const QHash<QString, QString> descriptions{
        {"Data.Load", "加载路径指向的体数据。RAW 的尺寸、间距和方向须与文件相符；可直接拖入文件填路径。"},
        {"Crop.Box", "直接启用盒裁剪。拖动主三维中的盒面或控制点调整范围，松开后生成历史节点。"},
        {"Crop.Plane", "直接启用平面裁剪。拖动平面位置或法线调整保留半空间。"},
        {"Crop.KeepInside", "保留盒内部或平面法线正半空间；直接切换，不需要再次执行。"},
        {"Crop.RemoveInside", "移除盒内部或平面法线正半空间；直接切换，不需要再次执行。"},
        {"Crop.PositionOnly", "只调整工具位置，不产生裁剪历史。"},
        {"Crop.FinishEditing", "确认当前已提交的裁剪历史并关闭工具，体数据尚未发布。"},
        {"Crop.Node", "切换到场景树选中的历史节点；后续节点保留，仍可重做。"},
        {"Crop.DeleteNode", "删除选中的预览操作，保留其余节点与顺序；受影响的当前预览会重新计算。原始数据和已物化基线不属于可删除的预览。"},
        {"Crop.ResetPreview", "将当前历史游标退回零，保留可重做节点。"},
        {"Crop.BuildResult", "从已确认历史生成独立裁剪数据；发布后可选择使用结果。"},
        {"Part.EditSelected", "将场景树选中的零件设为唯一编辑目标，并打开零件编辑页。"},
        {"Part.Highlight", "显示并高亮场景树选中的零件，同时取消上一个零件的高亮；无需勾选参数。零件透明度保持原值，体渲染中显示半透明定位标记。"},
        {"Part.ClearHighlight", "取消场景树选中零件的高亮，恢复目录颜色；不改变其他零件的选择。"},
        {"Part.SetState", "修改当前零件属性。勾选项显示该零件的实际状态；直接切换高亮请使用“高亮此零件”。"},
        {"PartEdit.Commit", "确认所选编辑候选，替换正式标签与零件目录。"},
        {"Artifact.Commit", "发布已计算的校正候选；后续算法输入由“使用校正数据”显式切换。"},
        {"Surface.OpenAlignment", "使用当前有效测量表面进入对齐页，仍需导入名义参考文件。"},
        {"Alignment.Restore", "从归档恢复对齐方案，随后需重新求解，不会自动应用旧位姿。"},
        {"Alignment.Activate", "将所选有效对齐结果应用到当前作用域。"},
        {"Alignment.SaveRecipe", "保存参考约束方案，原求解结果将失效；保存不等于完成求解。"}};
    const auto specific = descriptions.value(module + "." + action);
    if (!specific.isEmpty()) return specific;
    if (action == "SelectOutput") return "将所选已发布结果设为当前输入，后续业务使用该数据。";
    if (action == "RestoreSource") return "恢复当前派生结果对应的源数据，拒绝不匹配的旧引用。";
    if (action == "Discard") return "丢弃当前候选，保留正式结果。";
    if (action == "Cancel" || action == "Stop" || action == "Exit") return "请求停止或退出当前业务；信息栏将报告真实完成状态。";
    if (action == "Previous" || action == "Undo" || action == "Next" || action == "Redo") return "按当前对象的历史执行撤销或重做；候选型业务仍需确认候选。";
    return GetActionText(module, action) + "：直接填写本操作卡片的参数并执行；选择场景节点会带入对应对象。";
}
QString GetModuleText(const QString& module)
{
    static const QHash<QString, QString> labels{{"Data", "数据输入"}, {"View", "视图显示"},
        {"Crop", "正交裁剪"}, {"Gap", "孔隙分析"}, {"Part", "零件分割"}, {"PartEdit", "零件编辑"},
        {"Surface", "表面确定"}, {"Artifact", "伪影校正"}, {"Rotation", "模型旋转"}, {"Alignment", "计量对齐"}};
    return labels.value(module, module);
}
QString GetActionText(const QString& module, const QString& action)
{
    if (action == "UseData") return "从此数据继续";
    if (action == "GraphInfo") return "查看发布记录";
    if (module == "View" && action == "Visibility") return "应用辅助显示";
    static const QHash<QString, QString> specific{
        {"Crop.Start", "开始裁剪"}, {"Gap.Start", "开始孔隙分析"}, {"Part.Start", "开始分割"},
        {"Alignment.Start", "开始对齐"}, {"Crop.Exit", "退出裁剪编辑"}, {"Gap.Exit", "退出孔隙分析"},
        {"PartEdit.Commit", "确认编辑"}, {"Artifact.Commit", "发布校正结果"},
        {"Crop.SelectOutput", "使用裁剪结果"}, {"Artifact.SelectOutput", "使用校正数据"},
        {"Crop.Previous", "撤销上一步裁剪"}, {"Crop.Next", "恢复下一步裁剪"}, {"Crop.DeleteNode", "删除此预览节点"}, {"Crop.Node", "跳转到此节点"}};
    static const QHash<QString, QString> labels{
        {"Load", "加载体数据"}, {"Descriptor", "查看数据描述"}, {"Select", "选择当前输入"},
        {"ExportData", "导出数据"}, {"ExportSlices", "导出切片"}, {"LabelDescriptors", "查看标签描述"},
        {"ReadLabelRegion", "读取标签区域"}, {"CreateMask", "创建测试掩码"},
        {"Set", "设置视图"}, {"Cursor", "设置游标"}, {"Reset", "重置视图"}, {"State", "查看状态"},
        {"Box", "盒裁剪"}, {"Plane", "平面裁剪"}, {"Mode", "设置裁剪保留模式"}, {"Node", "切换裁剪历史节点"},
        {"ClearPolyData", "清除裁剪网格"}, {"SetPolyData", "导入裁剪网格"}, {"BuildResult", "发布裁剪结果"},
        {"RestoreSource", "恢复源数据"}, {"FinishEditing", "确认裁剪预览"}, {"ResetPreview", "撤销全部裁剪"},
        {"EditSelected", "编辑选定零件"}, {"OpenAlignment", "进入计量对齐"},
        {"KeepInside", "保留内部"}, {"RemoveInside", "移除内部"}, {"PositionOnly", "仅定位"},
        {"Overlay", "切换结果叠加显示"}, {"Stop", "停止计算"},
        {"Visibility", "设置结果可见性"}, {"Clear", "清除结果"}, {"SetState", "设置零件状态"}, {"Catalog", "查看零件目录"},
        {"Highlight", "高亮此零件"}, {"ClearHighlight", "取消此零件高亮"},
        {"Paint", "涂绘标签"}, {"Erase", "擦除标签"}, {"Fill", "填充区域"}, {"Island", "处理孤岛"},
        {"Grow", "区域生长"}, {"Split", "拆分零件"}, {"Merge", "合并零件"}, {"Undo", "撤销"}, {"Redo", "重做"},
        {"Discard", "丢弃候选"}, {"Cancel", "取消计算"},
        {"AutomaticIso50", "自动 ISO50 阈值估计"}, {"GlobalIsoPreview", "全局等值面预览"},
        {"LocalAdaptiveIso50", "局部自适应 ISO50"}, {"GradientPeak", "梯度峰值表面定位"},
        {"CopyIsoToDisplay", "将阈值复制到视图"}, {"CopyIsoToGap", "将阈值复制到孔隙分析"},
        {"CopyIsoToPart", "将阈值复制到零件分割"}, {"SamplePoints", "读取表面采样点"},
        {"Ring", "环形伪影校正"}, {"Diffusion", "扩散滤波"}, {"Combined", "环形校正与扩散滤波"},
        {"SetEnabled", "设置旋转启用状态"}, {"Rotate", "旋转模型"},
        {"ExportSequentialPlanesTemplate", "导出顺序平面对齐模板"}, {"ExportPlaneTwoHolesTemplate", "导出一面两孔对齐模板"},
        {"ExportRpsTemplate", "导出参考点系统对齐模板"}, {"ExportConstrainedBestFitTemplate", "导出约束最佳拟合模板"},
        {"ImportReference", "导入名义参考"}, {"SaveRecipe", "保存对齐方案"}, {"Activate", "应用对齐结果"},
        {"Deactivate", "停用对齐结果"}, {"ExportArchive", "导出对齐归档"}, {"Restore", "恢复对齐归档"}, {"Result", "查看对齐结果"}};
    return specific.value(module + "." + action, labels.value(action, action));
}
QString GetParameterText(const QString& key)
{
    if (key == "graphRevision") return "发布修订";
    if (key == "viewScope") return "辅助显示作用范围";
    static const QHash<QString, QString> labels{
        {"filePath", "输入文件路径"}, {"outputPath", "输出文件路径"}, {"outputDir", "输出目录"}, {"archivePath", "归档文件路径"},
        {"datasetId", "数据集标识"}, {"dimensions", "各轴体素数量"}, {"spacingLPS", "体素间距（LPS）"},
        {"originLPS", "原点坐标（LPS）"}, {"directionLPS", "方向矩阵（LPS）"}, {"sourceDigest", "源文件摘要"},
        {"evidenceKind", "数据证据类型"}, {"revision", "数据修订引用"}, {"expectedBindingRevision", "预期输入绑定版本"},
        {"viewId", "目标视图"}, {"format", "导出格式"}, {"angleDeg", "旋转角度（度）"}, {"id", "标签标识"},
        {"offset", "区域起始偏移（体素）"}, {"size", "区域尺寸（体素）"}, {"defaultValue", "掩码默认值"},
        {"boxValue", "盒内掩码值"}, {"indexBoxes", "体素索引盒列表"}, {"purpose", "掩码用途"},
        {"mode", "显示模式"}, {"iso", "显示等值阈值"}, {"opacity", "不透明度（0～1）"}, {"quality", "显示质量"},
        {"axes", "显示坐标轴"}, {"windowLevel", "窗宽与窗位"}, {"transfer", "颜色与透明度传递函数"},
        {"visibility", "辅助元素可见性"}, {"world", "游标世界坐标"}, {"axis", "作用轴"},
        {"removalMode", "裁剪保留模式"}, {"shape", "裁剪工具"}, {"nodeCount", "保留的历史节点数"}, {"plyPath", "裁剪网格路径（PLY）"},
        {"isoMode", "分析阈值方式"}, {"dataRangeRatio", "灰度范围比例"}, {"absoluteIsoValue", "绝对灰度阈值"},
        {"backgroundMean", "背景灰度均值"}, {"materialMean", "材料灰度均值"}, {"filter", "启用孔隙过滤"},
        {"minVolumeMM3", "最小孔隙体积（mm³）"}, {"threshold", "分割阈值"}, {"minPartVoxels", "最小零件体素数"},
        {"isVisible", "显示结果"}, {"isSelected", "选中零件"}, {"isReviewed", "已复核"}, {"colorRGBA", "颜色（RGBA）"},
        {"target", "目标零件"}, {"name", "零件名称"}, {"expectedCatalogRevision", "预期零件目录版本"},
        {"expectedLabelMap", "预期标签图修订"}, {"extent", "编辑索引范围"}, {"roiMask", "作用区域掩码"},
        {"protectionMask", "保护掩码"}, {"protectedParts", "受保护零件列表"}, {"sourcePointsMM", "源坐标笔刷点（mm）"},
        {"radiusMM", "笔刷半径（mm）"}, {"slice", "笔刷切片平面"}, {"overwriteParts", "允许覆盖的零件"},
        {"isBackgroundAllowed", "允许修改背景"}, {"seed", "种子位置（体素）"}, {"seeds", "种子列表"},
        {"minIslandVoxels", "孤岛体素数阈值"}, {"minimum", "生长灰度下限"}, {"maximum", "生长灰度上限"},
        {"barriers", "拆分屏障"}, {"parts", "待合并零件列表"}, {"previewId", "编辑候选编号"},
        {"componentSelection", "连通分量选择"}, {"initialIsoValue", "初始等值阈值"}, {"seedModelPoint", "种子点（模型坐标）"},
        {"roiModelBounds", "作用范围（模型坐标）"}, {"profileHalfLengthModel", "剖面半长（模型单位）"},
        {"profileSampleStepModel", "剖面采样间距（模型单位）"}, {"maximumOffsetModel", "最大定位偏移（模型单位）"},
        {"profileSmoothingSigmaModel", "剖面平滑尺度（模型单位）"}, {"minimumObjectVoxels", "最小对象体素数"},
        {"minimumContrast", "最小灰度对比度"}, {"targetRequestId", "目标计算请求编号"}, {"maxPoints", "最大采样点数"},
        {"source", "源数据修订"}, {"processingMask", "处理区域掩码"}, {"materialMask", "材料统计掩码"},
        {"timeoutMs", "计算超时（毫秒）"}, {"ring", "环形校正参数"}, {"diffusion", "扩散滤波参数"}, {"requestId", "计算请求编号"},
        {"isEnabled", "启用旋转"}, {"worldAxis", "世界坐标旋转轴"}, {"worldCenter", "世界坐标旋转中心"},
        {"reference", "名义参考与约束模板"}, {"mesh", "测量网格修订"}, {"recipe", "对齐方案"},
        {"initialPoses", "初始位姿矩阵列表"}, {"result", "对齐结果修订"},
        {"imageIndex", "种子体素索引"}, {"origin", "原点"}, {"normal", "法向"}, {"thicknessMM", "厚度（mm）"},
        {"colorNodes", "颜色节点"}, {"opacityNodes", "透明度节点"}, {"planes", "三维切片平面"}, {"crosshair", "十字线"}, {"ruler", "标尺"},
        {"setHigh", "零件集标识高位"}, {"setLow", "零件集标识低位"}, {"objectHigh", "零件标识高位"}, {"objectLow", "零件标识低位"}, {"resultRevision", "结果修订"},
        {"centerIndex", "截面中心索引"}, {"threshMin", "灰度下限"}, {"threshMax", "灰度上限"}, {"angularMin", "最小角度"}, {"ringWidth", "环宽"},
        {"strength", "校正强度"}, {"maxCorrection", "最大校正量"}, {"iterations", "迭代次数"}, {"factor", "扩散系数"}, {"slabDepth", "分块层数"},
        {"operationIndex", "裁剪节点标识"}, {"targetFrameId", "目标坐标系"}, {"sourceFrameId", "源坐标系"}, {"coordinateFrame", "坐标约定"}, {"scope", "测量范围标识"},
        {"unit", "长度单位"}, {"method", "对齐方法"}, {"exactMesh", "指定网格修订"}, {"nominalData", "名义数据修订"}, {"provenance", "参考来源说明"},
        {"geometries", "测量几何"}, {"constraints", "几何约束"}, {"fitPairs", "拟合对应点"}, {"region", "拟合选区"}, {"pinnedMesh", "选区网格修订"},
        {"vertexIds", "顶点编号"}, {"vertexId", "顶点编号"}, {"targetBounds", "目标范围"}, {"targetDirection", "目标方向"}, {"targetNormal", "目标法向"},
        {"radiusRange", "半径范围"}, {"maxFitRms", "拟合 RMS 上限"}, {"minCoverage", "最低覆盖率"}, {"minDirectionCosine", "方向余弦下限"},
        {"kind", "类型"}, {"association", "关联方法"}, {"geometryIndex", "几何编号"}, {"nominalPoint", "名义点坐标"}, {"priority", "优先级"},
        {"weight", "权重"}, {"tolerance", "容差"}, {"sectionPlane", "截面平面编号"}, {"datumCount", "基准数量"}, {"datumOffsets", "基准偏移"},
        {"iterationLimit", "迭代次数上限"}, {"requiresMeasurementQuality", "要求测量质量"}, {"minPairNormalCosine", "对应点法向余弦下限"},
        {"lengthScale", "长度尺度"}, {"rankTolerance", "秩判断容差"}, {"conditionLimit", "条件数上限"}, {"solveTolerance", "求解容差"},
        {"maxAlignmentRms", "对齐 RMS 上限"}, {"maxPairDistance", "对应点距离上限"}, {"minPairCoverage", "对应点覆盖率下限"},
        {"huberDistance", "稳健拟合距离"}, {"minSupportRatio", "支持比例下限"}, {"maxLocalizationSigma", "定位标准差上限"}, {"maxSurfaceFitResidual", "表面拟合残差上限"}};
    return labels.value(key, key);
}
QString GetParameterHelp(const QString& key)
{
    static const QHash<QString, QString> help{
        {"centerIndex", "未提供时使用当前源数据的截面中心；显式提供可逐项输入两个索引。"},
        {"initialIsoValue", "未提供时由算法估计当前输入的初始阈值；显式值使用原始灰度单位。"},
        {"minPartVoxels", "小于此体素数的连通域不生成零件。手动测试默认 1000 以过滤真实 CT 的微小噪声；需保留更小零件时可降低。"},
        {"dimensions", "分别填写 X、Y、Z 轴体素数量，必须与原始文件匹配。"},
        {"spacingLPS", "依次填写 X、Y、Z 轴间距；单位必须与数据来源及后续测量一致。"},
        {"originLPS", "分别填写 LPS 原点的 X、Y、Z 坐标，不需要手工翻转为 RAS。"},
        {"directionLPS", "按行、列逐格填写 3×3 方向矩阵。"},
        {"sourceDigest", "可填写已核实的源文件哈希；留空表示本次未提供摘要。"},
        {"axis", "0＝X 轴，1＝Y 轴，2＝Z 轴；游标操作中 -1 表示不限定单轴。"},
        {"worldAxis", "分别填写旋转轴方向的 X、Y、Z 分量，不能全部为零。"},
        {"windowLevel", "分别填写窗宽和窗位；不勾选“指定”时不修改。"},
        {"transfer", "颜色节点逐行填写灰度及红、绿、蓝分量；透明度节点逐行填写灰度和透明度。"},
        {"visibility", "分别指定是否修改三维切片平面、十字线和标尺，再设置启用状态。"},
        {"indexBoxes", "每行定义一个索引盒，分别填写三个轴的最小与最大索引。"},
        {"extent", "分别填写各轴最小与最大索引；不勾选“指定”时不额外限制。"},
        {"roiModelBounds", "分别填写各轴最小与最大模型坐标。"},
        {"target", "selected 表示当前选中零件；也可填零件目录返回的完整绑定对象。"},
        {"parts", "填写零件目录中的完整绑定对象列表，包含标签图修订、标签编号与稳定实例编号。"},
        {"slice", "分别填写源坐标原点、法向和厚度（mm）；不指定时使用三维笔刷。"},
        {"seeds", "每行填写一个种子的 X、Y、Z 索引；拆分时同时填写新标签编号。"},
        {"barriers", "每行填写一个屏障的位置及作用轴，轴为 0、1 或 2。"},
        {"ring", "axis＝网格轴；centerIndex＝其余两轴截面中心索引；threshMin/threshMax＝灰度范围；threshold＝检测阈值；angularMin＝最小角度；ringWidth＝环宽；mode＝Wrap（环绕）/Reflect（反射）；strength＝强度；maxCorrection＝最大校正量。"},
        {"diffusion", "iterations＝迭代次数；threshold＝灰度阈值；factor＝扩散系数；slabDepth＝分块层数。"},
        {"initialPoses", "每组为逐格填写的 4×4 变换矩阵，可添加多组初始位姿。"},
        {"reference", "模板必须填写实际测量选区、名义点/约束、单位和来源；导出后再导入名义参考。"},
        {"recipe", "不勾选“指定”时使用已导入参考中的方案；勾选后可分别编辑几何、约束和对应点。"},
        {"source", "current 表示当前输入；显式修订必须使用实际存在的数据引用。"},
        {"mesh", "surface 表示最近生成且属于当前输入的表面网格。"},
        {"result", "current 表示本页最近保存的结果修订。"},
        {"targetRequestId", "十进制字符串；0 表示由功能接口选择当前请求。"},
        {"requestId", "current 表示当前计算请求；显式编号使用十进制字符串。"},
        {"previewId", "current 表示当前候选；确认前请核对候选结果。"},
        {"expectedBindingRevision", "current 表示使用当前绑定版本；显式版本用于检查过期输入冲突。"},
        {"expectedCatalogRevision", "current 表示使用当前零件目录版本；显式版本使用十进制字符串。"},
        {"expectedLabelMap", "current 表示使用当前标签图；显式引用使用 实体编号:修订号。"}};
    return help.value(key, "按字段填写；可选项勾选“指定”后生效，未勾选时使用默认值或保持原值。");
}
ParameterChoices GetParameterChoices(const QString& module, const QString& key)
{
    if (module == "View" && key == "viewScope") {
        auto choices = GetParameterChoices(module, "viewId");
        choices.prepend({"slices", "三个切片"}); choices.prepend({"all", "所有视图"}); return choices;
    }
    if (module == "Artifact" && key == "mode") return {{"Wrap", "环绕"}, {"Reflect", "反射"}};
    if (key == "unit") return {{"ModelUnit", "模型单位"}, {"Millimeter", "毫米"}, {"Meter", "米"}};
    if (key == "method") return {{"SequentialPlanes", "依次拟合平面"}, {"PlaneTwoHoles", "一面两孔"}, {"Rps", "参考点系统"}, {"ConstrainedBestFit", "约束最佳拟合"}};
    if (key == "association") return {{"LeastSquares", "最小二乘"}, {"SequentialLeastSquares", "顺序最小二乘"}};
    if (module == "Crop" && key == "shape") return {{"Box", "盒裁剪"}, {"Plane", "平面裁剪"}};
    if (key == "viewId") return {{"primary-3d", "主三维"}, {"composite-volume", "体渲染"},
        {"slice-top-down", "上下切片"}, {"slice-front-back", "前后切片"}, {"slice-left-right", "左右切片"}};
    if (key == "componentSelection") return {{"Largest", "最大连通分量"}, {"Seeded", "种子所在分量"}, {"All", "全部分量"}};
    if (key == "isoMode") return {{"AbsoluteValue", "绝对灰度阈值"}, {"DataRangeRatio", "灰度范围比例"}};
    if (key == "removalMode") return {{"None", "不移除"}, {"KeepInside", "保留内部"}, {"RemoveInside", "移除内部"}};
    if (key == "format") return {{"Raw", "原始体数据（RAW）"}, {"Ply", "多边形网格（PLY）"}, {"Stl", "三角网格（STL）"}, {"Obj", "网格模型（OBJ）"}};
    if (key == "evidenceKind") return {{"real-data", "真实数据"}, {"synthetic-regression", "合成回归数据"}};
    if (module == "View" && key == "quality") return {{"Auto", "自动"}, {"Low", "低"}, {"High", "高"}, {"XHigh", "超高"}, {"Ultra", "原始分辨率"}};
    if (module == "View" && key == "mode") return {{"Volume", "体渲染"}, {"IsoSurface", "等值面"},
        {"CompositeVolume", "复合体渲染"}, {"CompositeIsoSurface", "复合等值面"},
        {"SliceTopDown", "上下切片"}, {"SliceFrontBack", "前后切片"}, {"SliceLeftRight", "左右切片"}};
    return {};
}
QStringList GetBoundParameters(const QString& module, const QString& action)
{
    if (action == "UseData" || action == "GraphInfo") return {"graphRevision"};
    if (module == "Crop") return {"nodeCount", "operationIndex"};
    if (module == "PartEdit") {
        QStringList fields{"target", "expectedLabelMap", "expectedCatalogRevision", "previewId"};
        if (action == "Merge") fields.append("parts");
        return fields;
    }
    if (module == "Part") return {"target"};
    if (module == "Artifact") return {"source", "requestId"};
    if (module == "Surface") return {"targetRequestId"};
    if (module == "Alignment") return {"source", "mesh", "result", "targetRequestId"};
    if (module == "Data") return {"expectedBindingRevision"};
    return {};
}
QString GetFlowText(const QJsonObject& record)
{
    const auto module = record["module"].toString();
    const auto action = record["action"].toString();
    const auto status = record["status"].toString();
    static const QHash<QString, QString> statuses{
        {"Sending", "提交操作"}, {"AcceptedPending", "已接纳，等待执行结果"}, {"Accepted", "请求已接纳"},
        {"Succeeded", "操作成功"}, {"Failed", "操作失败"}, {"Rejected", "操作被拒绝"}, {"InvalidInput", "参数无效"},
        {"Unchanged", "无标签变化"},
        {"Observed", "查询完成"}, {"Published", "结果已发布"}, {"MaskPublished", "掩码已发布"},
        {"PreviewReady", "编辑候选已就绪，请确认编辑或丢弃候选"}, {"Ready", "校正候选已就绪，请发布校正结果"},
        {"Discarded", "候选已丢弃"}, {"Cancelled", "已取消"}, {"CancelledOrInvalidated", "已取消或输入已失效"},
        {"Stopped", "会话已停止"}, {"Exited", "已退出"}, {"SourceChanged", "源数据已变化"}, {"PreviewConfirmed", "裁剪预览已确认"},
        {"SucceededWithDisplayFailure", "业务数据已完成，但显示失败"}, {"ParametersCopied", "参数已复制"},
        {"TemplateExported", "参考模板已导出，请填写测量选区与名义约束"},
        {"ReferencePublished", "名义参考已导入，下一步保存对齐方案"}, {"ArchiveExported", "对齐归档已导出"},
        {"FullyDetermined", "对齐操作完成，约束充分"}, {"Underconstrained", "约束不足"}, {"Degenerate", "几何退化"},
        {"Conflicting", "约束或版本冲突"}, {"Stale", "结果已过期"}};
    QStringList parts{"#" + record["operationId"].toString(), GetModuleText(module) + " · " + GetActionText(module, action)};
    if (record["duplicateCompletion"].toBool()) parts << "异常：收到重复完成通知";
    else if (!record["isTerminal"].toBool() && record.contains("progress"))
        parts << QString("执行进度 %1%").arg(record["progress"].toObject()["percent"].toInt());
    else if (module == "Alignment" && status == "FullyDetermined" && action != "Start") {
        static const QHash<QString, QString> completed{{"SaveRecipe", "对齐方案已保存"}, {"Activate", "对齐结果已应用"},
            {"Deactivate", "对齐结果已停用"}, {"Restore", "归档方案已恢复，下一步重新求解"}, {"Visibility", "结果可见性已设置"}};
        parts << completed.value(action, "对齐操作完成");
    } else parts << statuses.value(status, status);
    const auto result = record["result"].toObject();
    const bool thresholdOnly = module == "Surface" && action == "AutomaticIso50";
    if (thresholdOnly && status == "Succeeded") parts << "仅估计阈值，未生成网格：ISO=" + QString::number(result["isoEstimate"].toObject()["iso"].toDouble(), 'g', 10);
    for (const auto* key : {"message", "error", "hint"}) {
        if (result[key].isString() && !result[key].toString().isEmpty()) parts << result[key].toString().simplified().left(300);
    }
    if (record["isTerminal"].toBool()) {
        for (const auto& field : QVector<std::pair<QString, QString>>{{"partCount", "零件数"}, {"pointCount", "网格点数"},
            {"points", "网格点数"}, {"objectCount", "对象数"}, {"voidVolumeMM3", "孔隙体积（mm³）"}, {"porosityRatio", "孔隙率"},
            {"previewId", "候选编号"}, {"errorCode", "错误码"}, {"failureReason", "失败原因码"}}) {
            const auto value = result[field.first];
            if (thresholdOnly && (field.first == "pointCount" || field.first == "points" || field.first == "objectCount")) continue;
            if (value.isString() && !value.toString().isEmpty()) parts << field.second + "=" + value.toString();
            else if (value.isDouble() && ((!field.first.contains("Code") && field.first != "failureReason") || value.toDouble() != 0))
                parts << field.second + "=" + QString::number(value.toDouble(), 'g', 8);
        }
        if (module == "Artifact" && status == "Published") parts << "可选择“使用校正数据”切换当前输入";
        if (module == "Crop" && status == "Published") parts << "可选择“使用裁剪结果”切换当前输入";
        const auto current = record["current"].toObject();
        if (record["isSourceChanged"].toBool()) parts << "当前输入已更新：" + current["datasetId"].toString();
        parts << "耗时 " + record["elapsedMs"].toString() + " 毫秒";
    } else if (status == "Sending") {
        const auto params = record["parameters"].toObject();
        if (params["filePath"].isString()) parts << "文件：" + params["filePath"].toString().simplified();
        else if (record["source"].toObject()["available"].toBool())
            parts << "输入：" + record["source"].toObject()["datasetId"].toString();
    }
    return parts.join(" ｜ ");
}
}
