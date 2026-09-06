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
double Number(const std::string& text) {
    std::size_t end = 0;
    const double value = std::stod(text, &end);
    if (end != text.size() || !std::isfinite(value)) throw std::invalid_argument("工具选项数值无效：" + text);
    return value;
}
std::string Ref(const DataRevisionRef& ref) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    for (const auto byte : ref.entityId.bytes) { value += digits[byte >> 4]; value += digits[byte & 15]; }
    return value + ":" + std::to_string(ref.generation);
}
}

FeatureTestOptions GetFeatureTestOptions(const int argc, char* argv[]) {
    FeatureTestOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        const auto equal = argument.find('=');
        if (equal == std::string::npos) continue;
        const auto key = argument.substr(0, equal);
        const auto text = argument.substr(equal + 1);
        if (key == "--input") {
            if (text.empty()) throw std::invalid_argument("输入文件路径不能为空");
            options.inputPath = text;
            continue;
        }
        if (key == "--dimensions") {
            std::istringstream dimensions(text);
            std::string component;
            for (auto& dimension : options.dimensions) {
                if (!std::getline(dimensions, component, ','))
                    throw std::invalid_argument("体数据尺寸需要三个整数，例如 600,1800,600");
                const auto value = Number(component);
                if (value < 1 || value > std::numeric_limits<int>::max() || value != std::floor(value))
                    throw std::invalid_argument("体数据各轴尺寸必须为有效正整数");
                dimension = static_cast<int>(value);
            }
            if (!dimensions.eof()) throw std::invalid_argument("体数据尺寸只能包含三个整数");
            continue;
        }
        if (key == "--artifact-center") {
            const auto comma = text.find(',');
            if (comma == std::string::npos) throw std::invalid_argument("--artifact-center 需要两个以逗号分隔的索引 a,b");
            options.ringCenter = std::array<double, 2>{Number(text.substr(0, comma)), Number(text.substr(comma + 1))};
            continue;
        }
        if (key != "--tool-budget-mib" && key != "--tool-timeout-ms" && key != "--edit-radius-mm" && key != "--edit-island-voxels"
            && key != "--artifact-axis" && key != "--artifact-ring-width" && key != "--artifact-strength"
            && key != "--artifact-iterations") continue;
        const auto value = Number(text);
        if (key == "--tool-budget-mib") {
            if (value < 16 || value > 131072 || value != std::floor(value)) throw std::invalid_argument("工具内存预算必须为 16..131072 MiB");
            options.budgetBytes = static_cast<std::size_t>(value) * 1024 * 1024;
        } else if (key == "--tool-timeout-ms") {
            if (value < 1 || value > 3600000 || value != std::floor(value))
                throw std::invalid_argument("工具超时必须为 1..3600000 毫秒");
            options.timeoutMs = static_cast<std::uint32_t>(value);
        } else if (key == "--edit-radius-mm") {
            if (value <= 0) throw std::invalid_argument("编辑半径必须为正数");
            options.editRadius = value;
        } else if (key == "--edit-island-voxels") {
            if (value < 1 || value > 1e9 || value != std::floor(value)) throw std::invalid_argument("孤岛体素数量无效");
            options.islandVoxels = static_cast<std::uint64_t>(value);
        } else if (key == "--artifact-axis") {
            if (value < 0 || value > 2 || value != std::floor(value)) throw std::invalid_argument("伪影处理轴必须为 0、1 或 2");
            options.ringAxis = static_cast<int>(value);
        } else if (key == "--artifact-ring-width") {
            if (value < 1 || value > 64 || value != std::floor(value)) throw std::invalid_argument("环宽必须为 1..64");
            options.ringWidth = static_cast<int>(value);
        } else if (key == "--artifact-strength") {
            if (value < 0 || value > 1) throw std::invalid_argument("伪影校正强度必须为 0..1");
            options.ringStrength = value;
        } else {
            if (value < 1 || value > 16 || value != std::floor(value)) throw std::invalid_argument("扩散迭代次数必须为 1..16");
            options.diffusionIterations = static_cast<int>(value);
        }
    }
    return options;
}

void PrintFeatureTestHelp() {
    std::cout << "\n=== 扩展功能工具 ===\n";
#if defined(MVVCVTK_HAS_PART_SEGMENTATION)
    std::cout << "F6 / Shift+F6：切换标签编辑方式（涂绘/擦除/填充/孤岛处理/区域生长/拆分/合并；默认合并）\n"
        << "F7：计算编辑候选结果 | Ctrl+F7：确认 | Alt+F7：取消/丢弃\n"
        << "F8：预览撤销 | Shift+F8：预览重做；均使用 Ctrl+F7 确认\n"
        << "请先按 N 选择零件（可用 --part-picking 启用点击选择）；未选择时使用第一个零件。合并对象为所选零件及其下一个零件。\n"
        << "涂绘/擦除使用零件边界内的体素；填充/区域生长使用有限邻域；拆分使用两端的零件体素作为种子。\n"
        << "候选结果的统计信息会输出到控制台；确认后才更新正式显示。\n";
#endif
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    std::cout << "F9 / Shift+F9：环形伪影校正/扩散滤波/两者组合 | F10：准备候选结果 | Ctrl+F10：发布\n"
        << "Alt+F10：取消/丢弃 | Shift+F10：显示已发布的校正体数据\n"
        << "Ctrl+Shift+F10：恢复输入体数据。环形中心默认取网格中点（演示预设）。\n";
#endif
#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT)
    std::cout << "F11：已知变换的参考点系统对齐验证 | Shift+F11：已知变换的最佳拟合对齐验证\n"
        << "Ctrl+F11：显示/隐藏对齐结果 | Alt+F11：取消 | Ctrl+Shift+F11：输出报告/归档数值\n"
        << "验证使用网格采样几何，预设变换为绕 Z 轴旋转 10 度并平移 (2,-3,4)。\n"
        << "此验证检查模型单位下的几何变换恢复能力，不评估检测精度或测量点有效性。\n";
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    std::cout << "F12：全局预览网格 | Shift+F12：局部自适应网格 | Alt+F12：取消网格任务\n";
#endif
    std::cout << "Ctrl+F12：输出工具状态及结果引用 | --feature-audit：使用演示数据验证上述快捷键\n"
        << "真实数据：--input=文件路径 --dimensions=600,1800,600（原始浮点体数据）\n"
        << "工具预算默认取可用物理内存的一半，上限 64 GiB；可用 --tool-budget-mib=... 显式指定\n"
        << "编辑/伪影处理超时：--tool-timeout-ms=300000（毫秒），可按真实数据规模调整\n"
        << "选项：--edit-radius-mm=... --edit-island-voxels=2\n"
        << "         --artifact-axis=2 --artifact-center=a,b --artifact-ring-width=1\n"
        << "         --artifact-strength=0.5 --artifact-iterations=1\n" << std::flush;
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
        int key = 0;
        for (int candidate = 6; candidate <= 12; ++candidate)
            if (event.keySym == "F" + std::to_string(candidate)) key = candidate;
        if (key == 0) return {};
        auto& pressed = down[static_cast<std::size_t>(key - 6)];
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
    bool StartSurface(bool local);
    bool StartAlignment(bool bestFit);
    bool AlignmentActionRequest(int action);
    void Tick();
    VtkAppHostSession& session;
    FeatureTestBindings bindings;
    FeatureTestOptions options;
    HostViewTargets views;
    HostFeatureContext context;
    std::weak_ptr<FeatureTestControls> owner;
    std::array<bool, 7> down{};
    std::string failure;
    int editMode = 6;
    int artifactMode = 0;
    int artifactProgress = -1;
    int artifactStatus = -1;
    std::uint64_t partCompletions = 0;
    std::uint64_t alignmentCompletions = 0;
    bool alignmentMatched = false;
    std::optional<DataRevisionRef> artifactInput;
#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT)
    AlignmentMatrix expectedTransform = alignmentIdentity;
    std::optional<AlignmentResult> lastAlignment;
#endif
};

bool FeatureTestControls::Impl::Dispatch(const int key, const bool ctrl, const bool alt, const bool shift) {
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
        if (result.status != PartResultStatus::PreviewReady && result.status != PartResultStatus::Cancelled)
            tools.failure = result.message;
        tools.Status("编辑请求=" + std::to_string(result.requestId) + " | " + result.message);
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
        if (result.status != PartResultStatus::Succeeded) tools.failure = result.message;
        tools.Status("编辑确认请求=" + std::to_string(result.requestId) + " | " + result.message);
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
    Status(action == 0 ? "校正体数据已发布；按 Shift+F10 选择显示" : "伪影处理候选结果已取消/丢弃");
    Report(); return true;
#else
    (void)action; return Fail("伪影处理功能未启用");
#endif
}

bool FeatureTestControls::Impl::SelectArtifact(const bool restore) {
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    const auto feature = bindings.artifact.lock();
    const auto revision = restore ? artifactInput : feature ? feature->GetState().correctedVolume : std::nullopt;
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

bool FeatureTestControls::Impl::StartSurface(const bool local) {
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    const auto feature = bindings.surface.lock();
    if (!feature) return Fail("表面确定功能不可用");
    if (!EndCrop()) return Fail("提取网格前请先结束裁剪");
    const auto view = session.GetRenderViewState({primaryView});
    if (!view || !GetDataRevisionRefValid(view->dataRevision)) return Fail("请先加载当前图像");
    SurfaceDeterminationStartParams params;
    params.targetViews.viewIds = {primaryView};
    params.method = local ? SurfaceDeterminationMethod::LocalAdaptiveIso50 : SurfaceDeterminationMethod::GlobalIsoPreview;
    params.initialIsoValue = view->isoThreshold;
    SurfaceDeterminationRequest request;
    request.action = SurfaceDeterminationAction::Start;
    request.start = params;
    const auto admission = feature->SendRequest(request, [weak = owner](SurfaceDeterminationResult result) {
        const auto self = weak.lock();
        if (!self || !self->m_impl->context.host) return;
        auto& tools = *self->m_impl;
        if (result.status == SurfaceResultStatus::Failed) tools.failure = result.message;
        tools.Status("网格请求=" + std::to_string(result.requestId) + " 点数=" + std::to_string(result.pointCount)
            + " | " + result.message);
    });
    if (admission.status != SurfaceAdmissionStatus::Accepted) return Fail("网格提取被拒绝");
    Status(std::string(local ? "局部自适应网格" : "全局预览网格") + " 已请求 | 等值面阈值=" + std::to_string(view->isoThreshold));
    return true;
#else
    (void)local; return Fail("请使用 MVVCVTK_BUILD_SURFACE_DETERMINATION=ON 构建");
#endif
}

bool FeatureTestControls::Impl::StartAlignment(const bool bestFit) {
#if defined(MVVCVTK_HAS_METROLOGY_ALIGNMENT) && defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    const auto feature = bindings.alignment.lock();
    const auto surface = bindings.surface.lock();
    if (!feature || !surface || !context.data) return Fail("对齐或表面确定功能不可用");
    if (feature->GetState().isBusy) return Fail("对齐任务正在运行；按 Alt+F11 取消");
    const auto generation = surface->GetSurfaceSnapshot();
    if (!generation || !GetDataRevisionRefValid(generation->meshRevision)) return Fail("请先按 F12 并等待网格生成");
    const auto graph = context.data->GetDataGraph();
    const auto primary = context.data->GetDataBinding(graph, primaryVolumeBinding);
    if (!primary || primary->target != generation->sourceRevision) return Fail("网格源数据已过期；请按 F12 提取当前网格");
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

void FeatureTestControls::Impl::Tick() {
#if defined(MVVCVTK_HAS_ARTIFACT_REDUCTION)
    const auto feature = bindings.artifact.lock();
    if (!feature) return;
    const auto state = feature->GetState();
    const auto status = static_cast<int>(state.status);
    const auto progress = static_cast<int>(state.progressPercent / 10);
    if (status == artifactStatus && progress == artifactProgress) return;
    artifactStatus = status; artifactProgress = progress;
    if (state.status == ArtifactStatus::Failed && state.error != ArtifactError::Cancelled)
        failure = "伪影处理失败，错误=" + std::to_string(static_cast<int>(state.error));
    if (state.status == ArtifactStatus::Ready) Status("伪影处理候选结果已就绪 | Ctrl+F10 发布，Alt+F10 丢弃");
    Report();
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

std::vector<FeatureTestStep> FeatureTestControls::GetAuditSteps() {
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
#endif
#if defined(MVVCVTK_HAS_SURFACE_DETERMINATION)
    steps.push_back({"工具：表面网格", {0,"F12"}, [weak = m_impl->bindings.surface] {
        const auto feature = weak.lock();
        const auto result = feature ? feature->GetSurfaceSnapshot() : nullptr;
        return result && GetDataRevisionRefValid(result->meshRevision) && result->points && !result->points->empty();
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
        const auto feature = weak.lock(); return feature && feature->GetState().correctedVolume.has_value()
            && feature->GetState().qualityReport.has_value();
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
        return feature && image && feature->GetState().correctedVolume == image->dataRevision;
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
    steps.push_back({"工具：输出报告", {0,"F12",true}, [] { return true; }});
    return steps;
}
