// 测试用途：校验功能测试的 JSON 参数、数值和精确数据修订引用，并支持参数文件读写。
#include "JsonInput.h"
#include <QFile>
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
