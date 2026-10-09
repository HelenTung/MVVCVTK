// 测试用途：协调功能测试中的计算占用、输入选择和交互结束，避免互斥操作并发。
#pragma once
#include "Host/VtkAppHostSession.h"
#include "Support/TestResources.h"
#include <functional>
#include <QString>
#include <QJsonObject>
#include <vector>
#include <map>
#if defined(MANUAL_WALL)
#include "Host/WallThicknessHostTypes.h"
#endif

namespace Manual {
enum class TestPolicy { Read, View, Stop, Change, Compute, Input, Interaction };
struct MeasurementSurface final {
    DataRevisionRef source;
    DataRevisionRef mesh;
    std::optional<double> threshold;
};
#if defined(MANUAL_WALL)
struct ThicknessInputSelection final {
    std::optional<DataRevisionRef> source, labels, mesh;
    std::vector<std::uint64_t> materialLabels;
    ThicknessUnit unit = ThicknessUnit::Millimeter;
};
#endif
class TestWorkflow final {
public:
    TestResources resources;
    QJsonObject partEditContext;
    bool StartOperation(std::uint64_t id, TestPolicy policy, bool exitTools, const QString& module = {});
    void SetComplete(std::uint64_t id);
    void Stop() { m_isClosing = true; }
    bool GetIsClosing() const { return m_isClosing; }
    std::uint64_t GetBusyOperation() const { return m_busyId; }
    void AttachExit(QString module, std::function<bool()> exit) { m_exits.emplace_back(std::move(module), std::move(exit)); }
    std::optional<MeasurementSurface> GetMeasurementSurface() const
    { return getMeasurementSurface ? getMeasurementSurface() : std::nullopt; }
    QJsonObject GetPublishedGraph(VtkAppHostSession& session);
#if defined(MANUAL_WALL)
    ThicknessStartRequest BuildThicknessRequest(VtkAppHostSession& session,
        const ThicknessInputSelection& selection, ThicknessParams params) const;
#endif
    std::function<void(const QString&, const QString&, const QJsonObject&)> onCopyParameters;
    std::function<void(const QString&, const QString&, const QJsonObject&)> onNavigate;
    std::function<bool(const QString&, const QString&)> getActionAvailable;
    std::function<bool(const std::string&)> getRenderPending;
    std::function<bool(const std::string&)> getViewRenderPending;
    std::function<QJsonObject()> getPublishedGraph;
    std::function<QJsonObject()> getViewTransforms;
    std::function<DataRevisionRef()> getPartLabels;
    std::function<std::optional<MeasurementSurface>()> getMeasurementSurface;
private:
    bool m_isClosing = false;
    std::uint64_t m_busyId = 0;
    std::vector<std::pair<QString, std::function<bool()>>> m_exits;
    std::map<QString, std::uint64_t> m_catalogOrder;
};
}
