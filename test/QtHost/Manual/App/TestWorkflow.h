// 测试用途：协调功能测试中的计算占用、输入选择和交互结束，避免互斥操作并发。
#pragma once
#include "Host/VtkAppHostSession.h"
#include "Support/TestResources.h"
#include <functional>
#include <QString>
#include <QJsonObject>
#include <vector>

namespace Manual {
enum class TestPolicy { Read, View, Stop, Change, Compute, Input, Interaction };
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
    void SetSurfaceInput(DataRevisionRef source, DataRevisionRef mesh) { m_surfaceSource = source; m_surfaceMesh = mesh; }
    DataRevisionRef GetSurfaceSource() const { return m_surfaceSource; }
    DataRevisionRef GetSurfaceMesh() const { return m_surfaceMesh; }
    std::function<void(const QString&, const QString&, const QJsonObject&)> onCopyParameters;
    std::function<void(const QString&, const QString&, const QJsonObject&)> onNavigate;
    std::function<bool(const QString&, const QString&)> getActionAvailable;
    std::function<bool(const std::string&)> getRenderPending;
    std::function<bool(const std::string&)> getViewRenderPending;
    std::function<QJsonObject()> getPublishedGraph;
private:
    bool m_isClosing = false;
    std::uint64_t m_busyId = 0;
    std::vector<std::pair<QString, std::function<bool()>>> m_exits;
    DataRevisionRef m_surfaceSource;
    DataRevisionRef m_surfaceMesh;
};
}
