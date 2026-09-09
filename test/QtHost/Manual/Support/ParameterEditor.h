// 测试用途：将对象、坐标、矩阵和可变列表编辑为独立字段，保留原始请求的数据类型与未完成输入。
#pragma once
#include <QWidget>
#include <QJsonValue>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>
#include <map>
#include <vector>
class QVBoxLayout;
class QGridLayout;
class QCheckBox;
class QLabel;
class QPushButton;
namespace Manual {
class ParameterEditor final : public QWidget {
public:
    ParameterEditor(QString module, QString action, QString key, QJsonValue value, QJsonValue schema,
        QWidget* parent = nullptr, bool listItem = false, QString title = {});
    QJsonValue GetValue() const;
    void SetValue(const QJsonValue& value);
    void SetPatch(const QJsonObject& patch);
    ParameterEditor* GetField(const QString& key) const;
    ParameterEditor* GetElement(int index) const;
    int GetCount() const;
    bool GetHasInputs() const;
    void SetFieldApplicability(QJsonObject fields);
    void SetAppliedBoolean(QJsonValue value, const QString& context, const QString& unavailableReason = {});
    bool GetIsStateBound() const { return m_stateBound; }
    QJsonValue GetAppliedBoolean() const { return m_appliedBoolean; }
    std::function<void()> onEdited;
private:
    void BuildValue(const QJsonValue& value);
    void AddRow(const QJsonValue& value);
    void NotifyEdited();
    void UpdateBooleanState();
    void CreateBooleanStateControls();
    bool GetFieldApplicable(const QString& key) const;
    void SetFieldVisibility();
    QString m_module, m_action, m_key, m_title;
    QJsonValue m_schema, m_itemTemplate;
    QJsonObject m_applicableFields;
    QJsonValue::Type m_type = QJsonValue::Null;
    bool m_listItem = false, m_isList = false, m_optional = false, m_boolean = false, m_booleanSpecified = false;
    bool m_stateBound = false, m_booleanEdited = false;
    QJsonValue m_appliedBoolean;
    QString m_stateContext, m_booleanUnavailable;
    QVBoxLayout* m_layout = nullptr;
    QCheckBox* m_specified = nullptr;
    QLabel* m_booleanState = nullptr;
    QPushButton* m_unsetBoolean = nullptr;
    QWidget* m_body = nullptr;
    QWidget* m_input = nullptr;
    QVBoxLayout* m_rowLayout = nullptr;
    std::map<QString, ParameterEditor*> m_fields;
    std::vector<std::pair<QWidget*, ParameterEditor*>> m_rows;
};
}
