#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include "Data/DataGraphTypes.h"
#include <functional>
#include <vector>
class QLabel;
class QTreeWidget;
class QComboBox;
class QTabWidget;
class QHBoxLayout;
class QToolButton;
class QLineEdit;
class QPushButton;
class QHideEvent;
class QFocusEvent;
namespace Manual {
using GrayPointFilter=std::function<std::vector<bool>(const std::vector<std::array<double,3>>&)>;
struct GrayStatisticsScope {
    DataRevisionRef source,roi;
    QString key,title,description;
    GrayPointFilter filter;
};
struct StatisticsSamples {
    std::vector<double> values;
    std::vector<double> weights;
    std::vector<std::uint8_t> groups;
    std::size_t next=0, total=0, excluded=0;
    QString description;
};
// 单个可见业务页的有界统计任务；每次事件只读一块，精确来源变化后丢弃旧任务。
class BusinessStatistics final : public QWidget {
public:
    explicit BusinessStatistics(QWidget* parent=nullptr);
    void SetHistogram(const QJsonObject& data);
    void SetTable(const QStringList& headings,const QJsonArray& rows);
    void SetMessage(const QString& title,const QString& message);
    void StartHistogram(const QString& key,const QString& title,const QString& xUnit,
        const QString& yUnit,std::function<StatisticsSamples(std::size_t)> read);
    void Cancel();
    void SetCompact(bool compact);
    bool ContainsInterval(double value,double low,double high) const;
    void SetMarkers(const QJsonArray& markers);
    void SetInteractionModes(const QStringList& modes);
    void SetInteractionContext(const QString& context);
    void CancelInteraction();
    void SelectInteractionMode(const QString& mode);
    QString GetInteractionMode() const;
    void ShowTable();
    QJsonObject GetHistogram() const { return m_data; }
    std::function<void(double,double)> onInterval;
    std::function<void(int)> onRow;
    // 仅 owner thread 的释放、确认或 Enter 同步通知；移动、取消和失效不通知。
    // 调用前已清除候选，回调可发既有请求、查询或关闭界面；返回后不沿用旧交互状态。
    std::function<void(const QString&,double)> onMarker;
protected:
    void resizeEvent(QResizeEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
private:
    class Plot;
    Plot* m_plot=nullptr;
    QLabel* m_title=nullptr;
    QLabel* m_description=nullptr;
    QTreeWidget* m_table=nullptr;
    QComboBox* m_interaction=nullptr;
    QTabWidget* m_tabs=nullptr;
    QLineEdit* m_markerValue=nullptr;
    QPushButton* m_applyMarker=nullptr;
    QWidget* m_markerRow=nullptr;
    QHBoxLayout* m_controls=nullptr;
    QToolButton* m_info=nullptr;
    QHBoxLayout* m_markerLayout=nullptr;
    bool m_inlineMarker=false;
    QJsonObject m_data;
    QStringList m_tableHeadings;
    QJsonArray m_tableRows;
    QTimer m_timer;
    QString m_key,m_jobTitle,m_xUnit,m_yUnit,m_jobDescription;
    std::function<StatisticsSamples(std::size_t)> m_read;
    std::vector<double> m_counts;
    std::vector<double> m_comparisonCounts;
    std::size_t m_offset=0,m_used=0,m_excluded=0;
    qint64 m_maximumOwnerBlockNs=0;
    double m_minimum=0,m_maximum=0;
    bool m_isCounting=false,m_hasValue=false;
    enum class State { None,Loading,Ready,Empty,Failed,Cancelled };
    State m_state=State::None;
    std::uint64_t m_generation=0;
    bool m_compact=false;
    int m_tableTab=-1;
    QString m_interactionContext;
    QStringList m_interactionModes;
    bool m_autoModeSelected=false;
    QString m_markerEditKey;
    QJsonObject m_markerEditSource;
    void StopRead();
    void ResetPresentation();
    void Present(const QJsonObject& data);
    void ReadBlock();
};
QJsonObject BuildHistogram(const std::vector<double>& values,const std::vector<double>& weights,
    const QString& title,const QString& description,const QString& xUnit,const QString& yUnit);
}
