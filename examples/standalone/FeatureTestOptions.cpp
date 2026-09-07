#include "FeatureTestControls.h"
#include "Host/Types/HostRequestTypes.h"
#include "Data/ImageReadTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
double Number(const std::string& text) {
    std::size_t end = 0;
    const double value = std::stod(text, &end);
    if (end != text.size() || !std::isfinite(value)) throw std::invalid_argument("工具选项数值无效：" + text);
    return value;
}

template<std::size_t Count>
std::array<double, Count> GetNumbers(const std::string& text)
{
    std::array<double, Count> values{};
    std::istringstream stream(text);
    std::string component;
    for (auto& value : values) {
        if (!std::getline(stream, component, ',') || component.empty())
            throw std::invalid_argument("输入几何分量数量不足");
        value = Number(component);
    }
    if (!stream.eof()) throw std::invalid_argument("输入几何分量数量过多");
    return values;
}
} // namespace

FeatureTestOptions GetFeatureTestOptions(const int argc, char* argv[]) {
    FeatureTestOptions options;
    std::set<std::string> inputKeys;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        const auto equal = argument.find('=');
        if (equal == std::string::npos) continue;
        const auto key = argument.substr(0, equal);
        const auto text = argument.substr(equal + 1);
        const bool isInputKey = key == "--input" || key == "--dimensions" || key == "--spacing"
            || key == "--origin" || key == "--direction" || key == "--input-frame"
            || key == "--input-unit" || key == "--input-format" || key == "--dataset-id"
            || key == "--input-digest";
        if (isInputKey && !inputKeys.insert(key).second)
            throw std::invalid_argument("重复的输入选项：" + key);
        if (key == "--spacing" || key == "--origin") {
            const auto values = GetNumbers<3>(text);
            std::array<float, 3> converted{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (std::abs(values[axis]) > std::numeric_limits<float>::max())
                    throw std::invalid_argument("输入几何超出Host浮点范围");
                converted[axis] = static_cast<float>(values[axis]);
                if (key == "--spacing" && converted[axis] <= 0)
                    throw std::invalid_argument("体素间距必须为可表示的正数");
            }
            if (key == "--spacing") options.spacing = converted;
            else options.origin = converted;
            continue;
        }
        if (key == "--direction") { options.direction = GetNumbers<9>(text); continue; }
        if (key == "--input-frame") { options.inputFrame = text; continue; }
        if (key == "--input-unit") { options.inputUnit = text; continue; }
        if (key == "--input-format") { options.inputFormat = text; continue; }
        if (key == "--dataset-id") { options.datasetId = text; continue; }
        if (key == "--input-digest") { options.inputDigest = text; continue; }
        if (key == "--input") {
            if (text.empty()) throw std::invalid_argument("输入文件路径不能为空");
            options.inputPath = text;
            continue;
        }
        if (key == "--dimensions") {
            options.hasDimensions = true;
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

HostLoadRequest GetFeatureLoadRequest(const FeatureTestOptions& options, bool isAudit)
{
    if (!options.hasDimensions || !options.spacing || !options.origin || !options.direction)
        throw std::invalid_argument("真实输入需要显式dimensions/spacing/origin/direction");
    if (options.inputFrame != "LPS" || options.inputUnit != "mm")
        throw std::invalid_argument("真实加载边界要求input-frame=LPS、input-unit=mm；内部由Host转换为RAS");
    if (options.inputFormat != "float32-le-xfastest")
        throw std::invalid_argument("当前RAW入口仅支持float32-le-xfastest单分量无头数据");
    if (options.datasetId.empty() || options.datasetId.size() > imageDatasetIdByteLimit)
        throw std::invalid_argument("真实输入需要有效dataset-id");
    if ((isAudit || !options.inputDigest.empty())
        && (options.inputDigest.size() != 64
            || !std::all_of(options.inputDigest.begin(), options.inputDigest.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            }))) throw std::invalid_argument("input-digest必须为64位SHA256；仓内审计脚本会核对实际内容");
    HostLoadRequest request;
    request.filePath = options.inputPath;
    request.geometry.dimensions = options.dimensions;
    request.geometry.spacing = *options.spacing;
    request.geometry.origin = *options.origin;
    request.geometry.direction = *options.direction;
    request.metadata.identity.datasetId = options.datasetId;
    request.metadata.source.kind = ImageSourceKind::RawFile;
    request.metadata.source.uri = request.filePath;
    if (!options.inputDigest.empty()) request.metadata.source.digest = "sha256:" + options.inputDigest;
    request.metadata.attributes = {
        {"input-coordinate-frame", std::string{"LPS"}},
        {"input-geometry-unit", std::string{"mm"}},
        {"input-storage", options.inputFormat}
    };
    // direction正交、尺寸溢出和metadata完整性仍由Host既有加载边界最终检查。
    return request;
}

bool GetFeatureInputValid(const HostLoadRequest& request, const ImageDescriptor& descriptor)
{
    if (descriptor.dims != request.geometry.dimensions || descriptor.valueType != ImageValueType::Float32
        || descriptor.componentCount != 1 || descriptor.componentBytes != sizeof(float)
        || descriptor.metadata.identity.datasetId != request.metadata.identity.datasetId
        || descriptor.metadata.source.kind != request.metadata.source.kind
        || descriptor.metadata.source.uri != request.metadata.source.uri
        || descriptor.metadata.scalar.quantity != request.metadata.scalar.quantity
        || descriptor.metadata.scalar.unit != request.metadata.scalar.unit
        || descriptor.metadata.scalar.slope != request.metadata.scalar.slope
        || descriptor.metadata.scalar.intercept != request.metadata.scalar.intercept
        || descriptor.metadata.scalar.noData != request.metadata.scalar.noData
        || descriptor.metadata.source.digest != request.metadata.source.digest
        || !GetDataRevisionRefValid(descriptor.dataRevision)) return false;
    for (const auto& expected : request.metadata.attributes) {
        const auto found = std::find_if(descriptor.metadata.attributes.begin(), descriptor.metadata.attributes.end(),
            [&](const auto& actual) { return actual.key == expected.key && actual.value == expected.value; });
        if (found == descriptor.metadata.attributes.end()) return false;
    }
    std::uint64_t expectedBytes = sizeof(float);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (descriptor.dims[axis] <= 0
            || expectedBytes > std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(descriptor.dims[axis]))
            return false;
        expectedBytes *= static_cast<std::uint64_t>(descriptor.dims[axis]);
        if (descriptor.extent[axis * 2] != 0 || descriptor.extent[axis * 2 + 1] != descriptor.dims[axis] - 1
            || descriptor.spacing[axis] != static_cast<double>(request.geometry.spacing[axis])) return false;
    }
    if (descriptor.metadata.source.byteSize != expectedBytes) return false;
    // 从8个角点核对物理位置：Host RAW加载翻转X/Y索引，再由LPS变换到RAS。
    // 这是仓内验收断言，不对请求执行第二次几何转换。
    for (int corner = 0; corner < 8; ++corner) {
        std::array<double, 3> index{}, source{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            index[axis] = (corner & (1 << axis)) ? descriptor.dims[axis] - 1 : 0;
            source[axis] = axis < 2 ? descriptor.dims[axis] - 1 - index[axis] : index[axis];
        }
        for (std::size_t row = 0; row < 3; ++row) {
            double expected = request.geometry.origin[row], actual = descriptor.origin[row];
            for (std::size_t axis = 0; axis < 3; ++axis) {
                expected += request.geometry.direction[row * 3 + axis] * request.geometry.spacing[axis] * source[axis];
                actual += descriptor.direction[row * 3 + axis] * descriptor.spacing[axis] * index[axis];
            }
            if (row < 2) expected = -expected;
            if (!std::isfinite(actual) || !std::isfinite(expected)
                || std::abs(actual - expected) > 1e-7 * std::max(1.0, std::abs(expected))) return false;
        }
    }
    return true;
}
