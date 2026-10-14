#pragma once
#include <QDialog>
#include <QJsonObject>
#include <functional>
class QLabel;
namespace Manual {
class ParameterEditor;
class ColorLegendEditor final : public QDialog {
  public:
    ColorLegendEditor(QString module, QString action, QJsonObject parameters,
                      std::function<void(QJsonObject)> submit, QWidget *parent);
    void SetApplied(const QString &description);

  private:
    ParameterEditor *m_editor;
    QLabel *m_applied;
};
} // namespace Manual
