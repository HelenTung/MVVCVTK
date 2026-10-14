#include "ColorLegendEditor.h"
#include "ParameterEditor.h"
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
namespace Manual {
ColorLegendEditor::ColorLegendEditor(QString module, QString action, QJsonObject parameters,
                                     std::function<void(QJsonObject)> submit, QWidget *parent)
    : QDialog(parent, Qt::Tool) {
    const auto ref = parameters[module == "Alignment" ? "deviation" : "result"].toString();
    setWindowTitle("完整色环 · " + ref.section(':', 0, 0).right(8));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(390, 570);
    auto *layout = new QVBoxLayout(this);
    auto *structure = new QLabel(
        "−∞ → 下侧恒定色 → L → 中间色段 → U → 上侧恒定色 → +∞\nNaN／未测使用独立状态色。", this);
    structure->setWordWrap(true);
    layout->addWidget(structure);
    m_applied = new QLabel(this);
    m_applied->setWordWrap(true);
    layout->addWidget(m_applied);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    layout->addWidget(scroll);
    m_editor = new ParameterEditor(module, action, {}, parameters, parameters, this);
    m_editor->setObjectName("colorLegendParameters");
    scroll->setWidget(m_editor);
    auto *apply = new QPushButton("应用到此结果", this);
    apply->setObjectName("applyColorLegend");
    layout->addWidget(apply);
    connect(apply, &QPushButton::clicked, this, [this, submit] {
        try {
            submit(m_editor->GetValue().toObject());
        } catch (const std::exception &e) {
            m_applied->setText(QString::fromUtf8(e.what()));
        }
    });
}
void ColorLegendEditor::SetApplied(const QString &description) { m_applied->setText(description); }
} // namespace Manual
