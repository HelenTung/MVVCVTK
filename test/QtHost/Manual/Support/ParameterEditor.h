// 测试用途：将对象、坐标、矩阵和可变列表编辑为独立字段，保留原始请求的数据类型与未完成输入。
#pragma once
#include <QWidget>
#include <QJsonValue>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>
#include <map>
#include <vector>
#include "UiText.h"
class QVBoxLayout;
class QGridLayout;
class QCheckBox;
class QLabel;
class QPushButton;
class QComboBox;
namespace Manual {
class ParameterEditor final : public QWidget {
public:
    ParameterEditor(QString module, QString action, QString key, QJsonValue value, QJsonValue schema,
        QWidget* parent = nullptr, bool listItem = false, QString title = {}, bool showTitle = true, bool deferFields = false);
    bool GetFieldsBuilt() const { return !m_fieldsDeferred; }
    // 在卡片显示前按需建立 Qt 字段，避免边显示边构建触发整棵可见控件树的反复布局。
    void BuildFields();
    QJsonValue GetValue() const;
    void SetValue(const QJsonValue& value);
    void SetPatch(const QJsonObject& patch);
    ParameterEditor* GetField(const QString& key) const;
    ParameterEditor* GetElement(int index) const;
    int GetCount() const;
    bool GetHasInputs() const;
    void SetFieldApplicability(QJsonObject fields);
    void SetAppliedBoolean(QJsonValue value, const QString& context, const QString& unavailableReason = {});
    void SetDefaultValue(const QJsonValue& value);
    void SetReferenceChoices(const ParameterChoices& choices);
    void SetRegionCreator(std::function<void()> onCreate);
    void SetAllowedChoices(const ParameterChoices& choices);
    bool GetIsStateBound() const { return m_stateBound; }
    QJsonValue GetAppliedBoolean() const { return m_appliedBoolean; }
    std::function<void()> onEdited;
protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
private:
    void BuildValue(const QJsonValue& value);
    void AddRow(const QJsonValue& value);
    void NotifyEdited();
    void UpdateBooleanState();
    void CreateBooleanStateControls();
    bool GetFieldApplicable(const QString& key) const;
    void SetFieldVisibility();
    void SetPropertyLayout();
    void SetInheritedGeometry();
    QString m_module, m_action, m_key, m_title;
    QJsonValue m_schema, m_itemTemplate;
    QJsonValue m_deferredValue;
    bool m_fieldsDeferred=false;
    QJsonObject m_applicableFields;
    QJsonValue::Type m_type = QJsonValue::Null;
    bool m_listItem = false, m_isList = false, m_optional = false, m_boolean = false, m_booleanSpecified = false;
    bool m_stateBound = false, m_booleanEdited = false;
    QJsonValue m_appliedBoolean;
    QString m_stateContext, m_booleanUnavailable;
    QGridLayout* m_layout = nullptr;
    bool m_isPropertyRow = false;
    bool m_isVector = false, m_vectorBelow = false;
    int m_nameColumnWidth = 0, m_appliedNameWidth = -1;
    int m_bodyRow = 0;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_unitLabel = nullptr;
    QCheckBox* m_specified = nullptr;
    QLabel* m_booleanState = nullptr;
    QPushButton* m_unsetBoolean = nullptr;
    QWidget* m_body = nullptr;
    QLabel* m_inheritedGeometry = nullptr;
    QWidget* m_input = nullptr;
    QComboBox* m_parameterSource = nullptr;
    bool m_isManualSource = false;
    QPushButton* m_createRegion = nullptr;
    std::function<void()> m_onCreateRegion;
    QVBoxLayout* m_rowLayout = nullptr;
    std::map<QString, QWidget*> m_groupPages;
    std::map<QString, QWidget*> m_groupSections;
    std::map<QString, ParameterEditor*> m_fields;
    std::vector<std::pair<QWidget*, ParameterEditor*>> m_rows;
};
}
