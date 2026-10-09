// 测试用途：校验功能测试的 JSON 参数、数值和精确数据修订引用，并支持参数文件读写。
#include "JsonInput.h"
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>

namespace Manual {
QJsonObject GetJson(const QString& text)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::invalid_argument((QString("JSON 对象无效: ") + error.errorString()).toStdString());
    return document.object();
}

QJsonObject LoadJson(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024)
        throw std::invalid_argument("无法读取参数文件，或文件超过 8 MiB");
    return GetJson(QString::fromUtf8(file.readAll()));
}

void ExportJson(const QString& path, const QJsonObject& value)
{
    QSaveFile file(path);
    const auto bytes = QJsonDocument(value).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("JSON 文件保存失败");
}

QString GetJsonText(const QJsonObject& value)
{
    return QString::fromUtf8(QJsonDocument(value).toJson());
}

QString GetText(const QJsonObject& object, const char* key)
{
    if (!object[key].isString()) throw std::invalid_argument(std::string("需要字符串: ") + key);
    return object[key].toString();
}

double GetNumber(const QJsonObject& object, const char* key)
{
    const double value = object[key].toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!object[key].isDouble() || !std::isfinite(value))
        throw std::invalid_argument(std::string("需要有限数字: ") + key);
    return value;
}

double GetScalarMidpoint(const ImageDescriptor& image)
{
    const auto low = image.scalarRange[0], high = image.scalarRange[1];
    if (!std::isfinite(low) || !std::isfinite(high) || low > high)
        throw std::invalid_argument("当前输入没有有效的原始灰度范围");
    return low * 0.5 + high * 0.5;
}

double GetInputNumber(const QJsonObject& object, const char* key, double defaultValue)
{
    return object[key].isNull() || object[key].isUndefined() ? defaultValue : GetNumber(object, key);
}

namespace {
void CheckLinearMatrix(const std::array<double, 9>& matrix, bool rigid)
{
    auto normalized = matrix;
    for (int column = 0; column < 3; ++column) {
        const auto scale = std::hypot(matrix[column], matrix[3 + column], matrix[6 + column]);
        if (!std::isfinite(scale) || scale == 0.0)
            throw std::invalid_argument("矩阵的三个轴必须具有有限且非零的长度");
        if (rigid && std::abs(scale - 1.0) > 1e-6)
            throw std::invalid_argument("方向或刚体矩阵的各轴长度必须为 1");
        for (int row = 0; row < 3; ++row) normalized[row * 3 + column] /= scale;
    }
    const auto& a = normalized;
    const auto determinant = a[0] * (a[4] * a[8] - a[5] * a[7])
        - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-12)
        throw std::invalid_argument("矩阵不可逆，或三个轴接近共面");
    if (rigid) for (int left = 0; left < 3; ++left) for (int right = left + 1; right < 3; ++right) {
        double dot = 0.0;
        for (int row = 0; row < 3; ++row) dot += a[row * 3 + left] * a[row * 3 + right];
        if (std::abs(dot) > 1e-6) throw std::invalid_argument("方向或刚体矩阵的三个轴必须相互正交");
    }
}
}

std::array<double, 9> GetDirectionMatrix(const QJsonValue& value)
{
    const auto matrix = GetArray<double, 9>(value);
    CheckLinearMatrix(matrix, true);
    return matrix;
}

std::array<double, 16> GetAffineMatrix(const QJsonValue& value, bool rigid)
{
    const auto matrix = GetArray<double, 16>(value);
    if (matrix[12] != 0.0 || matrix[13] != 0.0 || matrix[14] != 0.0 || matrix[15] != 1.0)
        throw std::invalid_argument("4×4 仿射矩阵的最后一行必须为 0、0、0、1");
    const std::array<double, 9> linear{matrix[0],matrix[1],matrix[2],matrix[4],matrix[5],matrix[6],matrix[8],matrix[9],matrix[10]};
    CheckLinearMatrix(linear, rigid);
    if (rigid) {
        const auto determinant = linear[0] * (linear[4] * linear[8] - linear[5] * linear[7])
            - linear[1] * (linear[3] * linear[8] - linear[5] * linear[6]) + linear[2] * (linear[3] * linear[7] - linear[4] * linear[6]);
        if (determinant <= 0.0) throw std::invalid_argument("刚体位姿必须保持右手坐标方向");
    }
    return matrix;
}

std::array<int, 3> GetInputDimensions(const QString& path, const QJsonValue& value)
{
    const auto size = QFileInfo(path).size();
    if (size <= 0 || size % sizeof(float) != 0) throw std::invalid_argument("文件长度不符合 float32 RAW 输入契约");
    std::array<int,3> dimensions{};
    if (value.isNull() || value.isUndefined()) {
        const auto count = static_cast<std::uint64_t>(size) / sizeof(float);
        const auto side = static_cast<std::uint64_t>(std::llround(std::cbrt(static_cast<double>(count))));
        if (side == 0 || side > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()) || side * side * side != count)
            throw std::invalid_argument("RAW 没有尺寸头，当前文件不能按立方体推测；请提供 X、Y、Z 尺寸");
        dimensions.fill(static_cast<int>(side));
    } else dimensions = GetArray<int,3>(value);
    std::uint64_t bytes = sizeof(float);
    for (const auto side : dimensions) {
        if (side <= 0 || bytes > (std::numeric_limits<std::uint64_t>::max)() / static_cast<std::uint64_t>(side))
            throw std::invalid_argument("体素尺寸必须为正整数且不能溢出");
        bytes *= static_cast<std::uint64_t>(side);
    }
    if (bytes != static_cast<std::uint64_t>(size)) throw std::invalid_argument("尺寸与文件长度不一致，请核对 X、Y、Z 及 float32 数据类型");
    return dimensions;
}
double GetVoxelSpacing(const ImageDescriptor& image)
{
    const auto spacing = *std::min_element(image.spacing.begin(),image.spacing.end());
    if (!std::isfinite(spacing) || spacing <= 0) throw std::invalid_argument("当前输入没有有效的体素间距");
    return spacing;
}
double GetInputDiagonal(const ImageDescriptor& image)
{
    return std::hypot(image.spacing[0] * image.dims[0],image.spacing[1] * image.dims[1],image.spacing[2] * image.dims[2]);
}

bool GetBool(const QJsonObject& object, const char* key)
{
    if (!object[key].isBool()) throw std::invalid_argument(std::string("需要布尔值: ") + key);
    return object[key].toBool();
}

std::uint64_t GetId(const QJsonValue& value)
{
    // JSON double 无法精确表示所有 uint64；ID 始终使用十进制字符串。
    const auto text = value.toString();
    bool valid = false;
    const auto number = text.toULongLong(&valid);
    if (!value.isString() || !valid || !QRegularExpression("^[0-9]+$").match(text).hasMatch())
        throw std::invalid_argument("ID/revision 必须是 uint64 十进制字符串");
    return number;
}

QString GetRefText(const DataRevisionRef& value)
{
    QByteArray bytes;
    for (const auto byte : value.entityId.bytes) bytes.append(static_cast<char>(byte));
    return QString::fromLatin1(bytes.toHex()) + ":" + QString::number(value.generation);
}

DataRevisionRef GetRef(const QJsonValue& value)
{
    const auto text = value.toString();
    if (!QRegularExpression("^[0-9a-fA-F]{32}:[0-9]+$").match(text).hasMatch())
        throw std::invalid_argument("修订格式必须为 32 位十六进制实体:十进制 generation");
    const auto bytes = QByteArray::fromHex(text.left(32).toLatin1());
    DataRevisionRef result;
    for (int index = 0; index < 16; ++index) result.entityId.bytes[index] = static_cast<std::uint8_t>(bytes[index]);
    result.generation = GetId(text.mid(33));
    if (!GetDataRevisionRefValid(result)) throw std::invalid_argument("数据修订不能为空");
    return result;
}

QJsonObject GetDescriptor(const std::optional<ImageDescriptor>& value)
{
    if (!value) return {{"available", false}};
    return {{"available", true}, {"datasetId", QString::fromStdString(value->metadata.identity.datasetId)},
        {"uri", QString::fromStdString(value->metadata.source.uri)},
        {"digest", QString::fromStdString(value->metadata.source.digest.value_or(""))},
        {"byteSize", QString::number(value->metadata.source.byteSize)},
        {"dataRevision", GetRefText(value->dataRevision)}, {"bindingRevision", QString::number(value->bindingRevision)},
        {"dims", GetValues(value->dims)}, {"extent", GetValues(value->extent)},
        {"spacingRAS", GetValues(value->spacing)}, {"originRAS", GetValues(value->origin)},
        {"directionRAS", GetValues(value->direction)}, {"scalarRange", GetValues(value->scalarRange)},
        {"valueType", static_cast<int>(value->valueType)}};
}
}
