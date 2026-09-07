#include "FeatureTestControls.h"
#include "Data/DataPayloads.h"
#include "Host/CropHostFeature.h"
#include "Host/VtkAppHostSession.h"
#include "Host/Types/HostRequestTypes.h"
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
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
constexpr auto controlId = "main.feature-tools";
constexpr auto primaryView = "primary-3d";
constexpr std::array<const char*, 7> editNames{"涂绘", "擦除", "填充", "孤岛处理", "区域生长", "拆分", "合并"};
constexpr std::array<const char*, 3> artifactNames{"环形伪影校正", "扩散滤波", "环形伪影校正与扩散滤波"};
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
const char* ArtifactErrorText(const ArtifactError error) {
    switch (error) {
    case ArtifactError::None: return "无错误";
    case ArtifactError::Unavailable: return "功能不可用";
    case ArtifactError::WrongThread: return "调用线程不正确";
    case ArtifactError::Busy: return "已有任务或结果正在处理";
    case ArtifactError::InvalidRequest: return "参数无效";
    case ArtifactError::InvalidData: return "输入或结果数据无效";
    case ArtifactError::UnsupportedType: return "不支持该体素类型";
    case ArtifactError::SourceChanged: return "处理期间输入数据已切换";
    case ArtifactError::TooLarge: return "内存预算不足或分配失败";
    case ArtifactError::Cancelled: return "已取消";
    case ArtifactError::TimedOut: return "超过处理时间上限";
    case ArtifactError::InsufficientEvidence: return "缺少处理所需的有效上下文";
    case ArtifactError::CommitFailed: return "发布结果失败";
    case ArtifactError::UnsupportedGeometry: return "不支持该网格几何";
    case ArtifactError::UnsupportedValidity: return "输入包含算法不支持的无效体素";
    case ArtifactError::KernelFailed: return "滤波计算失败";
    }
    return "未知错误";
}
const char* ArtifactStageText(const unsigned int progress) {
    if (progress < 10) return "检查并准备输入";
    if (progress < 40) return "环形校正";
    if (progress < 75) return "扩散滤波";
    if (progress < 80) return "处理写回范围";
    if (progress < 95) return "统计质量指标";
    return "冻结候选结果";
}
#endif
std::string Ref(const DataRevisionRef& ref) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    for (const auto byte : ref.entityId.bytes) { value += digits[byte >> 4]; value += digits[byte & 15]; }
    return value + ":" + std::to_string(ref.generation);
}
}

void PrintFeatureTestHelp() {
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    std::cout << "\n【零件标签编辑：计算候选与确认分两步】\n"
        << "  前置：先按 B 并等待成功，再按 N 选择目标零件；未选择时使用第一个零件。\n"
        << "  F6 / Shift+F6：下一种/上一种编辑方式；启动时默认“合并”。切换后窗口标题显示当前方式。\n"
        << "  循环顺序：涂绘 → 擦除 → 填充 → 孤岛处理 → 区域生长 → 拆分 → 合并 → 涂绘。\n"
        << "  F7：计算候选，完成后在终端输出候选零件数、体素数；正式显示此时尚未替换。\n"
        << "  Ctrl+F7：确认当前候选并更新正式结果；必须先等“候选结果已就绪”。\n"
        << "  Alt+F7：计算中请求取消，候选就绪后丢弃；保留确认前的正式结果。\n"
        << "  F8：准备撤销候选；Shift+F8：准备重做候选；两者都要等待完成后再按 Ctrl+F7 确认。\n"
        << "  例：B → 等待成功 → N → F7 → 等待候选 → Ctrl+F7 → F8 → 等待候选 → Ctrl+F7。\n"
        << "  本工具使用以下固定操作预设，种子由所选零件的真实标签自动查找：\n"
        << "    涂绘/擦除：在自动找到的一个零件体素处使用球形笔刷；此入口不采集鼠标绘制轨迹。\n"
        << "    填充：从种子旁的一个背景体素开始，填充种子附近的有限盒形区域。\n"
        << "    孤岛处理：处理所选零件中小于体素数量阈值的连通区域；阈值默认 2。\n"
        << "    区域生长：从一个种子在有限盒形区域内生长，灰度下限取 A 的阈值，上限取输入最大值。\n"
        << "    拆分：从所选零件包围范围的正向和反向各找一个属于该零件的体素，作为两个种子。\n"
        << "    合并：合并“所选零件”和“目录中的下一个零件”；在末尾时，下一个回到第一个。\n"
        << "  涂绘/擦除半径默认 1.5 倍最大体素间距；可用 --edit-radius-mm 调整。\n"
        << "  填充/生长的盒形范围由同一半径计算，每轴从种子向两侧最多扩展 16 个体素。\n"
        << "  若提示种子搜索超限或没有相邻背景，改选合适零件；连续按 F7 不会改变这些预设。\n";
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    std::cout << "\n【表面网格】\n"
        << "  前置：加载当前图像，建议先按 U 并等待完成。K 使用 A 窗口当前阈值。\n"
        << "  K：全局等值面预览；Shift+K：局部自适应 ISO50；Ctrl+Shift+K：梯度峰值测量网格。\n"
        << "  “局部自适应”是算法方式；此快捷键仍使用当前输入，没有另外指定鼠标选框范围。\n"
        << "  Alt+K：请求取消；成功后终端显示网格点数，Ctrl+K 可查看网格版本。\n"
        << "  K 的“预览网格”成功后即成为表面结果，无需使用零件编辑的 Ctrl+F7 确认。\n"
        << "  原 F12 / Shift+F12 / Alt+F12 / Ctrl+F12 保留兼容；Visual Studio 调试时请使用 K 组合。\n"
        << "  Windows 会将实体 F12 用于调试中断，可能停在 ntdll 并提示缺少 ntdll.pdb；这是系统符号提示。\n"
        << "  若在这种中断处暂停，回到 VS 按 F5 继续，再激活 main 视图使用 K；其他异常应检查调用堆栈。\n";
#endif
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    std::cout << "\n【伪影校正：准备 → 发布 → 选择输入】\n"
        << "  F9 / Shift+F9：下一种/上一种模式；顺序为环形校正 → 扩散滤波 → 两者组合，默认环形校正。\n"
        << "  F10：读取当前输入并计算候选；等待进度完成及“候选结果已就绪”。\n"
        << "  运行中标题显示当前阶段、总体进度和耗时；混合模式依次进行环形校正、扩散滤波、质量统计。\n"
        << "  Ctrl+F10：将候选发布为正式校正体数据；此步骤保留当前显示输入。\n"
        << "  Shift+F10：发布成功后，将校正体设为当前输入，同时影响显示和之后启动的算法。\n"
        << "  Ctrl+Shift+F10：重新选择最近一次成功接纳的 F10 请求所记录的输入版本。\n"
        << "  Alt+F10：运行中请求取消，候选就绪后丢弃；已发布的数据仍可能由历史记录保留。\n"
        << "  例：F9 选模式 → F10 → 等待候选 → Ctrl+F10 → Shift+F10 → 查看校正体。\n"
        << "  环形中心默认取垂直于处理轴的截面中点；强度、环宽、中心及扩散迭代数见下方参数。\n"
        << "  处理中的输入若被切换，旧候选可能失效；请对新输入重新按 F10 计算。\n";
#endif
#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT)
    std::cout << "\n【对齐验证】\n"
        << "  前置：先按 K 并等待生成当前输入的有效网格；切换输入后要重新提取网格。\n"
        << "  F11：参考点系统对齐验证；Shift+F11：最佳拟合对齐验证。\n"
        << "  Ctrl+F11：隐藏/显示对齐结果；Alt+F11：请求取消对齐任务；Ctrl+Shift+F11：输出报告数值。\n"
        << "  此入口从当前网格采样，并生成绕 Z 轴旋转 10 度、平移 (2,-3,4) 的内置参考。\n"
        << "  结果用于核对已知几何变换的恢复，单位随模型；它不代表另一个实测模型的配准精度。\n";
#endif
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    std::cout << "\n【多功能一起使用时】\n"
        << "  裁切生效时旧零件覆盖层会临时隐藏，避免完整零件遮住裁切画面；正式标签仍保留。\n"
        << "  退出裁切控件会保留裁切效果；当前可编辑裁切已撤销且控件退出后，才恢复原可见偏好。\n"
        << "  切换到裁切或校正数据后，旧零件/网格结果可能过期；先 U，再按 B/K 重新生成。\n"
        << "  F7、F10、K 会先结束裁切控件编辑；仅退出控件不会把裁切预览变成新的算法输入。\n";
#endif
    const FeatureTestOptions defaults;
    std::cout << "\n【状态、内存与等待】\n"
        << "  Ctrl+K：输出零件、表面、伪影和对齐的状态、请求编号及结果版本。\n"
        << "  候选就绪后再确认；取消请求发出后也需等待任务停止，旧正式结果才会稳定保留。\n"
        << "  工具预算分别约束零件、表面、伪影任务；它不是进程总内存上限，也不约束孔隙分析。\n"
        << "  加载缓存、历史结果和多个并行任务会叠加。隐藏覆盖层通常不会释放整卷标签。\n"
        << "  1536×1536×1536 的一份 32 位体数据就是 13.5 GiB，算法还需要输出和工作缓冲。\n"
        << "\n【真实数据启动参数：使用 --参数=值 的形式】\n"
        << "  --input=路径：单分量、32 位浮点原始体数据文件。默认路径：" << defaults.inputPath << '\n'
        << "  --dimensions=X,Y,Z：与文件匹配的三个正整数；默认 "
        << defaults.dimensions[0] << ',' << defaults.dimensions[1] << ',' << defaults.dimensions[2] << "。\n"
        << "  真实输入必须显式提供 --dimensions、--spacing=X,Y,Z、--origin=X,Y,Z（毫米）。\n"
        << "  --direction=九个逗号分隔值：行主序3x3；--input-frame=LPS --input-unit=mm。\n"
        << "  --input-format=float32-le-xfastest：仅支持单分量float32、小端、X最快、无头无padding的RAW。\n"
        << "  --dataset-id=来源标识；--input-digest=64位SHA256（真实审计必填，由仓内运行脚本核验）。\n"
        << "  RAW 输入extent从零开始；已有裁切数据应提供对应的新origin，不能只裁数组。\n"
        << "  路径含空格时，将整个参数放入双引号，例如 \"--input=F:/CT data/scan.raw\"。\n"
        << "  文件名和尺寸不能证明采集几何；缺少上述信息时真实加载拒绝，不沿用旧样本间距。\n"
        << "  --tool-budget-mib=整数：16..131072 MiB；默认取启动时可用物理内存一半，上限 64 GiB。\n"
        << "    例如 --tool-budget-mib=49152 表示 48 GiB；启动时终端会打印实际配置的预算。\n"
        << "  --tool-timeout-ms=整数：零件编辑和伪影处理的超时，范围 1..3600000，默认 "
        << defaults.timeoutMs << " 毫秒（5 分钟）；不控制 B 分割、K 网格或 G 孔隙任务。\n"
        << "  --edit-radius-mm=正数：笔刷半径，也用于计算填充/生长的邻域；默认随体素间距计算。\n"
        << "  --edit-island-voxels=整数：孤岛体素数量阈值，1..1000000000，默认 " << defaults.islandVoxels << "。\n"
        << "  --artifact-axis=0|1|2：沿 X/Y/Z 轴逐截面处理环形伪影，默认 " << defaults.ringAxis << "（Z）。\n"
        << "  --artifact-center=a,b：截面内其余两轴的中心索引，按 X/Y/Z 剩余轴顺序填写；不是毫米。\n"
        << "    例如 axis=2 时 a,b 分别为 X/Y 索引；省略时自动取截面中点。\n"
        << "  --artifact-ring-width=整数：环宽参数，1..64，默认 " << defaults.ringWidth << "。\n"
        << "  --artifact-strength=数值：环形校正强度，0..1，默认 " << defaults.ringStrength << "。\n"
        << "  --artifact-iterations=整数：扩散滤波迭代次数，1..16，默认 " << defaults.diffusionIterations << "。\n"
        << "\n【其他运行入口】\n"
        << "  --help：输出本帮助后退出；--part-picking：启用鼠标点击选件。\n"
        << "  --real-audit：使用 --input 指定的真实数据自动执行 U、B，并记录主线程响应时间。\n"
        << "  --demo：使用内置小体数据交互演示；--demo-audit：使用内置数据自动验证基础操作。\n"
        << "  --feature-audit：使用内置数据自动验证扩展工具。测试真实数据时请勿添加这三个演示选项。\n"
        << "==================================\n" << std::flush;
}

class FeatureTestControls::Impl final {
public:
    Impl(VtkAppHostSession& value, FeatureTestBindings features, FeatureTestOptions settings, HostViewTargets targets)
        : session(value), bindings(std::move(features)), options(std::move(settings)), views(std::move(targets)) {}
    void Status(const std::string& text) {
        std::cout << "[功能工具] " << text << '\n' << std::flush;
        if (context.host) (void)context.host->SetViewStatus({primaryView}, text);
    }
    bool Fail(const std::string& text) { failure = text; Status(text); return false; }
    bool EndCrop() {
        const auto crop = bindings.crop.lock();
        if (!crop || !crop->GetState().isActive) return true;
        CropHostRequest exit; exit.action = CropHostAction::Exit;
        return crop->SendRequest(exit);
    }
    bool Dispatch(int key, bool ctrl, bool alt, bool shift);
    InteractionResult OnInput(const InteractionEvent& event) {
        if (event.eventKind == InteractionEventKind::Cancel) { down.fill(false); return {}; }
        // Windows 将 F12 保留给调试器；K 及其修饰键复用同一组表面动作。
        const bool isSurfaceAlias = event.keyCode == 'k' || event.keyCode == 'K'
            || event.keySym == "k" || event.keySym == "K";
        int key = isSurfaceAlias ? 12 : 0;
        for (int candidate = 6; candidate <= 12; ++candidate)
            if (event.keySym == "F" + std::to_string(candidate)) key = candidate;
        if (key == 0) return {};
        // 别名使用独立的按下状态，避免 K 与 F12 的释放事件互相干扰。
        auto& pressed = isSurfaceAlias ? down.back() : down[static_cast<std::size_t>(key - 6)];
        if (event.eventKind == InteractionEventKind::KeyRelease) {
            const bool handled = std::exchange(pressed, false);
            return handled ? InteractionResult{true, true, true, InteractionFailureReason::None} : InteractionResult{};
        }
        if (event.eventKind == InteractionEventKind::TextInput) return {true, true, true, InteractionFailureReason::None};
        if (event.eventKind != InteractionEventKind::KeyPress) return {};
        if (pressed) return {true, true, true, InteractionFailureReason::None};
        pressed = true;
        failure.clear();
        bool succeeded = false;
        try { succeeded = Dispatch(key, event.isCtrlDown, event.isAltDown, event.isShiftDown); }
        catch (const std::exception& error) { succeeded = Fail(error.what()); }
        catch (...) { succeeded = Fail("工具请求失败"); }
        return {true, true, succeeded, succeeded ? InteractionFailureReason::None : InteractionFailureReason::StateRejected};
    }
    void Report();
    bool PreparePart(int mode);
    bool ConfirmPart();
    bool CancelPart();
    bool PrepareArtifact();
    bool ArtifactActionRequest(int action);
    bool SelectArtifact(bool restore);
    bool StartSurface(bool local, bool gradient = false);
    bool StartAlignment(bool bestFit);
    bool AlignmentActionRequest(int action);
    void Tick();
    void UpdateCropPartVisibility();
    VtkAppHostSession& session;
    FeatureTestBindings bindings;
    FeatureTestOptions options;
    HostViewTargets views;
    HostFeatureContext context;
    std::weak_ptr<FeatureTestControls> owner;
    std::array<bool, 8> down{}; // F6..F12，以及 K。
    std::string failure;
    int editMode = 6;
    int artifactMode = 0;
    int artifactProgress = -1;
    int artifactStatus = -1;
    int artifactRunMode = 0;
    std::int64_t artifactElapsedSeconds = -1;
    std::chrono::steady_clock::time_point artifactStarted{};
    std::uint64_t partCompletions = 0;
    std::uint64_t alignmentCompletions = 0;
    bool alignmentMatched = false;
    std::optional<DataRevisionRef> artifactInput;
    std::optional<DataRevisionRef> artifactPublished;
    std::optional<bool> partVisibilityBeforeCrop;
#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT)
    AlignmentMatrix expectedTransform = alignmentIdentity;
    std::optional<AlignmentResult> lastAlignment;
#endif
};

bool FeatureTestControls::Impl::Dispatch(const int key, const bool ctrl, const bool alt, const bool shift) {
    if (key == 12 && ctrl && shift && !alt) return StartSurface(true, true);
    if (key == 12 && ctrl && !alt && !shift) { Report(); return true; }
    if (key == 6 && !ctrl && !alt) {
        editMode = (editMode + (shift ? 6 : 1)) % 7;
        Status(std::string("标签编辑：") + editNames[editMode] + " | F7 预览，Ctrl+F7 确认"); return true;
    }
    if (key == 7 && !shift) {
        if (ctrl && !alt) return ConfirmPart();
        if (alt && !ctrl) return CancelPart();
        if (!alt && !ctrl) return PreparePart(editMode);
    }
    if (key == 8 && !ctrl && !alt) return PreparePart(shift ? 8 : 7);
    if (key == 9 && !ctrl && !alt) {
        artifactMode = (artifactMode + (shift ? 2 : 1)) % 3;
        Status(std::string("伪影处理模式：") + artifactNames[artifactMode] + " | F10 准备候选结果"); return true;
    }
    if (key == 10) {
        if (shift && !alt) return SelectArtifact(ctrl);
        if (!shift && ctrl && !alt) return ArtifactActionRequest(0);
        if (!shift && alt && !ctrl) return ArtifactActionRequest(1);
        if (!ctrl && !alt && !shift) return PrepareArtifact();
    }
    if (key == 11) {
        if (ctrl && shift && !alt) { Report(); return true; }
        if (ctrl && !shift && !alt) return AlignmentActionRequest(0);
        if (alt && !ctrl && !shift) return AlignmentActionRequest(1);
        if (!ctrl && !alt) return StartAlignment(shift);
    }
    if (key == 12 && !ctrl) {
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        if (alt && !shift) {
            const auto feature = bindings.surface.lock();
            SurfaceDeterminationRequest request; request.action = SurfaceDeterminationAction::Stop;
            return feature && feature->SendRequest(request).status == SurfaceAdmissionStatus::Accepted;
        }
#endif
        if (!alt) return StartSurface(shift);
    }
    return Fail("不支持此工具快捷键；请按 F1 查看帮助");
}

#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
namespace {
using Voxel = std::array<int, 3>;
std::string PartEditResultText(const PartSegmentationResult& result) {
    if (result.status == PartResultStatus::PreviewReady) return "候选结果已就绪，请按 Ctrl+F7 确认";
    if (result.status == PartResultStatus::Succeeded) return "已确认，正式结果已更新";
    if (result.status == PartResultStatus::SucceededWithDisplayFailure) return "数据已更新，但显示失败";
    switch (result.failureReason) {
    case PartFailureReason::None: return "编辑已结束";
    case PartFailureReason::InvalidSource: return "输入数据不可用";
    case PartFailureReason::InvalidGeometry: return "数据网格或几何信息无效";
    case PartFailureReason::UnsupportedScalar: return "不支持此体素数值类型";
    case PartFailureReason::BudgetExceeded: return "编辑工作区或历史结果超出内存预算";
    case PartFailureReason::Cancelled: return "编辑已取消";
    case PartFailureReason::SourceChanged: return "输入数据已切换，候选结果已失效";
    case PartFailureReason::DisplayFailed: return "编辑结果显示失败";
    case PartFailureReason::InternalError: return "编辑处理发生内部错误";
    case PartFailureReason::InvalidEdit: return "编辑参数或标签数据无效";
    case PartFailureReason::ConstraintConflict: return "编辑与保护范围、作用域或种子位置冲突";
    case PartFailureReason::UnassignedVoxels: return "拆分后仍有体素未连接到种子";
    case PartFailureReason::RevisionConflict: return "数据版本已变化，请重新生成候选结果";
    case PartFailureReason::NoChange: return "本次操作未改变体素归属";
    case PartFailureReason::TimedOut: return "编辑超时";
    }
    return "编辑未完成";
}
std::uint64_t LabelAt(const LabelMap3DPayload& labels, const Voxel& point) {
    const auto& geometry = labels.GetGeometry();
    const auto x = static_cast<std::size_t>(static_cast<std::int64_t>(point[0]) - geometry.extent[0]);
    const auto y = static_cast<std::size_t>(static_cast<std::int64_t>(point[1]) - geometry.extent[2]);
    const auto z = static_cast<std::size_t>(static_cast<std::int64_t>(point[2]) - geometry.extent[4]);
    const auto offset = x + static_cast<std::size_t>(geometry.dimensions[0])
        * (y + static_cast<std::size_t>(geometry.dimensions[1]) * z);
    return std::visit([offset](const auto& values) -> std::uint64_t {
        if (!values || offset >= values->size()) throw std::runtime_error("标签索引超出冻结数据范围");
        return static_cast<std::uint64_t>((*values)[offset]);
    }, labels.GetValues());
}
Voxel OccupiedSeed(const LabelMap3DPayload& labels, const PartSnapshot& part, bool reverse) {
    const auto& geometry = labels.GetGeometry();
    auto bounds = part.metrics.voxelExtent;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        bounds[axis * 2] = std::max(bounds[axis * 2], geometry.extent[axis * 2]);
        bounds[axis * 2 + 1] = std::min(bounds[axis * 2 + 1], geometry.extent[axis * 2 + 1]);
    }
    const std::int64_t step = reverse ? -1 : 1;
    const auto start = [&](int axis) { return static_cast<std::int64_t>(bounds[axis * 2 + (reverse ? 1 : 0)]); };
    const auto in = [&](std::int64_t value, int axis) { return value >= bounds[axis * 2] && value <= bounds[axis * 2 + 1]; };
    std::size_t inspected = 0;
    for (auto z = start(2); in(z, 2); z += step)
        for (auto y = start(1); in(y, 1); y += step)
            for (auto x = start(0); in(x, 0); x += step) {
                if (++inspected > 2000000) throw std::runtime_error("已达到种子搜索上限；请选择更小的零件或裁剪输入数据");
                const Voxel voxel{static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)};
                if (LabelAt(labels, voxel) == part.labelId) return voxel;
            }
    throw std::runtime_error("所选零件范围内没有属于该零件的体素");
}
std::array<double, 3> Physical(const GridGeometry3D& geometry, const Voxel& voxel) {
    auto point = geometry.origin;
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t axis = 0; axis < 3; ++axis)
            point[row] += geometry.direction[row * 3 + axis] * geometry.spacing[axis] * voxel[axis];
    return point;
}
}
#endif

bool FeatureTestControls::Impl::PreparePart(const int mode) {
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    const auto feature = bindings.parts.lock();
    if (!feature || !context.data) return Fail("零件分割功能不可用");
    if (!EndCrop()) return Fail("请先结束当前裁剪操作");
    const auto state = feature->GetState();
    const auto snapshot = feature->GetPartSetSnapshot();
    if (!snapshot || snapshot->isStale || snapshot->parts.empty()) return Fail("编辑前请先按 B 生成当前零件结果");
    auto selected = std::find_if(snapshot->parts.begin(), snapshot->parts.end(),
        [](const auto& part) { return part.presentation.isSelected; });
    if (selected == snapshot->parts.end()) selected = snapshot->parts.begin();
    PartEditRequest request;
    request.expectedLabelMap = state.labelMap;
    request.expectedCatalogRevision = snapshot->catalogRevision;
    if (mode >= 7) request.operation = PartHistoryEdit{mode == 8};
    else if (mode == 6) {
        if (snapshot->parts.size() < 2) return Fail("合并需要两个当前有效零件");
        auto next = selected + 1;
        if (next == snapshot->parts.end()) next = snapshot->parts.begin();
        request.operation = PartMergeEdit{{selected->binding, next->binding}};
    } else {
        const auto graph = context.data->GetDataGraph();
        const auto data = context.data->GetData(graph, state.labelMap);
        const auto* labels = data ? dynamic_cast<const LabelMap3DPayload*>(data->payload.get()) : nullptr;
        if (!labels || !labels->GetValid()) return Fail("正式标签数据不可用");
        const auto& geometry = labels->GetGeometry();
        const auto seed = OccupiedSeed(*labels, *selected, false);
        const double radius = options.editRadius > 0 ? options.editRadius
            : 1.5 * *std::max_element(geometry.spacing.begin(), geometry.spacing.end());
        if (mode <= 1) {
            PartBrushEdit brush;
            brush.target = selected->binding;
            brush.isErase = mode == 1;
            brush.radiusMM = radius;
            brush.sourcePoints.push_back(Physical(geometry, seed));
            request.operation = std::move(brush);
        } else if (mode == 3) request.operation = PartIslandEdit{selected->binding, options.islandVoxels};
        else if (mode == 5) {
            const auto opposite = OccupiedSeed(*labels, *selected, true);
            if (seed == opposite) return Fail("拆分至少需要两个属于该零件的体素");
            PartSplitEdit split;
            split.target = selected->binding;
            split.seeds = {{seed, 1}, {opposite, 2}};
            request.operation = std::move(split);
        } else {
            auto box = geometry.extent;
            const auto padding = static_cast<int>(std::min(16.0, 1.0 + std::ceil(radius
                / *std::min_element(geometry.spacing.begin(), geometry.spacing.end()))));
            for (std::size_t axis = 0; axis < 3; ++axis) {
                box[axis * 2] = static_cast<int>(std::max<std::int64_t>(geometry.extent[axis * 2], static_cast<std::int64_t>(seed[axis]) - padding));
                box[axis * 2 + 1] = static_cast<int>(std::min<std::int64_t>(geometry.extent[axis * 2 + 1], static_cast<std::int64_t>(seed[axis]) + padding));
            }
            request.scope.extent = box;
            if (mode == 2) {
                auto fillSeed = seed;
                bool found = false;
                for (std::size_t axis = 0; axis < 3 && !found; ++axis) for (const int offset : {-1, 1}) {
                    const auto coordinate = static_cast<std::int64_t>(seed[axis]) + offset;
                    if (coordinate < geometry.extent[axis * 2] || coordinate > geometry.extent[axis * 2 + 1]) continue;
                    auto neighbor = seed; neighbor[axis] = static_cast<int>(coordinate);
                    if (LabelAt(*labels, neighbor) == 0) { fillSeed = neighbor; found = true; break; }
                }
                if (!found) return Fail("有限范围填充未找到相邻的背景种子");
                request.operation = PartFillEdit{selected->binding, fillSeed};
            } else {
                const auto image = context.data->GetData(graph, state.sourceRevision);
                const auto* payload = image ? dynamic_cast<const ImageGrid3DPayload*>(image->payload.get()) : nullptr;
                if (!payload) return Fail("源图像不可用");
                const auto view = session.GetRenderViewState({primaryView});
                PartGrowEdit grow;
                grow.target = selected->binding;
                grow.seeds = {seed};
                grow.minimum = view ? view->isoThreshold : payload->GetScalarRange()[0];
                grow.maximum = payload->GetScalarRange()[1];
                request.operation = std::move(grow);
            }
        }
        Status(std::string("编辑 ") + editNames[mode] + " | 标签=" + std::to_string(selected->labelId)
            + " | 种子=" + std::to_string(seed[0]) + "," + std::to_string(seed[1]) + "," + std::to_string(seed[2])
            + " | 半径=" + std::to_string(radius));
    }
    const auto admission = feature->SendEditRequest(std::move(request), [weak = owner](PartSegmentationResult result) {
        const auto self = weak.lock();
        if (!self || !self->m_impl->context.host) return;
        auto& tools = *self->m_impl;
        ++tools.partCompletions;
        const auto message = PartEditResultText(result);
        if (result.status != PartResultStatus::PreviewReady && result.status != PartResultStatus::Cancelled)
            tools.failure = message;
        tools.Status("编辑请求=" + std::to_string(result.requestId) + " | " + message);
        tools.Report();
    });
    if (admission.status != PartAdmissionStatus::Accepted)
        return Fail("编辑被拒绝，接纳状态=" + std::to_string(static_cast<int>(admission.status)) + "；请检查当前数据版本和工具内存预算");
    Status("已请求编辑候选结果 | 请求编号=" + std::to_string(admission.requestId));
    return true;
#else
    (void)mode; return Fail("请使用 MVVCVTK_BUILD_PART_SEGMENTATION=ON 构建");
#endif
}

bool FeatureTestControls::Impl::ConfirmPart() {
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    const auto feature = bindings.parts.lock();
    const auto preview = feature ? feature->GetEditPreview() : nullptr;
    if (!preview) return Fail("没有编辑候选结果；请先按 F7 或 F8");
    const auto admission = feature->SetEditCommit(preview->previewId, [weak = owner](PartSegmentationResult result) {
        const auto self = weak.lock();
        if (!self || !self->m_impl->context.host) return;
        auto& tools = *self->m_impl;
        ++tools.partCompletions;
        const auto message = PartEditResultText(result);
        if (result.status != PartResultStatus::Succeeded) tools.failure = message;
        tools.Status("编辑确认请求=" + std::to_string(result.requestId) + " | " + message);
        tools.Report();
    });
    return admission.status == PartAdmissionStatus::Accepted || Fail("编辑确认被拒绝");
#else
    return Fail("零件分割功能未启用");
#endif
}

bool FeatureTestControls::Impl::CancelPart() {
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    const auto feature = bindings.parts.lock();
    if (!feature) return Fail("零件分割功能不可用");
    const auto preview = feature->GetEditPreview();
    if (preview) {
        if (feature->ClearEditPreview(preview->previewId).status != PartMutationStatus::Succeeded) return Fail("丢弃请求被拒绝");
    } else {
        PartSegmentationRequest stop; stop.action = PartSegmentationAction::Stop;
        if (feature->SendRequest(stop).status != PartAdmissionStatus::Accepted) return Fail("编辑取消请求被拒绝");
    }
    Status("编辑已取消/丢弃；正式结果已保留"); return true;
#else
    return Fail("零件分割功能未启用");
#endif
}

bool FeatureTestControls::Impl::PrepareArtifact() {
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    const auto feature = bindings.artifact.lock();
    if (!feature || !context.data) return Fail("伪影处理功能不可用");
    if (!EndCrop()) return Fail("请先结束当前裁剪操作");
    const auto graph = context.data->GetDataGraph();
    const auto primary = context.data->GetDataBinding(graph, primaryVolumeBinding);
    const auto data = primary && primary->target ? context.data->GetData(graph, *primary->target) : nullptr;
    const auto* image = data ? dynamic_cast<const ImageGrid3DPayload*>(data->payload.get()) : nullptr;
    if (!image) return Fail("伪影处理前请先加载当前图像");
    const auto range = image->GetScalarRange();
    const auto span = std::max(1e-6, range[1] - range[0]);
    ArtifactRequest request;
    request.source = data->self;
    request.timeoutMs = options.timeoutMs;
    if (artifactMode != 1) {
        ArtifactRingParams ring;
        ring.axis = options.ringAxis;
        std::size_t planeAxis = 0;
        for (int axis = 0; axis < 3; ++axis) if (axis != ring.axis) {
            const auto& extent = image->GetGeometry().extent;
            ring.centerIndex[planeAxis++] = 0.5 * (static_cast<double>(extent[axis * 2]) + extent[axis * 2 + 1]);
        }
        if (options.ringCenter) ring.centerIndex = *options.ringCenter;
        ring.ringWidth = options.ringWidth;
        ring.strength = options.ringStrength;
        ring.threshMin = range[0]; ring.threshMax = range[1];
        ring.threshold = span * 0.2;
        ring.maxCorrection = span * 0.25;
        request.ring = ring;
        Status("环形校正轴=" + std::to_string(ring.axis) + " 中心=" + std::to_string(ring.centerIndex[0])
            + "," + std::to_string(ring.centerIndex[1]) + " 强度=" + std::to_string(ring.strength));
    }
    if (artifactMode != 0) {
        ArtifactDiffusionParams diffusion;
        diffusion.iterations = options.diffusionIterations;
        diffusion.threshold = span * 0.1;
        request.diffusion = diffusion;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto admission = feature->SendRequest({ArtifactAction::Prepare, request, 0});
    if (admission.error == ArtifactError::TooLarge) {
        return Fail("伪影处理超出预算：输入字节数=" + std::to_string(image->GetValues()->size())
            + "，工作预算字节数=" + std::to_string(options.budgetBytes)
            + "。算法还需要完整输出和工作缓冲；请显式调整 --tool-budget-mib 或选择较小的真实体数据。");
    }
    if (admission.error != ArtifactError::None)
        return Fail("伪影处理准备请求被拒绝，错误=" + std::to_string(static_cast<int>(admission.error))
            + "；请检查输入类型、几何与有效性。裁切掩码仍保留原网格尺寸，不能减少整卷工作预算。");
    artifactInput = data->self;
    artifactStarted = started;
    artifactRunMode = artifactMode;
    artifactElapsedSeconds = -1;
    Status(std::string("伪影处理 ") + artifactNames[artifactMode] + " 已请求 | 请求编号=" + std::to_string(admission.requestId));
    return true;
#else
    return Fail("请使用 MVVCVTK_BUILD_ARTIFACT_REDUCTION=ON 构建");
#endif
}

bool FeatureTestControls::Impl::ArtifactActionRequest(const int action) {
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    const auto feature = bindings.artifact.lock();
    if (!feature) return Fail("伪影处理功能不可用");
    const auto state = feature->GetState();
    ArtifactHostRequest request;
    if (action == 0) {
        request.action = ArtifactAction::Commit;
        request.requestId = state.requestId;
    } else request.action = state.status == ArtifactStatus::Running || state.status == ArtifactStatus::Cancelling
        ? ArtifactAction::Cancel : ArtifactAction::Discard;
    const auto admission = feature->SendRequest(request);
    if (admission.error != ArtifactError::None)
        return Fail("伪影处理操作被拒绝，错误=" + std::to_string(static_cast<int>(admission.error)));
    // Feature 状态随下一次 Prepare/Discard 重置；应用的“查看上次结果”
    // 仍应指向最后一次成功发布的图版本，不持有或复制整卷数据。
    if (action == 0) artifactPublished = feature->GetState().correctedVolume;
    Status(action == 0 ? "校正体数据已发布；按 Shift+F10 选择显示"
        : request.action == ArtifactAction::Cancel ? "已请求取消伪影处理，正在等待当前分块结束"
        : "伪影处理候选结果已丢弃");
    Report(); return true;
#else
    (void)action; return Fail("伪影处理功能未启用");
#endif
}

bool FeatureTestControls::Impl::SelectArtifact(const bool restore) {
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    const auto feature = bindings.artifact.lock();
    const auto published = artifactPublished ? artifactPublished
        : feature ? feature->GetState().correctedVolume : std::nullopt;
    const auto revision = restore ? artifactInput : published;
    if (!revision || !context.data) return Fail("没有可选择的已发布校正数据或输入数据");
    if (!EndCrop()) return Fail("选择数据版本前请先结束裁剪");
    const auto binding = context.data->GetDataBinding(context.data->GetDataGraph(), primaryVolumeBinding);
    if (!binding) return Fail("主数据绑定不可用");
    HostDataSelectRequest request;
    request.dataRevision = *revision;
    request.expectedBindingRevision = binding->revision;
    if (!session.SendRequest(std::move(request))) return Fail("主数据选择被拒绝");
    Status(std::string(restore ? "已恢复伪影处理输入数据：" : "已选择校正体数据：") + Ref(*revision));
    return true;
#else
    (void)restore; return Fail("伪影处理功能未启用");
#endif
}

#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
namespace {
std::string SurfaceResultText(const SurfaceDeterminationResult& result) {
    switch (result.failureReason) {
    case SurfaceFailureReason::None: return "网格已生成";
    case SurfaceFailureReason::InvalidSource: return "输入数据不可用";
    case SurfaceFailureReason::InvalidGeometry: return "数据网格或几何信息无效";
    case SurfaceFailureReason::UnsupportedScalar: return "不支持此体素数值类型";
    case SurfaceFailureReason::InvalidRoi: return "局部范围无效";
    case SurfaceFailureReason::ThresholdUnreliable: return "自动阈值不可靠，请调整阈值后重试";
    case SurfaceFailureReason::NoSurface: return "当前阈值和范围内没有可用表面";
    case SurfaceFailureReason::BudgetExceeded: return "表面网格及工作区超出内存预算";
    case SurfaceFailureReason::Cancelled: return "网格提取已取消";
    case SurfaceFailureReason::SourceChanged: return "输入数据已切换，网格结果已失效";
    case SurfaceFailureReason::DisplayFailed: return "网格数据已生成，但显示失败";
    case SurfaceFailureReason::InternalError: return "网格提取发生内部错误";
    }
    return "网格提取未完成";
}
}
#endif

bool FeatureTestControls::Impl::StartSurface(const bool local, const bool gradient) {
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    const auto feature = bindings.surface.lock();
    if (!feature) return Fail("表面确定功能不可用");
    if (!EndCrop()) return Fail("提取网格前请先结束裁剪");
    const auto view = session.GetRenderViewState({primaryView});
    if (!view || !GetDataRevisionRefValid(view->dataRevision)) return Fail("请先加载当前图像");
    SurfaceDeterminationStartParams params;
    params.targetViews.viewIds = {primaryView};
    params.method = gradient ? SurfaceDeterminationMethod::GradientPeak
        : local ? SurfaceDeterminationMethod::LocalAdaptiveIso50 : SurfaceDeterminationMethod::GlobalIsoPreview;
    params.initialIsoValue = view->isoThreshold;
    SurfaceDeterminationRequest request;
    request.action = SurfaceDeterminationAction::Start;
    request.start = params;
    const auto admission = feature->SendRequest(request, [weak = owner](SurfaceDeterminationResult result) {
        const auto self = weak.lock();
        if (!self || !self->m_impl->context.host) return;
        auto& tools = *self->m_impl;
        const auto message = SurfaceResultText(result);
        if (result.status == SurfaceResultStatus::Failed) tools.failure = message;
        if (const auto surface = tools.bindings.surface.lock()) {
            const auto state = surface->GetState();
            const auto snapshot = surface->GetSurfaceSnapshot();
            std::cout << "AUDIT_SURFACE method=" << (snapshot ? static_cast<int>(snapshot->method) : -1)
                << " points=" << state.pointCount << " accepted=" << state.acceptedPointCount
                << " rejected=" << state.rejectedPointCount << " truncated=" << state.truncatedPointCount
                << " nonmanifold=" << state.nonManifoldObjectCount << " source_generation=" << state.sourceRevision.generation << '\n';
        }
        tools.Status("网格请求=" + std::to_string(result.requestId) + " 点数=" + std::to_string(result.pointCount)
            + " | " + message);
    });
    if (admission.status != SurfaceAdmissionStatus::Accepted) return Fail("网格提取被拒绝");
    Status(std::string(gradient ? "梯度峰值网格" : local ? "局部自适应网格" : "全局预览网格") + " 已请求 | 等值面阈值=" + std::to_string(view->isoThreshold));
    return true;
#else
    (void)local; (void)gradient; return Fail("请使用 MVVCVTK_BUILD_SURFACE_DETERMINATION=ON 构建");
#endif
}

bool FeatureTestControls::Impl::StartAlignment(const bool bestFit) {
#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT) && defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    const auto feature = bindings.alignment.lock();
    const auto surface = bindings.surface.lock();
    if (!feature || !surface || !context.data) return Fail("对齐或表面确定功能不可用");
    if (feature->GetState().isBusy) return Fail("对齐任务正在运行；按 Alt+F11 取消");
    const auto generation = surface->GetSurfaceSnapshot();
    if (!generation || !GetDataRevisionRefValid(generation->meshRevision)) return Fail("请先按 K 并等待网格生成");
    const auto graph = context.data->GetDataGraph();
    const auto primary = context.data->GetDataBinding(graph, primaryVolumeBinding);
    if (!primary || primary->target != generation->sourceRevision) return Fail("网格源数据已过期；请按 K 提取当前网格");
    const auto meshData = context.data->GetData(graph, generation->meshRevision);
    const auto* mesh = meshData ? dynamic_cast<const SurfaceMeshPayload*>(meshData->payload.get()) : nullptr;
    if (!mesh || mesh->GetVertices().size() < 9) return Fail("网格几何数据不足，无法进行参考验证");
    const auto& vertices = mesh->GetVertices();
    const std::size_t count = vertices.size() / 3;
    const auto point = [&](std::size_t index) { return AlignmentPoint{vertices[index*3], vertices[index*3+1], vertices[index*3+2]}; };
    const auto subtract = [](const auto& a, const auto& b) { return AlignmentPoint{a[0]-b[0], a[1]-b[1], a[2]-b[2]}; };
    const auto squared = [](const auto& v) { return v[0]*v[0]+v[1]*v[1]+v[2]*v[2]; };
    const auto cross = [](const auto& a, const auto& b) { return AlignmentPoint{a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; };
    const auto stride = std::max<std::size_t>(1, count / 4096);
    const auto first = point(0);
    std::size_t secondIndex = 0, thirdIndex = 0, fourthIndex = 0;
    double distance = 0, area = 0, volume = 0;
    for (std::size_t index = 0; index < count; index += stride) {
        const auto candidate = squared(subtract(point(index), first));
        if (candidate > distance) { distance = candidate; secondIndex = index; }
    }
    const auto direction = subtract(point(secondIndex), first);
    for (std::size_t index = 0; index < count; index += stride) {
        const auto candidate = squared(cross(direction, subtract(point(index), first)));
        if (candidate > area) { area = candidate; thirdIndex = index; }
    }
    if (distance <= 0 || area <= distance * distance * 1e-12) return Fail("参考网格采样点共线");
    const auto normal = cross(direction, subtract(point(thirdIndex), first));
    for (std::size_t index = 0; index < count; index += stride) {
        const auto delta = subtract(point(index), first);
        const auto candidate = std::abs(delta[0]*normal[0]+delta[1]*normal[1]+delta[2]*normal[2]);
        if (candidate > volume) { volume = candidate; fourthIndex = index; }
    }
    std::vector<AlignmentPoint> samples{first, point(secondIndex), point(thirdIndex)};
    if (volume > std::sqrt(area * distance) * 1e-6) samples.push_back(point(fourthIndex));
    const double angle = 10.0 * 3.14159265358979323846 / 180.0;
    expectedTransform = {std::cos(angle),-std::sin(angle),0,2, std::sin(angle),std::cos(angle),0,-3,
        0,0,1,4, 0,0,0,1};
    const auto nominalPoint = [&](const AlignmentPoint& p) {
        AlignmentPoint result{};
        for (std::size_t row = 0; row < 3; ++row) {
            result[row] = expectedTransform[row*4+3];
            for (std::size_t column = 0; column < 3; ++column) result[row] += expectedTransform[row*4+column]*p[column];
        }
        return result;
    };
    std::vector<double> referenceVertices, nominalX, nominalY, nominalZ;
    for (const auto& p : samples) {
        referenceVertices.insert(referenceVertices.end(), p.begin(), p.end());
        const auto q = nominalPoint(p);
        nominalX.push_back(q[0]); nominalY.push_back(q[1]); nominalZ.push_back(q[2]);
    }
    const DataRevisionRef referenceRef{context.data->CreateDataEntityId(), 1};
    const DataRevisionRef nominalRef{context.data->CreateDataEntityId(), 1};
    auto reference = std::make_shared<const SurfaceMeshPayload>(std::move(referenceVertices), std::vector<std::uint64_t>{},
        std::vector<MeshAttribute>{}, mesh->GetCoordinateFrame());
    auto nominal = std::make_shared<const RecordTablePayload>(DataTypes::recordTable, "standalone-geometric-reference-v1",
        std::vector<RecordColumn>{{"target-x", std::move(nominalX)}, {"target-y", std::move(nominalY)}, {"target-z", std::move(nominalZ)}});
    // Explicit test geometry: these samples make no measurement-quality claim. Preserve the source mesh edge.
    const DataProvenance provenance{controlId, "known-transform-geometric-reference", "1", "model-units; Z=10deg; translation=2,-3,4; not-measurement-points"};
    const std::vector<DataInputRef> inputs{{"source-volume", generation->sourceRevision}, {"sampled-mesh", generation->meshRevision}};
    DataTransaction transaction;
    transaction.outputs = {{referenceRef.entityId, 0, DataTypes::surfaceMesh, inputs, reference, provenance},
        {nominalRef.entityId, 0, DataTypes::recordTable, inputs, nominal, provenance}};
    if (context.data->SetDataCommit(std::move(transaction)).status != DataCommitStatus::Succeeded) return Fail("参考数据发布失败");
    AlignmentInput input;
    input.source = generation->sourceRevision;
    input.mesh = referenceRef;
    input.scopeData = nominalRef;
    input.sourceFrameId = "standalone-source-model";
    input.coordinateFrame = mesh->GetCoordinateFrame();
    input.scope = "standalone-reference";
    input.sourceBinding = std::string(primaryVolumeBinding);
    input.unit = AlignmentUnit::ModelUnit;
    AlignmentRecipe recipe;
    recipe.id = bestFit ? "standalone-best-fit-reference" : "standalone-rps-reference";
    recipe.targetFrameId = "standalone-known-target";
    recipe.unit = AlignmentUnit::ModelUnit;
    recipe.method = bestFit ? AlignmentMethod::ConstrainedBestFit : AlignmentMethod::Rps;
    recipe.datumCount = 0;
    recipe.nominalData = nominalRef;
    recipe.lengthScale = std::max(1.0, std::sqrt(distance));
    recipe.huberDistance = 1e6;
    recipe.maxPairDistance = 1e9;
    recipe.maxAlignmentRms = 1e-5;
    recipe.exactMesh = bestFit ? referenceRef : DataRevisionRef{};
    for (std::size_t index = 0; index < samples.size(); ++index) {
        AlignmentGeometrySpec geometry;
        geometry.id = "sample-" + std::to_string(index);
        geometry.kind = AlignmentGeometryKind::Point;
        geometry.region.pinnedMesh = referenceRef;
        geometry.region.vertexIds = {index};
        recipe.geometries.push_back(geometry);
        if (bestFit) recipe.fitPairs.push_back({index, nominalPoint(samples[index]), {}, 1.0});
        else for (std::size_t axis = 0; axis < 3; ++axis) {
            AlignmentConstraint constraint;
            constraint.geometryIndex = index;
            constraint.nominalPoint = nominalPoint(samples[index]);
            constraint.targetDirection = {0,0,0}; constraint.targetDirection[axis] = 1;
            recipe.constraints.push_back(constraint);
        }
    }
    alignmentMatched = false;
    AlignmentRequest save;
    save.action = AlignmentAction::SaveRecipe;
    save.recipe = std::move(recipe);
    const auto admission = feature->SendRequest(std::move(save), [weak = owner, input](AlignmentResult saved) {
        const auto self = weak.lock();
        if (!self || !self->m_impl->context.host) return;
        auto& tools = *self->m_impl;
        const auto alignment = tools.bindings.alignment.lock();
        if (!alignment || !GetDataRevisionRefValid(saved.recipe)) { tools.Fail("参考对齐方案被拒绝：" + saved.message); return; }
        AlignmentRequest request;
        request.input = input;
        request.recipeRef = saved.recipe;
        const auto started = alignment->SendRequest(std::move(request), [weak](AlignmentResult result) {
            const auto current = weak.lock();
            if (!current || !current->m_impl->context.host) return;
            auto& app = *current->m_impl;
            ++app.alignmentCompletions;
            app.lastAlignment = result;
            const auto data = app.context.data->GetData(app.context.data->GetDataGraph(), result.transform);
            const auto* transform = data ? dynamic_cast<const Transform3DPayload*>(data->payload.get()) : nullptr;
            double error = 0;
            if (transform) for (std::size_t index = 0; index < 16; ++index)
                error = std::max(error, std::abs(transform->GetSourceToTarget()[index] - app.expectedTransform[index]));
            app.alignmentMatched = transform && result.status == AlignmentStatus::FullyDetermined
                && result.diagnostics.isQualityPassed && error < 1e-5;
            if (!app.alignmentMatched && result.status != AlignmentStatus::Cancelled) app.failure = "已知变换验证不匹配：" + result.message;
            std::cout << "AUDIT_ALIGNMENT matrix_max_error=" << error
                << " rms=" << result.diagnostics.rms << " remaining_dof=" << result.diagnostics.remaining << '\n';
            app.Status("对齐验证 | 已匹配=" + std::to_string(app.alignmentMatched)
                + " 矩阵最大误差=" + std::to_string(error) + " 均方根误差=" + std::to_string(result.diagnostics.rms));
            app.Report();
        });
        if (started.status != AlignmentAdmissionStatus::Accepted) tools.Fail("对齐求解被拒绝");
    });
    if (admission.status != AlignmentAdmissionStatus::Accepted) return Fail("对齐方案接纳被拒绝");
    Status(bestFit ? "已请求最佳拟合几何验证" : "已请求参考点系统几何验证");
    return failure.empty();
#else
    (void)bestFit; return Fail("请同时启用计量对齐和表面确定功能");
#endif
}

bool FeatureTestControls::Impl::AlignmentActionRequest(const int action) {
#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT)
    const auto feature = bindings.alignment.lock();
    if (!feature) return Fail("计量对齐功能不可用");
    AlignmentRequest request;
    if (action == 0) { request.action = AlignmentAction::SetVisibility; request.isVisible = !feature->GetState().isOverlayVisible; }
    else request.action = AlignmentAction::Cancel;
    const auto admission = feature->SendRequest(request);
    if (admission.status != AlignmentAdmissionStatus::Accepted) return Fail("对齐操作被拒绝；请检查是否有任务正在运行");
    Report(); return true;
#else
    (void)action; return Fail("计量对齐功能未启用");
#endif
}

void FeatureTestControls::Impl::Report() {
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    if (const auto feature = bindings.parts.lock()) {
        const auto state = feature->GetState();
        const auto preview = feature->GetEditPreview();
        std::cout << "AUDIT_PART count=" << state.partCount << " result_revision=" << state.resultRevision
            << " source_generation=" << state.sourceRevision.generation << '\n';
        std::cout << "[工具·零件] 模式=" << editNames[editMode] << " 零件数=" << state.partCount
            << " 标签=" << Ref(state.labelMap) << " 目录版本=" << state.catalogRevision;
        if (preview) std::cout << " 候选结果=" << preview->previewId
            << " 候选零件数=" << (preview->parts ? preview->parts->parts.size() : 0)
            << " 候选体素数=" << (preview->labels ? preview->labels->size() : 0);
        std::cout << '\n';
    }
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    if (const auto feature = bindings.surface.lock()) {
        const auto result = feature->GetSurfaceSnapshot();
        std::cout << "[工具·表面] 阶段=" << static_cast<int>(feature->GetState().stage);
        if (result) std::cout << " 网格=" << Ref(result->meshRevision) << " 点数=" << (result->points ? result->points->size() : 0);
        std::cout << '\n';
    }
#endif
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    if (const auto feature = bindings.artifact.lock()) {
        const auto state = feature->GetState();
        if (state.status == ArtifactStatus::Idle && state.commitStatus == DataCommitStatus::Succeeded) {
            std::cout << "AUDIT_ARTIFACT mode=" << artifactMode << " changed=" << state.quality.changedCount
                << " rms_delta=" << state.quality.rmsDelta << " fidelity_verified=" << state.quality.fidelityVerified << '\n';
        }
        std::cout << "[工具·伪影] 模式=" << artifactNames[artifactMode] << " 状态=" << static_cast<int>(state.status)
            << " 错误=" << static_cast<int>(state.error) << " 进度=" << state.progressPercent
            << " 所需字节数=" << state.requiredBytes << " 变化体素数=" << state.quality.changedCount
            << " 均方根误差=" << state.quality.rmsDelta;
        if (state.correctedVolume) std::cout << " 校正结果=" << Ref(*state.correctedVolume);
        if (state.qualityReport) std::cout << " 报告=" << Ref(*state.qualityReport);
        std::cout << '\n';
    }
#endif
#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT)
    if (const auto feature = bindings.alignment.lock()) {
        const auto state = feature->GetState();
        std::cout << "[工具·对齐] 正在运行=" << state.isBusy << " 当前有效=" << state.isCurrent
            << " 显示就绪=" << state.isDisplayReady << " 参考验证匹配=" << alignmentMatched << '\n';
        if (lastAlignment && GetDataRevisionRefValid(lastAlignment->result)) {
            const auto archive = feature->GetArchive(lastAlignment->result);
            const auto result = feature->GetResult(lastAlignment->result);
            std::cout << "  结果=" << Ref(lastAlignment->result) << " 变换=" << Ref(lastAlignment->transform);
            if (result) std::cout << " 均方根误差=" << result->diagnostics.rms << " 剩余自由度=" << result->diagnostics.remaining;
            if (archive) {
                std::cout << "\n  源到目标变换矩阵=";
                for (const auto value : archive->sourceToTarget) std::cout << value << ' ';
            }
            std::cout << '\n';
        }
    }
#endif
    std::cout << std::flush;
}

void FeatureTestControls::Impl::UpdateCropPartVisibility() {
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    const auto parts = bindings.parts.lock();
    if (!parts) return;
    const auto crop = bindings.crop.lock();
    const auto cropState = crop ? crop->GetState() : CropHostState{};
    // Exit 只关闭控件；当前前缀仍有节点时，底层模型继续受裁剪影响。
    // baseNodeCount 已物化进当前输入的 mask，针对该输入重新分割的零件可以显示。
    const bool isCropping = cropState.isActive || cropState.history.nodeCount != 0;
    const auto state = parts->GetState();
    if (isCropping && !partVisibilityBeforeCrop) partVisibilityBeforeCrop = state.isOverlayVisible;
    if (!partVisibilityBeforeCrop) return;
    // 切换源数据后保持旧叠加隐藏；待当前输入重新生成结果后再恢复显示偏好。
    if (!isCropping && state.status == PartSegmentationStatus::Stale) return;
    const bool isVisible = !isCropping && *partVisibilityBeforeCrop;
    if (state.isOverlayVisible != isVisible) {
        PartSegmentationRequest request;
        request.action = PartSegmentationAction::SetVisibility;
        request.isVisible = isVisible;
        if (parts->SendRequest(request).status != PartAdmissionStatus::Accepted) return;
        if (parts->GetState().isOverlayVisible != isVisible) return;
        Status(isCropping ? "裁剪生效期间已临时隐藏旧零件叠加" : "已恢复零件叠加可见状态");
    }
    if (!isCropping) partVisibilityBeforeCrop.reset();
#endif
}

void FeatureTestControls::Impl::Tick() {
    UpdateCropPartVisibility();
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    const auto feature = bindings.artifact.lock();
    if (!feature) return;
    const auto state = feature->GetState();
    const auto status = static_cast<int>(state.status);
    const auto progress = static_cast<int>(state.progressPercent / 10);
    const bool isRunning = state.status == ArtifactStatus::Running || state.status == ArtifactStatus::Cancelling;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - artifactStarted).count();
    const bool reportChanged = status != artifactStatus || progress != artifactProgress;
    if (!reportChanged && (!isRunning || elapsed == artifactElapsedSeconds)) return;
    artifactStatus = status; artifactProgress = progress;
    artifactElapsedSeconds = elapsed;
    if (isRunning) {
        std::string message = state.status == ArtifactStatus::Cancelling ? "伪影正在取消，等待分块结束"
            : std::string(artifactNames[artifactRunMode]) + " | " + ArtifactStageText(state.progressPercent);
        message += " | " + std::to_string(state.progressPercent) + "% | " + std::to_string(elapsed) + " 秒";
        if (state.status == ArtifactStatus::Running) message += " | Alt+F10 取消";
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        const auto parts = bindings.parts.lock();
        if (parts && parts->GetEditPreview()) message += " | 零件候选：Ctrl+F7 确认";
#endif
        Status(message);
    }
    else if (state.status == ArtifactStatus::Failed) {
        std::string message = std::string("伪影处理：") + ArtifactErrorText(state.error);
        if (state.error == ArtifactError::TimedOut) message += "（" + std::to_string(options.timeoutMs / 1000.0) + " 秒）";
        if (state.error != ArtifactError::Cancelled) failure = message;
        Status(message + "；未生成候选，正式结果保留");
    }
    else if (state.status == ArtifactStatus::Ready)
        Status("伪影处理候选结果已就绪 | 耗时 " + std::to_string(elapsed) + " 秒 | Ctrl+F10 发布，Alt+F10 丢弃");
    if (reportChanged) Report();
#endif
}

FeatureTestControls::FeatureTestControls(VtkAppHostSession& session, FeatureTestBindings bindings,
    FeatureTestOptions options, HostViewTargets views)
    : m_impl(std::make_unique<Impl>(session, std::move(bindings), std::move(options), std::move(views))) {}
FeatureTestControls::~FeatureTestControls() = default;
std::string_view FeatureTestControls::GetFeatureId() const noexcept { return controlId; }
bool FeatureTestControls::AttachHost(const HostFeatureContext& context) {
    if (m_impl->context.host || !context.host || !context.data || weak_from_this().expired()) return false;
    m_impl->context = context;
    m_impl->owner = weak_from_this();
    HostInputBinding binding;
    binding.featureId = controlId;
    binding.targetViews = m_impl->views;
    binding.onInput = [weak = weak_from_this()](const InteractionEvent& event) {
        const auto self = weak.lock();
        return self ? self->m_impl->OnInput(event) : InteractionResult{};
    };
    if (context.host->AttachInput(std::move(binding))) return true;
    m_impl->context = {}; return false;
}
bool FeatureTestControls::DetachHost() {
    if (m_impl->context.host && !m_impl->context.host->DetachInput(controlId)) return false;
    m_impl->down.fill(false);
    m_impl->context = {};
    return true;
}
bool FeatureTestControls::OnHostTick() {
    if (!m_impl->context.host) return false;
    m_impl->Tick(); return true;
}
std::string FeatureTestControls::GetFailure() const { return m_impl->failure; }

std::vector<FeatureTestStep> FeatureTestControls::GetAuditSteps(const bool isReal) {
    (void)isReal;
    std::vector<FeatureTestStep> steps;
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    const auto partReady = [weak = m_impl->bindings.parts] {
        const auto feature = weak.lock(); return feature && static_cast<bool>(feature->GetEditPreview());
    };
    const auto partCount = [weak = m_impl->bindings.parts](std::size_t count) {
        const auto feature = weak.lock();
        const auto snapshot = feature ? feature->GetPartSetSnapshot() : nullptr;
        return snapshot && snapshot->parts.size() == count && !feature->GetEditPreview();
    };
    if (isReal) {
        const auto baseline = std::make_shared<std::shared_ptr<const PartSetSnapshot>>();
        const auto painted = std::make_shared<std::shared_ptr<const PartSetSnapshot>>();
        const auto restored = [weak = m_impl->bindings.parts, baseline] {
            const auto feature = weak.lock();
            const auto current = feature ? feature->GetPartSetSnapshot() : nullptr;
            if (!current || !*baseline || feature->GetEditPreview()
                || current->resultRevision <= (*baseline)->resultRevision
                || current->parts.size() != (*baseline)->parts.size()) return false;
            for (std::size_t i = 0; i < current->parts.size(); ++i) {
                const auto& a = current->parts[i];
                const auto& b = (*baseline)->parts[i];
                if (a.binding.object.objectId != b.binding.object.objectId
                    || a.labelId != b.labelId || a.metrics != b.metrics || a.userState != b.userState) return false;
            }
            return true;
        };
        steps.push_back({"real-edit-select", {0,"F6"}, [weak = m_impl->bindings.parts, baseline] {
            const auto feature = weak.lock();
            *baseline = feature ? feature->GetPartSetSnapshot() : nullptr;
            return *baseline && !(*baseline)->isStale && !(*baseline)->parts.empty();
        }});
        // 擦除已占用种子保证存在可测的标签变化；涂绘内部点可能合法地NoChange。
        steps.push_back({"real-edit-erase", {0,"F6"}, [weak = weak_from_this()] {
            const auto self = weak.lock(); return self && self->m_impl->editMode == 1;
        }});
        steps.push_back({"real-edit-preview", {0,"F7"}, partReady});
        steps.push_back({"real-edit-commit", {0,"F7",true}, [weak = m_impl->bindings.parts, baseline, painted] {
            const auto feature = weak.lock();
            const auto current = feature ? feature->GetPartSetSnapshot() : nullptr;
            if (!current || !*baseline || feature->GetEditPreview()
                || current->resultRevision <= (*baseline)->resultRevision
                || current->sourceRevision != (*baseline)->sourceRevision) return false;
            std::uint64_t before = 0, after = 0;
            for (const auto& part : (*baseline)->parts) before += part.metrics.voxelCount;
            for (const auto& part : current->parts) after += part.metrics.voxelCount;
            if (after >= before) return false;
            std::cout << "AUDIT_EDIT erased_voxels=" << before - after << '\n';
            *painted = current;
            return true;
        }});
        steps.push_back({"real-edit-undo-preview", {0,"F8"}, partReady});
        steps.push_back({"real-edit-undo", {0,"F7",true}, restored});
        steps.push_back({"real-edit-redo-preview", {0,"F8",false,false,true}, partReady});
        steps.push_back({"real-edit-redo", {0,"F7",true}, [weak = m_impl->bindings.parts, painted] {
            const auto feature = weak.lock();
            const auto current = feature ? feature->GetPartSetSnapshot() : nullptr;
            if (!current || !*painted || feature->GetEditPreview()
                || current->resultRevision <= (*painted)->resultRevision
                || current->parts.size() != (*painted)->parts.size()) return false;
            for (std::size_t i = 0; i < current->parts.size(); ++i) {
                if (current->parts[i].binding.object.objectId != (*painted)->parts[i].binding.object.objectId
                    || current->sourceRevision != (*painted)->sourceRevision
                    || current->parts[i].labelId != (*painted)->parts[i].labelId
                    || current->parts[i].metrics != (*painted)->parts[i].metrics
                    || current->parts[i].userState != (*painted)->parts[i].userState) return false;
            }
            return true;
        }});
    } else {
    steps.push_back({"工具：合并预览", {0,"F7"}, partReady});
    steps.push_back({"工具：确认合并", {0,"F7",true}, [partCount] { return partCount(1); }});
    steps.push_back({"工具：撤销预览", {0,"F8"}, partReady});
    steps.push_back({"工具：确认撤销", {0,"F7",true}, [partCount] { return partCount(2); }});
    steps.push_back({"工具：重做预览", {0,"F8",false,false,true}, partReady});
    steps.push_back({"工具：确认重做", {0,"F7",true}, [partCount] { return partCount(1); }});
    steps.push_back({"工具：选择涂绘", {0,"F6"}, [weak = weak_from_this()] {
        const auto self = weak.lock(); return self && self->m_impl->editMode == 0;
    }});
    steps.push_back({"工具：涂绘预览", {0,"F7"}, partReady});
    steps.push_back({"工具：丢弃涂绘", {0,"F7",false,true}, [partCount] { return partCount(1); }});
    }
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    const auto surfaceReady = [weak = m_impl->bindings.surface](SurfaceDeterminationMethod method) {
        const auto feature = weak.lock();
        const auto result = feature ? feature->GetSurfaceSnapshot() : nullptr;
        return feature && feature->GetState().stage == SurfaceDeterminationStage::Ready
            && result && result->method == method && GetDataRevisionRefValid(result->meshRevision)
            && result->points && !result->points->empty() && result->triangleIndices && !result->triangleIndices->empty();
    };
    steps.push_back({"surface-global", {'k'}, [surfaceReady] {
        return surfaceReady(SurfaceDeterminationMethod::GlobalIsoPreview);
    }});
    steps.push_back({"surface-local", {'k',{},false,false,true}, [surfaceReady] {
        return surfaceReady(SurfaceDeterminationMethod::LocalAdaptiveIso50);
    }});
    steps.push_back({"surface-gradient", {'k',{},true,false,true}, [surfaceReady] {
        return surfaceReady(SurfaceDeterminationMethod::GradientPeak);
    }});
#endif
#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT) && defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    const auto aligned = [weak = weak_from_this()](std::uint64_t count) {
        const auto self = weak.lock();
        return self && self->m_impl->alignmentMatched && self->m_impl->alignmentCompletions >= count;
    };
    steps.push_back({"工具：参考点系统对齐验证", {0,"F11"}, [aligned] { return aligned(1); }});
    steps.push_back({"工具：最佳拟合对齐验证", {0,"F11",false,false,true}, [aligned] { return aligned(2); }});
    steps.push_back({"工具：隐藏对齐结果", {0,"F11",true}, [weak = m_impl->bindings.alignment] {
        const auto feature = weak.lock(); return feature && !feature->GetState().isOverlayVisible;
    }});
    steps.push_back({"工具：显示对齐结果", {0,"F11",true}, [weak = m_impl->bindings.alignment] {
        const auto feature = weak.lock(); return feature && feature->GetState().isOverlayVisible;
    }});
#endif
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    const auto artifactReady = [weak = m_impl->bindings.artifact] {
        const auto feature = weak.lock(); return feature && feature->GetState().status == ArtifactStatus::Ready;
    };
    const auto artifactPublished = [weak = m_impl->bindings.artifact] {
        const auto feature = weak.lock(); return feature && feature->GetState().status == ArtifactStatus::Idle
            && feature->GetState().commitStatus == DataCommitStatus::Succeeded
            && feature->GetState().correctedVolume.has_value()
            && feature->GetState().qualityReport.has_value()
            && !feature->GetState().quality.fidelityVerified;
    };
    for (const auto* name : artifactNames) {
        if (name != artifactNames.front()) steps.push_back({std::string("工具：选择 ") + name, {0,"F9"}, [] { return true; }});
        steps.push_back({std::string("工具：准备 ") + name, {0,"F10"}, artifactReady});
        steps.push_back({std::string("工具：发布 ") + name, {0,"F10",true}, artifactPublished});
    }
    steps.push_back({"工具：选择校正数据", {0,"F10",false,false,true}, [weak = weak_from_this()] {
        const auto self = weak.lock();
        if (!self) return false;
        const auto feature = self->m_impl->bindings.artifact.lock();
        const auto image = self->m_impl->session.GetImageDescriptor();
        if (!feature || !image || feature->GetState().correctedVolume != image->dataRevision) return false;
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
        const auto parts = self->m_impl->bindings.parts.lock();
        if (parts && parts->GetPartSetSnapshot() && !parts->GetPartSetSnapshot()->isStale) return false;
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
        const auto surface = self->m_impl->bindings.surface.lock();
        if (surface && surface->GetSurfaceSnapshot()
            && surface->GetState().stage != SurfaceDeterminationStage::Stale) return false;
#endif
        return true;
    }});
    steps.push_back({"工具：恢复校正输入", {0,"F10",true,false,true}, [weak = weak_from_this()] {
        const auto self = weak.lock();
        const auto image = self ? self->m_impl->session.GetImageDescriptor() : std::nullopt;
        return self && image && self->m_impl->artifactInput == image->dataRevision;
    }});
    steps.push_back({"工具：生成可取消的候选结果", {0,"F10"}, [] { return true; }});
    steps.push_back({"工具：取消并丢弃候选结果", {0,"F10",false,true}, [weak = m_impl->bindings.artifact] {
        const auto feature = weak.lock();
        if (!feature) return false;
        const auto state = feature->GetState();
        return state.status != ArtifactStatus::Running && state.status != ArtifactStatus::Cancelling && state.status != ArtifactStatus::Ready;
    }});
#endif
    steps.push_back({"工具：输出报告", {'k',{},true}, [] { return true; }});
    return steps;
}
