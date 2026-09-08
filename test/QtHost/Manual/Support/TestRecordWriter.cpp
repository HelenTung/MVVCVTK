// 测试用途：记录功能操作的提交、接纳、真实进度与终态，保留精确修订和单次完成诊断。
#include "TestRecordWriter.h"
#include <QDateTime>
#include <QJsonArray>

namespace Manual {
std::uint64_t TestRecordWriter::StartRecord(const QString& module, const QString& action,
    const QJsonObject& params, const QJsonObject& source)
{
    const auto id = m_entries.size() + 1;
    Entry entry;
    entry.clock.start();
    entry.value = {{"operationId", QString::number(id)}, {"session", "1"},
        {"module", module}, {"action", action}, {"parameters", params}, {"source", source},
        {"startedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"hostUpdatesAtStart", static_cast<double>(m_timings["Host.Update"].count)},
        {"status", "Sending"}, {"completeCount", 0}, {"isTerminal", false}};
    m_entries.push_back(std::move(entry));
    if (onChanged) onChanged(m_entries.back().value);
    return id;
}
void TestRecordWriter::SetAdmission(std::uint64_t id, const QJsonObject& admission)
{
    if (id == 0 || id > m_entries.size()) return;
    auto& entry = m_entries[id - 1];
    auto& value = entry.value;
    auto merged = value["admission"].toObject();
    const auto previous = merged;
    for (auto it = admission.begin(); it != admission.end(); ++it) merged[it.key()] = it.value();
    value["admission"] = merged;
    // 同步完成后仍保存接纳详情，但不再追加“等待结果”或重复终态日志。
    if (!value["isTerminal"].toBool() && merged["isAccepted"].toBool()) {
        value["status"] = "AcceptedPending";
        value["elapsedMs"] = QString::number(entry.clock.elapsed());
        if (merged != previous && onChanged) onChanged(value);
    }
}
void TestRecordWriter::SetProgress(std::uint64_t id, int percent)
{
    if (id == 0 || id > m_entries.size() || percent < 0 || percent > 100) return;
    auto& entry = m_entries[id - 1];
    if (entry.value["isTerminal"].toBool()) return;
    const QJsonObject progress{{"percent", percent}};
    if (entry.value["progress"].toObject() == progress) return;
    entry.value["progress"] = progress;
    entry.value["elapsedMs"] = QString::number(entry.clock.elapsed());
    if (onChanged) onChanged(entry.value);
}
void TestRecordWriter::SetComplete(std::uint64_t id, const QString& status,
    const QJsonObject& result, const QJsonObject& current)
{
    if (id == 0 || id > m_entries.size()) return;
    auto& entry = m_entries[id - 1];
    const int count = entry.value["completeCount"].toInt() + 1;
    entry.value["completeCount"] = count;
    if (count == 1) {
        entry.value["status"] = status;
        entry.value["result"] = result;
        entry.value["isTerminal"] = true;
        entry.value["elapsedMs"] = QString::number(entry.clock.elapsed());
        entry.value["hostUpdatesAtComplete"] = static_cast<double>(m_timings["Host.Update"].count);
        entry.value["current"] = current;
        const auto source = entry.value["source"].toObject();
        entry.value["isSourceChanged"] = source["dataRevision"] != current["dataRevision"];
    } else entry.value["duplicateCompletion"] = true;
    if (onChanged) onChanged(entry.value);
}
QJsonObject TestRecordWriter::GetRecord(std::uint64_t id) const
{
    return id > 0 && id <= m_entries.size() ? m_entries[id - 1].value : QJsonObject{};
}
QJsonObject TestRecordWriter::GetRecords() const
{
    QJsonArray values;
    for (const auto& entry : m_entries) values.append(entry.value);
    return {{"schema", "qt-feature-test-1"}, {"codeHead", MANUAL_CODE_HEAD},
        {"buildConfig", MANUAL_BUILD_CONFIG}, {"sourceFingerprint", MANUAL_SOURCE_FINGERPRINT}, {"records", values}, {"performance", GetTimings()}};
}
void TestRecordWriter::AddTiming(const QString& name, qint64 nanoseconds)
{
    if (!m_isTiming) return;
    auto& timing = m_timings[name]; const auto elapsed = static_cast<std::uint64_t>(qMax(qint64(0), nanoseconds));
    ++timing.count; timing.total += elapsed; timing.maximum = qMax(timing.maximum, elapsed);
    if (elapsed > 16000000) ++timing.over16ms;
}
QJsonObject TestRecordWriter::GetTimings() const
{
    QJsonObject result;
    for (const auto& entry : m_timings) {
        const auto& t = entry.second;
        result[entry.first] = QJsonObject{{"count", static_cast<double>(t.count)}, {"totalMs", t.total / 1e6},
            {"meanMs", t.count ? t.total / 1e6 / t.count : 0.0}, {"maxMs", t.maximum / 1e6}, {"over16ms", static_cast<double>(t.over16ms)}};
    }
    return result;
}
void TestRecordWriter::StopPending(const QJsonObject& current)
{
    for (std::size_t index = 0; index < m_entries.size(); ++index)
        if (!m_entries[index].value["isTerminal"].toBool())
            SetComplete(index + 1, "Stopped", {{"message", "会话已停止；此操作未在停止前观察到正常终态"}}, current);
}
}
