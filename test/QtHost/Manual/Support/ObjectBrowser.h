#pragma once
#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>
#include <optional>
class QTreeWidget;
class QLineEdit;
class QLabel;
class QPushButton;
namespace Manual {
// 只保存目录投影与选择，不持有体素、网格或业务状态。
class ObjectBrowser final : public QWidget {
public:
    explicit ObjectBrowser(QWidget* parent = nullptr);
    void SetObjects(const QJsonObject& graph, const QJsonArray& regions,
        const QJsonArray& moduleNodes, const QString& current, const QStringList& roiActions,
        std::optional<double> currentOpacity = {});
    std::function<void(const QString&, const QString&, const QJsonObject&)> onAction;
private:
    void SetSelection();
    void FilterObjects();
    void SendObjectAction(const QString& id,const QString& action,const QJsonObject& overrides = {});
    QTreeWidget* m_tree = nullptr;
    QLineEdit* m_search = nullptr;
    QLabel* m_summary = nullptr;
    QPushButton* m_use = nullptr;
    QPushButton* m_show = nullptr;
    QPushButton* m_hide = nullptr;
    QJsonArray m_objects;
};
}
