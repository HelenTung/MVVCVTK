// 测试用途：校验功能测试的 JSON 参数、数值和精确数据修订引用，并支持参数文件读写。
#pragma once

#include "Data/ImageReadTypes.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Manual {
QJsonObject GetJson(const QString& text);
QJsonObject LoadJson(const QString& path);
void ExportJson(const QString& path, const QJsonObject& value);
QString GetText(const QJsonObject& object, const char* key);
double GetNumber(const QJsonObject& object, const char* key);
bool GetBool(const QJsonObject& object, const char* key);
std::uint64_t GetId(const QJsonValue& value);
DataRevisionRef GetRef(const QJsonValue& value);
QString GetRefText(const DataRevisionRef& value);
QJsonObject GetDescriptor(const std::optional<ImageDescriptor>& value);
QString GetJsonText(const QJsonObject& value);

template<class T, std::size_t N>
std::array<T, N> GetArray(const QJsonValue& value)
{
    if (!value.isArray() || value.toArray().size() != N)
        throw std::invalid_argument("数组长度不符合当前操作要求");
    const auto values = value.toArray();
    std::array<T, N> result{};
    for (int index = 0; index < static_cast<int>(N); ++index) {
        const auto number = values[index].toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!values[index].isDouble() || !std::isfinite(number)
            || number < static_cast<double>(std::numeric_limits<T>::lowest())
            || number > static_cast<double>(std::numeric_limits<T>::max())
            || (std::is_integral_v<T> && (std::trunc(number) != number
                || std::abs(number) > 9007199254740991.0)))
            throw std::invalid_argument("数组元素类型或范围无效");
        result[index] = static_cast<T>(number);
    }
    return result;
}

template<class T, std::size_t N>
QJsonArray GetValues(const std::array<T, N>& values)
{
    QJsonArray result;
    for (const auto value : values) result.append(static_cast<double>(value));
    return result;
}

template<class T>
T GetEnum(const QJsonObject& object, const char* key,
    std::initializer_list<std::pair<const char*, T>> values)
{
    const auto text = GetText(object, key);
    for (const auto& value : values)
        if (text == value.first) return value.second;
    throw std::invalid_argument((QString("无效选项 ") + key + ": " + text).toStdString());
}
}
