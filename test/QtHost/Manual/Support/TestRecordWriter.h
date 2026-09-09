// 测试用途：记录功能操作的提交、接纳、真实进度与终态，保留精确修订和单次完成诊断。
#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <vector>
#include <map>

namespace Manual {
class TestRecordWriter final {
public:
    std::uint64_t StartRecord(const QString& module, const QString& action,
        const QJsonObject& params, const QJsonObject& source);
    void SetAdmission(std::uint64_t id, const QJsonObject& admission);
    void SetProgress(std::uint64_t id, int percent);
    void SetComplete(std::uint64_t id, const QString& status,
        const QJsonObject& result, const QJsonObject& current);
    QJsonObject GetRecord(std::uint64_t id) const;
    QJsonObject GetRecords() const;
    void AddTiming(const QString& name, qint64 nanoseconds);
    QJsonObject GetTimings() const;
    void StartTiming() { m_timings.clear(); m_isTiming = true; }
    void StopTiming() { m_isTiming = false; }
    void StopPending(const QJsonObject& current);
    std::function<void(const QJsonObject&)> onChanged;
private:
    struct Entry { QJsonObject value; QElapsedTimer clock; };
    std::vector<Entry> m_entries;
    struct Timing { std::uint64_t count = 0, total = 0, maximum = 0, over16ms = 0; };
    std::map<QString, Timing> m_timings;
    bool m_isTiming = true;
};
// 只在已发生的事件中计时；固定大小累计统计，不投递事件、不写逐帧日志。
class TestTiming final {
public:
    TestTiming(TestRecordWriter& records, QString name) : m_records(records), m_name(std::move(name)) { m_clock.start(); }
    ~TestTiming() { m_records.AddTiming(m_name, m_clock.nsecsElapsed()); }
private:
    TestRecordWriter& m_records;
    QString m_name;
    QElapsedTimer m_clock;
};
}
