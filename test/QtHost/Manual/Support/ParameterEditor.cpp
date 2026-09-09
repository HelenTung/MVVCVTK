// 测试用途：提供中文结构化参数控件；多值逐格输入，多组数据逐行增删，不要求手写 JSON。
#include "ParameterEditor.h"
#include "UiText.h"
#include "PathInput.h"
#if defined(MANUAL_ALIGNMENT)
#include "Modules/AlignmentInput.h"
#endif
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QSet>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Manual {
namespace {
class BooleanCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
protected:
    // 半选只表示“不修改”；正常点击始终在 true/false 之间切换，不进入省略状态。
    void nextCheckState() override { setCheckState(checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked); }
};
bool IsList(const QString& key)
{
    static const QSet<QString> keys{"sourcePointsMM", "seeds", "barriers", "parts", "protectedParts", "overwriteParts",
        "indexBoxes", "initialPoses", "colorNodes", "opacityNodes", "geometries", "constraints", "fitPairs", "vertexIds"};
    return keys.contains(key);
}
QJsonValue Shape(const QString& key)
{
    if (key == "worldCenter" || key == "seedModelPoint" || key == "targetNormal") return QJsonArray{0,0,0};
    if (key == "evaluationBounds" || key == "extent" || key == "roiModelBounds" || key == "targetBounds") return QJsonArray{0,0,0,0,0,0};
    if (key == "windowLevel" || key == "radiusRange") return QJsonArray{0,1};
    if (key == "centerIndex") return QJsonArray{0,0};
    if (key == "colorRGBA") return QJsonArray{1,1,1,1};
    if (key == "transfer") return QJsonObject{{"colorNodes", QJsonArray{}}, {"opacityNodes", QJsonArray{}}};
    if (key == "slice") return QJsonObject{{"origin", QJsonArray{0,0,0}}, {"normal", QJsonArray{0,0,1}}, {"thicknessMM", 1.0}};
    if (key == "axes" || (key.startsWith("is") && key.size() > 2 && key[2].isUpper()) || key == "planes" || key == "crosshair" || key == "ruler") return false;
    if (key == "iso" || key == "opacity" || key == "angleDeg" || key == "minimumContrast" || key == "initialIsoValue"
        || key.startsWith("profile") || key == "maximumOffsetModel" || key == "minPairNormalCosine") return 0.0;
#if defined(MANUAL_ALIGNMENT)
    if (key == "recipe") return GetReferenceTemplate(AlignmentMethod::Rps)["recipe"];
#endif
    return QString();
}
QJsonValue RowTemplate(const QString& action, const QString& key, const QJsonArray& values)
{
    if (!values.isEmpty()) return values.first();
    if (key == "seeds") return action == "Split" ? QJsonValue(QJsonObject{{"imageIndex", QJsonArray{0,0,0}}, {"target", "1"}}) : QJsonValue(QJsonArray{0,0,0});
    if (key == "sourcePointsMM") return QJsonArray{0,0,0};
    if (key == "barriers") return QJsonObject{{"imageIndex", QJsonArray{0,0,0}}, {"axis", 0}};
    if (key == "indexBoxes") return QJsonArray{0,0,0,0,0,0};
    if (key == "colorNodes") return QJsonArray{0,1,1,1};
    if (key == "opacityNodes") return QJsonArray{0,1};
    if (key == "initialPoses") return QJsonArray{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (key == "parts" || key == "protectedParts" || key == "overwriteParts") return QString("selected");
    if (key == "vertexIds") return QString("0");
#if defined(MANUAL_ALIGNMENT)
    if (key == "geometries" || key == "constraints") return GetReferenceTemplate(AlignmentMethod::Rps)["recipe"].toObject()[key].toArray().first();
    if (key == "fitPairs") return GetReferenceTemplate(AlignmentMethod::ConstrainedBestFit)["recipe"].toObject()[key].toArray().first();
#endif
    return QString();
}
QString Component(const QString& key, int index, int size)
{
    if (size == 9 || size == 16) { const int columns = size == 9 ? 3 : 4; return QString("%1 行 %2 列").arg(index/columns+1).arg(index%columns+1); }
    if (key == "windowLevel") return index == 0 ? "窗宽" : "窗位";
    if (key == "colorNodes") return QStringList{"灰度", "红", "绿", "蓝"}.value(index);
    if (key == "opacityNodes") return index == 0 ? "灰度" : "透明度";
    if (key == "colorRGBA") return QStringList{"红", "绿", "蓝", "透明度"}.value(index);
    if (key == "centerIndex") return QString("截面轴 %1").arg(index+1);
    if (size == 6) return QStringList{"X 最小", "X 最大", "Y 最小", "Y 最大", "Z 最小", "Z 最大"}.value(index);
    if (size == 3) return QStringList{"X", "Y", "Z"}.value(index);
    if (size == 2) return index == 0 ? "最小值" : "最大值";
    return QString("值 %1").arg(index+1);
}
bool IsPath(const QString& key)
{
    return key == "filePath" || key == "plyPath" || key == "outputPath" || key == "outputDir" || key == "archivePath";
}
}
ParameterEditor::ParameterEditor(QString module, QString action, QString key, QJsonValue value, QJsonValue schema,
    QWidget* parent, bool listItem, QString title)
    : QWidget(parent), m_module(std::move(module)), m_action(std::move(action)), m_key(std::move(key)),
      m_title(title.isEmpty() ? GetParameterText(m_key) : std::move(title)), m_schema(std::move(schema)),
      m_listItem(listItem), m_optional(m_schema.isNull() && !m_key.isEmpty())
{
    m_boolean = m_schema.isBool() || (m_schema.isNull() && Shape(m_key).isBool());
    setObjectName(m_key); setAccessibleName(m_title);
    m_layout = new QVBoxLayout(this); m_layout->setContentsMargins(0,0,0,0); m_layout->setSpacing(4);
    if (!m_key.isEmpty()) {
        auto* heading = new QHBoxLayout;
        auto* label = new QLabel(m_title, this); label->setObjectName("parameterTitle"); label->setWordWrap(true); label->setToolTip(GetParameterHelp(m_key)); heading->addWidget(label, 1);
        if (m_optional && !m_boolean) {
            m_specified = new QCheckBox("提供参数", this); m_specified->setObjectName("specified");
            m_specified->setToolTip("取消仅表示不提供此参数，由业务使用默认值或保留原值；不会清零或设置 false。"); heading->addWidget(m_specified);
            connect(m_specified, &QCheckBox::toggled, this, [this](bool enabled) {
                if (m_body) { m_body->setEnabled(enabled); if (m_type == QJsonValue::Object) m_body->setVisible(enabled); }
                NotifyEdited();
            });
        }
        m_layout->addLayout(heading);
    }
    SetValue(value);
}
bool ParameterEditor::GetFieldApplicable(const QString& key) const
{
    if (m_key.isEmpty() && m_module == "View" && m_action == "Set"
        && (key == "mode" || key == "iso" || key == "quality" || key == "transfer")) {
        const auto target = m_fields.find("viewId");
        return target == m_fields.end() || !target->second->GetValue().toString().startsWith("slice-");
    }
    if (m_key.isEmpty() && m_module == "Surface" && key == "seedModelPoint") {
        const auto selection = m_fields.find("componentSelection");
        return selection != m_fields.end() && selection->second->GetValue() == "Seeded";
    }
    return true;
}
void ParameterEditor::SetFieldVisibility()
{
    if (!m_key.isEmpty()) return;
    const auto bound = GetBoundParameters(m_module, m_action);
    for (const auto& field : m_fields) field.second->setVisible(!bound.contains(field.first) && GetFieldApplicable(field.first));
}
void ParameterEditor::NotifyEdited() { SetFieldVisibility(); if (onEdited) onEdited(); }
void ParameterEditor::UpdateBooleanState()
{
    if (!m_booleanState) return;
    auto* input = qobject_cast<QCheckBox*>(m_input);
    if (m_stateBound) {
        if (!m_booleanEdited) {
            const QSignalBlocker blocker(input);
            input->setCheckState(m_appliedBoolean.isNull() ? Qt::PartiallyChecked : m_appliedBoolean.toBool() ? Qt::Checked : Qt::Unchecked);
        }
        input->setEnabled(m_booleanUnavailable.isEmpty());
        const QString applied = !m_booleanUnavailable.isEmpty() ? m_booleanUnavailable : m_appliedBoolean.isNull() ? "各对象状态不同" : m_appliedBoolean.toBool() ? "当前开启" : "当前关闭";
        m_booleanState->setText(m_booleanEdited ? QString("待应用：%1 · %2").arg(input->isChecked() ? "开启" : "关闭", applied) : applied);
        m_unsetBoolean->setText("撤回修改"); m_unsetBoolean->setEnabled(m_booleanEdited);
    } else {
        m_booleanState->setText(!m_booleanSpecified ? "不修改" : input->isChecked() ? "开启" : "关闭");
        m_unsetBoolean->setText("不修改"); m_unsetBoolean->setEnabled(m_booleanSpecified);
    }
}
void ParameterEditor::CreateBooleanStateControls()
{
    if (m_booleanState) return;
    auto* layout = static_cast<QHBoxLayout*>(m_body->layout());
    m_booleanState = new QLabel(m_body); m_booleanState->setObjectName("booleanState"); m_booleanState->setWordWrap(true);
    m_unsetBoolean = new QPushButton("不修改", m_body); m_unsetBoolean->setObjectName("unsetBoolean");
    layout->addWidget(m_booleanState); layout->addStretch(); layout->addWidget(m_unsetBoolean);
    connect(m_unsetBoolean, &QPushButton::clicked, this, [this] {
        auto* input = qobject_cast<QCheckBox*>(m_input); const QSignalBlocker blocker(input);
        if (!m_stateBound) input->setCheckState(Qt::PartiallyChecked);
        m_booleanEdited = false; m_booleanSpecified = false; UpdateBooleanState(); NotifyEdited();
    });
}
void ParameterEditor::SetAppliedBoolean(QJsonValue value, const QString& context, const QString& unavailableReason)
{
    if (!m_boolean || !m_input) return;
    if (!value.isBool()) value = QJsonValue();
    if (!m_stateBound || m_stateContext != context || !unavailableReason.isEmpty()) m_booleanEdited = false;
    m_booleanUnavailable = unavailableReason;
    m_stateBound = true; m_stateContext = context; m_appliedBoolean = value;
    if (m_booleanEdited && value.isBool() && qobject_cast<QCheckBox*>(m_input)->isChecked() == value.toBool()) m_booleanEdited = false;
    CreateBooleanStateControls(); UpdateBooleanState();
}
void ParameterEditor::SetValue(const QJsonValue& value)
{
    if (m_boolean && !value.isNull() && !value.isBool()) throw std::invalid_argument((m_title + "：请输入布尔值").toStdString());
    auto effective = value;
    if (effective.isNull()) effective = m_schema.isNull() ? Shape(m_key) : m_schema;
    if (m_body) { m_layout->removeWidget(m_body); delete m_body; }
    m_fields.clear(); m_rows.clear(); m_input = nullptr; m_rowLayout = nullptr;
    m_booleanState = nullptr; m_unsetBoolean = nullptr; m_booleanSpecified = !value.isNull();
    if (m_stateBound) m_booleanEdited = value.isBool();
    m_body = new QWidget(this); m_layout->addWidget(m_body);
    m_type = effective.type();
    BuildValue(effective);
    UpdateBooleanState();
    SetFieldVisibility();
    if (m_specified) {
        m_specified->setChecked(!value.isNull()); m_body->setEnabled(!value.isNull());
        if (m_type == QJsonValue::Object) m_body->setVisible(!value.isNull());
    }
}
void ParameterEditor::BuildValue(const QJsonValue& value)
{
    if (value.isObject()) {
        auto* grid = new QGridLayout(m_body); grid->setContentsMargins(0,0,0,0); grid->setHorizontalSpacing(12); grid->setVerticalSpacing(10);
        const auto object = value.toObject(); auto keys = object.keys();
        const QStringList first{"filePath", "viewScope", "viewId", "target", "seed", "seeds", "sourcePointsMM", "ring", "diffusion", "componentSelection", "initialIsoValue", "seedModelPoint", "roiModelBounds", "world", "worldAxis", "worldCenter", "dimensions", "spacingLPS", "originLPS", "directionLPS"};
        for (auto i = first.crbegin(); i != first.crend(); ++i) if (keys.removeOne(*i)) keys.prepend(*i);
        int row = 0, column = 0;
        const bool seedRow = m_listItem && object.contains("imageIndex") && object.size() == 2;
        for (const auto& key : keys) {
            const auto v = object[key]; const auto prototype = m_schema.isObject() && m_schema.toObject().contains(key) ? m_schema.toObject()[key] : v;
            auto* field = new ParameterEditor(m_module, m_action, key, v, prototype, m_body, false,
                seedRow && key == "target" ? "新标签编号" : QString());
            field->onEdited = [this] { NotifyEdited(); }; m_fields[key] = field;
            if (m_key.isEmpty() && GetBoundParameters(m_module, m_action).contains(key)) { field->hide(); continue; }
            if (seedRow) { grid->addWidget(field, 0, key == "imageIndex" ? 0 : 3, 1, key == "imageIndex" ? 3 : 1); continue; }
            const bool wide = v.isArray() || v.isObject() || (v.isNull() && (Shape(key).isArray() || Shape(key).isObject())) || IsPath(key);
            if (wide && column) { ++row; column = 0; }
            grid->addWidget(field, row, column, 1, wide ? 2 : 1);
            if (wide || column == 1) { ++row; column = 0; } else column = 1;
        }
        if (seedRow) { grid->setColumnStretch(0, 1); grid->setColumnStretch(1, 1); grid->setColumnStretch(2, 1); grid->setColumnStretch(3, 1); }
        else { grid->setColumnStretch(0, 1); grid->setColumnStretch(1, 1); }
        return;
    }
    if (value.isArray()) {
        const auto array = value.toArray();
        m_isList = !m_listItem && (IsList(m_key) || array.isEmpty() || (!array.first().isDouble() && !array.first().isString()));
        if (m_isList) {
            auto* layout = new QVBoxLayout(m_body); layout->setContentsMargins(0,0,0,0); layout->setSpacing(6);
            auto* rows = new QWidget(m_body); m_rowLayout = new QVBoxLayout(rows); m_rowLayout->setContentsMargins(0,0,0,0); m_rowLayout->setSpacing(8); layout->addWidget(rows);
            const auto prototype = m_schema.isArray() ? m_schema.toArray() : array;
            m_itemTemplate = RowTemplate(m_action, m_key, prototype);
            for (const auto item : array) AddRow(item);
            auto* add = new QPushButton("添加一行", m_body); add->setObjectName("addRow"); layout->addWidget(add, 0, Qt::AlignLeft);
            connect(add, &QPushButton::clicked, this, [this] { AddRow(m_itemTemplate); NotifyEdited(); });
        } else {
            auto* grid = new QGridLayout(m_body); grid->setContentsMargins(0,0,0,0); grid->setSpacing(8);
            const int columns = array.size() == 16 || array.size() == 4 ? 4 : array.size() == 2 || array.size() == 6 ? 2 : 3;
            for (int i = 0; i < array.size(); ++i) {
                auto* field = new ParameterEditor(m_module, m_action, QString::number(i), array[i], array[i], m_body, true, Component(m_key, i, array.size()));
                field->onEdited = [this] { NotifyEdited(); }; grid->addWidget(field, i/columns, i%columns);
                m_rows.emplace_back(field, field); grid->setColumnStretch(i%columns, 1);
            }
        }
        return;
    }
    auto* layout = new QHBoxLayout(m_body); layout->setContentsMargins(0,0,0,0);
    auto choices = GetParameterChoices(m_module, m_key);
    if (m_module == "Alignment" && m_key == "kind") {
        if (value == "Hard" || value == "Soft" || value == "Check") choices = {{"Hard", "硬约束"}, {"Soft", "软约束"}, {"Check", "校验约束"}};
        else choices = {{"Point", "点"}, {"Line", "直线"}, {"Plane", "平面"}, {"Circle", "圆"}, {"Sphere", "球"}, {"Cylinder", "圆柱"}};
    }
    if (!choices.isEmpty() && value.isString()) {
        auto* input = new QComboBox(m_body); input->setObjectName("value");
        for (const auto& choice : choices) input->addItem(choice.second, choice.first);
        int index = input->findData(value.toString());
        if (index < 0) { input->addItem(value.toString().isEmpty() ? "请选择" : "自定义：" + value.toString(), value.toString()); index = input->count()-1; }
        input->setCurrentIndex(index); m_input = input;
        connect(input, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { NotifyEdited(); });
    } else if (value.isBool()) {
        auto* input = new BooleanCheckBox("启用", m_body); input->setObjectName("value");
        input->setCheckState(m_optional && !m_booleanSpecified ? Qt::PartiallyChecked : value.toBool() ? Qt::Checked : Qt::Unchecked); m_input = input;
        input->setToolTip("勾选发送 true；取消勾选发送 false。");
        connect(input, &QCheckBox::stateChanged, this, [this](int state) { m_booleanSpecified = state != Qt::PartiallyChecked; m_booleanEdited = true; UpdateBooleanState(); NotifyEdited(); });
        if (m_optional || m_stateBound) {
            layout->addWidget(input); CreateBooleanStateControls();
            input->setAccessibleName(m_title);
            return;
        }
    } else {
        QLineEdit* input = IsPath(m_key) ? static_cast<QLineEdit*>(new PathInput(m_key == "outputDir", m_body)) : new QLineEdit(m_body);
        input->setObjectName("value"); input->setMinimumWidth(35);
        input->setText(value.isString() ? value.toString() : QString::number(value.toDouble(), 'g', 17));
        m_input = input; input->setAccessibleName(m_title); connect(input, &QLineEdit::textEdited, this, [this] { NotifyEdited(); });
        if (IsPath(m_key)) {
            static_cast<PathInput*>(input)->onPathChanged = [this](const QString&) { NotifyEdited(); };
            auto* browse = new QPushButton("选择…", m_body); layout->addWidget(input, 1); layout->addWidget(browse);
            connect(browse, &QPushButton::clicked, this, [this, input] {
                const auto path = m_key == "outputDir" ? QFileDialog::getExistingDirectory(this, "选择目录", input->text())
                    : m_key == "outputPath" ? QFileDialog::getSaveFileName(this, "选择输出文件", input->text()) : QFileDialog::getOpenFileName(this, "选择文件", input->text());
                if (!path.isEmpty()) { input->setText(path); NotifyEdited(); }
            });
            return;
        }
    }
    layout->addWidget(m_input, 1);
    m_input->setAccessibleName(m_title);
}
void ParameterEditor::AddRow(const QJsonValue& value)
{
    auto* row = new QWidget(m_body); auto* layout = new QHBoxLayout(row); layout->setContentsMargins(0,0,0,0);
    auto* editor = new ParameterEditor(m_module, m_action, m_key, value, m_itemTemplate, row, true, "第 " + QString::number(m_rows.size()+1) + " 组");
    editor->onEdited = [this] { NotifyEdited(); };
    auto* remove = new QPushButton("删除", row); remove->setObjectName("removeRow"); remove->setFixedWidth(55);
    layout->addWidget(editor, 1); layout->addWidget(remove, 0, Qt::AlignTop); m_rowLayout->addWidget(row); m_rows.emplace_back(row, editor);
    connect(remove, &QPushButton::clicked, this, [this, row] {
        m_rows.erase(std::remove_if(m_rows.begin(), m_rows.end(), [row](const auto& item) { return item.first == row; }), m_rows.end());
        m_rowLayout->removeWidget(row); row->hide(); row->deleteLater();
        for (std::size_t i = 0; i < m_rows.size(); ++i) {
            auto* editor = m_rows[i].second; editor->m_title = "第 " + QString::number(i+1) + " 组";
            editor->setAccessibleName(editor->m_title);
            editor->findChild<QLabel*>("parameterTitle", Qt::FindDirectChildrenOnly)->setText(editor->m_title);
        }
        NotifyEdited();
    });
}
QJsonValue ParameterEditor::GetValue() const
{
    if (m_boolean && m_stateBound && !m_booleanEdited) return m_appliedBoolean;
    if (m_boolean && m_optional && !m_booleanSpecified) return {};
    if (m_specified && !m_specified->isChecked()) return {};
    if (m_type == QJsonValue::Object) {
        QJsonObject value;
        for (const auto& field : m_fields) {
            if (!GetFieldApplicable(field.first)) continue;
            const auto v = field.second->GetValue();
            value[field.first] = v;
        }
        return value;
    }
    if (m_type == QJsonValue::Array) { QJsonArray value; for (const auto& row : m_rows) value.append(row.second->GetValue()); return value; }
    if (auto* input = qobject_cast<QComboBox*>(m_input)) return input->currentData().toString();
    if (auto* input = qobject_cast<QCheckBox*>(m_input)) return input->isChecked();
    const auto text = static_cast<QLineEdit*>(m_input)->text();
    if (m_type == QJsonValue::String) return text;
    bool valid = false; const double number = text.toDouble(&valid);
    if (!valid || !std::isfinite(number)) { m_input->setFocus(); throw std::invalid_argument((m_title + "：请输入有效数字").toStdString()); }
    return number;
}
void ParameterEditor::SetPatch(const QJsonObject& patch)
{
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const auto field = m_fields.find(it.key());
        if (field == m_fields.end()) throw std::invalid_argument(("未知参数：" + it.key()).toStdString());
        field->second->SetValue(it.value());
    }
    SetFieldVisibility();
}
ParameterEditor* ParameterEditor::GetField(const QString& key) const { const auto it = m_fields.find(key); return it == m_fields.end() ? nullptr : it->second; }
ParameterEditor* ParameterEditor::GetElement(int index) const { return index >= 0 && index < static_cast<int>(m_rows.size()) ? m_rows[index].second : nullptr; }
int ParameterEditor::GetCount() const { return static_cast<int>(m_rows.size()); }
bool ParameterEditor::GetHasInputs() const
{
    const auto bound = GetBoundParameters(m_module, m_action);
    return std::any_of(m_fields.begin(), m_fields.end(), [&bound](const auto& field) { return !bound.contains(field.first); });
}
}
