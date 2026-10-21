#include "BusinessStatistics.h"
#include "WorkbenchStyle.h"
#include <QLabel>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QHeaderView>
#include <QPainter>
#include <QMouseEvent>
#include <QJsonArray>
#include <QComboBox>
#include <QTabWidget>
#include <QTabBar>
#include <QToolButton>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QFocusEvent>
#include <QHideEvent>
#include <QFontMetrics>
#include <QLineEdit>
#include <QPushButton>
#include <QFocusEvent>
#include <QHideEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QLocale>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <limits>
namespace Manual {
namespace {
// 显示取六位有效数字；编辑和提交始终保留原始双精度文本。
class MarkerInput final : public QLineEdit {
public:
    explicit MarkerInput(QWidget* parent):QLineEdit(parent){connect(this,&QLineEdit::textEdited,this,[this](const QString& value){m_exact=value;});}
    void SetNumber(double value){m_exact=QString::number(value,'g',17);SetDisplay();}
    double GetNumber(bool* valid) const {auto number=m_exact.toDouble(valid);if(!*valid)number=QLocale().toDouble(m_exact,valid);return number;}
protected:
    void focusInEvent(QFocusEvent* event) override {QLineEdit::focusInEvent(event);setText(m_exact);}
    void focusOutEvent(QFocusEvent* event) override {QLineEdit::focusOutEvent(event);SetDisplay();}
private:
    QString m_exact;
    void SetDisplay(){bool valid=false;const auto number=GetNumber(&valid);auto locale=QLocale();if(!locale.zeroDigit().isDigit())locale=QLocale::c();const auto display=hasFocus()?m_exact:valid?locale.toString(number,'g',6):m_exact;if(text()!=display)setText(display);}
};
QJsonObject HistogramData(const std::vector<double>& counts,double low,double high,
    const QString& title,const QString& description,const QString& xUnit,const QString& yUnit)
{
    QJsonArray bins,edges;
    for(auto value:counts) bins.append(value);
    for(std::size_t i=0;i<=counts.size();++i) edges.append(low+(high-low)*i/counts.size());
    return {{"title",title},{"description",description},{"counts",bins},{"edges",edges},{"xUnit",xUnit},{"yUnit",yUnit}};
}
}
QJsonObject BuildHistogram(const std::vector<double>& values,const std::vector<double>& weights,
    const QString& title,const QString& description,const QString& xUnit,const QString& yUnit)
{
    double low=std::numeric_limits<double>::infinity(),high=-low;
    for(auto value:values) if(std::isfinite(value)){low=std::min(low,value);high=std::max(high,value);}
    if(!std::isfinite(low)) return {{"title",title},{"description",description+" · 无有效样本"}};
    if(low==high){const double half=std::max(0.5,std::abs(low)*1e-6);low-=half;high+=half;}
    std::vector<double> counts(32,0);
    for(std::size_t i=0;i<values.size();++i) if(std::isfinite(values[i])) {
        const auto bin=std::min<std::size_t>(counts.size()-1,static_cast<std::size_t>(std::max(0.0,(values[i]-low)/(high-low))*counts.size()));
        if(weights.empty()) counts[bin]+=1;
        else if(i<weights.size() && std::isfinite(weights[i]) && weights[i]>0) counts[bin]+=weights[i];
    }
    return HistogramData(counts,low,high,title,description,xUnit,yUnit);
}
class BusinessStatistics::Plot final : public QWidget {
public:
    explicit Plot(BusinessStatistics& owner):QWidget(&owner),m_owner(owner){setObjectName("businessHistogramPlot");setMinimumHeight(150);setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);setAccessibleName("灰度直方图；方向键调整候选，Enter 提交，Esc 取消");}
    void ResetRange(){m_owner.CancelInteraction();m_hasRange=false;update();}
    void CancelInteraction(){m_dragKey.clear();m_dragMode.clear();m_pressBin=-1;m_dragSource={};m_pressSource={};update();}
    void ValidateMarkers(const QJsonArray& markers){if(m_dragKey.isEmpty())return;for(const auto item:markers){const auto marker=item.toObject();if(marker["key"].toString(marker["label"].toString())==m_dragKey && marker["editable"].toBool())return;}CancelInteraction();}
    QJsonObject SourceStamp() const{return GetSourceStamp();}
protected:
    void paintEvent(QPaintEvent*) override {
        const auto colors=GetWorkbenchPlotColors();
        // Qt 5 的非十进制本地数字不连续（例如〇），避免刻度被映射成括号。
        auto locale=QLocale();if(!locale.zeroDigit().isDigit())locale=QLocale::c();
        QPainter painter(this); painter.fillRect(rect(),colors.surface); painter.setPen(colors.text);
        if(hasFocus()){painter.setPen(QPen(colors.marker,1,Qt::DashLine));painter.drawRect(rect().adjusted(1,1,-2,-2));painter.setPen(colors.text);}
        const auto counts=m_owner.m_data["counts"].toArray(),edges=m_owner.m_data["edges"].toArray();
        const auto comparison=m_owner.m_data["comparisonCounts"].toArray();
        if(counts.isEmpty() || edges.size()!=counts.size()+1){painter.drawText(rect(),Qt::AlignCenter,m_owner.m_state==State::Loading?m_owner.m_isCounting?"正在统计灰度分布…":"正在读取灰度范围…":"暂无可用分布数据");return;}
        const auto area=GetPlotArea();const int textHeight=fontMetrics().height()+2;
        if(area.width()<=0 || area.height()<=0)return;
        const auto fullLow=edges.first().toDouble(),fullHigh=edges.last().toDouble();
        if(!(fullHigh>fullLow))return;
        if(m_edges!=edges){m_owner.CancelInteraction();m_edges=edges;m_hasRange=false;}
        const auto low=m_hasRange?m_low:fullLow,high=m_hasRange?m_high:fullHigh;
        double maximum=0;for(const auto value:counts) maximum=std::max(maximum,value.toDouble());
        for(const auto value:comparison)maximum=std::max(maximum,value.toDouble());
        const auto yUnit=m_owner.m_data["yUnit"].toString();painter.drawText(QRectF(4,0,62,textHeight),Qt::AlignLeft,yUnit.contains("频数")||yUnit.contains("次数")?QString("频数"):fontMetrics().elidedText(yUnit,Qt::ElideRight,62));
        const int gridSteps=area.height()<60?2:3;
        for(int i=0;i<=gridSteps;++i){const auto y=area.bottom()-area.height()*i/gridSteps;painter.setPen(colors.grid);painter.drawLine(QPointF(area.left(),y),QPointF(area.right(),y));painter.setPen(colors.text);painter.drawText(QRectF(0,y-textHeight/2.,56,textHeight),Qt::AlignRight|Qt::AlignVCenter,locale.toString(maximum*i/gridSteps,'g',4));}
        painter.setPen(colors.text);
        painter.setPen(Qt::NoPen);
        const auto span=high-low;painter.save();painter.setClipRect(area);
        for(int i=0;i<counts.size();++i){const auto h=maximum>0 ? area.height()*counts[i].toDouble()/maximum : 0;const auto x=area.left()+area.width()*(edges[i].toDouble()-low)/span;const auto w=area.width()*(edges[i+1].toDouble()-edges[i].toDouble())/span;
            painter.setBrush(colors.series);painter.setPen(Qt::NoPen);painter.drawRect(QRectF(x,area.bottom()-h,std::max(0.5,(comparison.isEmpty()?w:w*0.5)-0.5),h));
            if(comparison.size()==counts.size()){const auto other=maximum>0?area.height()*comparison[i].toDouble()/maximum:0;const QRectF bar(x+w*0.5,area.bottom()-other,std::max(0.5,w*0.5-0.5),other);painter.setBrush(QBrush(colors.comparison,Qt::DiagCrossPattern));painter.setPen(QPen(colors.comparison,0.8));painter.drawRect(bar);}}
        painter.restore();
        painter.setPen(colors.text);
        painter.drawText(QRectF(area.left()-12,area.bottom()+2,100,textHeight),Qt::AlignLeft,locale.toString(low,'g',6));
        painter.drawText(QRectF(area.right()-100,area.bottom()+2,110,textHeight),Qt::AlignRight,locale.toString(high,'g',6));
        painter.drawText(QRectF(area.left(),height()-textHeight,area.width(),textHeight),Qt::AlignCenter,m_owner.m_data["xUnit"].toString());
        int markerIndex=0;const auto legendWidth=area.width()/std::max(1,m_owner.m_data["markers"].toArray().size());
        if(high>low) for(const auto marker:m_owner.m_data["markers"].toArray()) {
            const auto item=marker.toObject();const auto key=item["key"].toString(item["label"].toString());const bool candidate=key==m_dragKey;
            const auto value=candidate?m_candidate:item["value"].toDouble();if(value<low || value>high)continue;
            const auto x=area.left()+area.width()*(value-low)/(high-low);const bool editable=item["editable"].toBool();QPen pen(editable?colors.marker:colors.text);pen.setStyle(editable?Qt::SolidLine:Qt::DashLine);painter.setPen(pen);painter.drawLine(QPointF(x,area.top()),QPointF(x,area.bottom()));
            if(editable){painter.setBrush(colors.marker);painter.drawRect(QRectF(x-4,area.top()-3,8,8));}
            const auto legend=item["label"].toString()+(editable?QString():" "+locale.toString(value,'g',5));
            painter.drawText(QRectF(area.left()+8+legendWidth*markerIndex,0,std::max(0.,legendWidth-8),textHeight),Qt::AlignLeft,fontMetrics().elidedText(legend,Qt::ElideRight,static_cast<int>(std::max(0.,legendWidth-8))));++markerIndex;
            if(candidate){painter.setPen(colors.marker);painter.drawText(QRectF(area.left(),area.top()+4,area.width(),textHeight),Qt::AlignCenter,QString("%1 · 待提交").arg(locale.toString(value,'g',6)));}
        }
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if(!m_dragKey.isEmpty()) {
            if(m_dragSource!=GetSourceStamp() || !m_owner.onMarker){CancelInteraction();return;}
            if(m_owner.GetInteractionMode()=="查看分布"){CancelInteraction();return;}
            m_candidate=ValueAt(event->pos().x());static_cast<MarkerInput*>(m_owner.m_markerValue)->SetNumber(m_candidate);update();return;
        }
        const auto counts=m_owner.m_data["counts"].toArray(),edges=m_owner.m_data["edges"].toArray();
        const auto i=GetBin(event->pos(),counts.size());
        if(i>=0 && edges.size()==counts.size()+1) {
            const auto comparison=m_owner.m_data["comparisonCounts"].toArray();
            auto text=QString("[%1, %2%3 · %4 %5").arg(edges[i].toDouble()).arg(edges[i+1].toDouble()).arg(i==counts.size()-1 ? "]" : ")").arg(counts[i].toDouble()).arg(m_owner.m_data["yUnit"].toString());
            if(comparison.size()==counts.size())text+=QString(" · 源 %1 / 候选 %2").arg(counts[i].toDouble()).arg(comparison[i].toDouble());
            setToolTip(text);
        } else setToolTip({});
    }
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()!=Qt::LeftButton)return;m_owner.CancelInteraction();setFocus();
        const auto edges=m_owner.m_data["edges"].toArray();if(edges.size()<2)return;
        const auto low=m_hasRange?m_low:edges.first().toDouble(),high=m_hasRange?m_high:edges.last().toDouble();
        if(m_owner.GetInteractionMode()!="查看分布" && m_owner.onMarker && high>low)for(const auto value:m_owner.m_data["markers"].toArray()) {
            const auto marker=value.toObject();if(!marker["editable"].toBool())continue;
            const auto x=62+(width()-82)*(marker["value"].toDouble()-low)/(high-low);
            if(std::abs(event->pos().x()-x)<=8 && event->pos().y()>=4 && event->pos().y()<=height()-36) {
                m_dragKey=marker["key"].toString(marker["label"].toString());m_candidate=marker["value"].toDouble();m_dragSource=GetSourceStamp();m_dragMode=m_owner.GetInteractionMode();return;
            }
        }
        m_pressX=event->pos().x();m_pressBin=GetBin(event->pos(),edges.size()-1);m_pressSource=GetSourceStamp();m_pressMode=m_owner.GetInteractionMode();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if(event->button()!=Qt::LeftButton)return;
        if(!m_dragKey.isEmpty()){
            const auto key=m_dragKey;const auto value=m_candidate;const auto source=m_dragSource;const auto mode=m_dragMode;CancelInteraction();const auto callback=m_owner.onMarker;
            if(source==GetSourceStamp() && m_owner.GetInteractionMode()==mode && m_owner.GetInteractionMode()!="查看分布" && callback)callback(key,value);
            return;
        }
        const auto edges=m_owner.m_data["edges"].toArray();const auto i=GetBin(event->pos(),edges.size()-1);
        const auto select=m_owner.onInterval;
        const auto pressBin=m_pressBin;const auto pressSource=m_pressSource;const auto pressMode=m_pressMode;const auto pressX=m_pressX;m_pressBin=-1;
        if(i>=0 && pressBin>=0 && select && pressSource==GetSourceStamp()
            && m_owner.GetInteractionMode()==pressMode && m_owner.GetInteractionMode()!="查看分布") {
            if(std::abs(event->pos().x()-pressX)>4)select(std::min(ValueAt(pressX),ValueAt(event->pos().x())),std::max(ValueAt(pressX),ValueAt(event->pos().x())));
            else select(edges[i].toDouble(),edges[i+1].toDouble());
        }
    }
    void wheelEvent(QWheelEvent* event) override {
        const auto edges=m_owner.m_data["edges"].toArray();if(edges.size()<2)return;
        const auto low=m_hasRange?m_low:edges.first().toDouble(),high=m_hasRange?m_high:edges.last().toDouble();
        const auto center=ValueAt(event->pos().x()),factor=event->angleDelta().y()>0?0.8:1.25;
        m_low=std::max(edges.first().toDouble(),center-(center-low)*factor);m_high=std::min(edges.last().toDouble(),center+(high-center)*factor);
        if(m_high>m_low){m_owner.CancelInteraction();m_hasRange=true;update();}event->accept();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if(event->key()==Qt::Key_Escape){CancelInteraction();m_owner.CancelInteraction();return;}
        if(m_owner.GetInteractionMode()=="查看分布"){QWidget::keyPressEvent(event);return;}
        if(event->key()==Qt::Key_Left || event->key()==Qt::Key_Right) {
            if(m_dragKey.isEmpty())for(const auto value:m_owner.m_data["markers"].toArray()){const auto marker=value.toObject();if(marker["editable"].toBool()){m_dragKey=marker["key"].toString(marker["label"].toString());m_candidate=marker["value"].toDouble();m_dragSource=GetSourceStamp();m_dragMode=m_owner.GetInteractionMode();break;}}
            if(!m_dragKey.isEmpty() && m_dragSource==GetSourceStamp()){const auto edges=m_owner.m_data["edges"].toArray();const auto step=edges.size()>1?(edges.last().toDouble()-edges.first().toDouble())/100.0:1.0;m_candidate+=event->key()==Qt::Key_Right?step:-step;update();}return;
        }
        if(event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter){CommitCandidate();return;}
        QWidget::keyPressEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override {CancelInteraction();QWidget::focusOutEvent(event);}
    void hideEvent(QHideEvent* event) override {CancelInteraction();QWidget::hideEvent(event);}
private:
    BusinessStatistics& m_owner;
    QJsonArray m_edges;
    bool m_hasRange=false;
    double m_low=0,m_high=1,m_candidate=0;
    QString m_dragKey,m_dragMode,m_pressMode;
    QJsonObject m_dragSource,m_pressSource;
    int m_pressX=0,m_pressBin=-1;
    QRectF GetPlotArea() const {const int textHeight=fontMetrics().height()+2;const int top=std::max(24,textHeight+2),bottom=std::max(36,textHeight*2+4);return QRectF(62,top,width()-82,height()-top-bottom);}
    void CommitCandidate(){if(m_dragKey.isEmpty() || m_dragSource!=GetSourceStamp() || m_dragMode!=m_owner.GetInteractionMode())return;const auto key=m_dragKey;const auto value=m_candidate;const auto source=m_dragSource;const auto mode=m_dragMode;CancelInteraction();const auto callback=m_owner.onMarker;if(source==GetSourceStamp() && mode==m_owner.GetInteractionMode() && callback)callback(key,value);}
    QJsonObject GetSourceStamp() const {
        const auto edges=m_owner.m_data["edges"].toArray();QJsonArray modes;for(const auto& mode:m_owner.m_interactionModes)modes.append(mode);
        return {{"cacheKey",m_owner.m_data["cacheKey"]},{"description",m_owner.m_data["description"]},{"context",m_owner.m_interactionContext},{"modes",modes},{"mode",m_owner.GetInteractionMode()},{"edges",edges},
            {"low",m_hasRange?m_low:edges.isEmpty()?0:edges.first().toDouble()},{"high",m_hasRange?m_high:edges.isEmpty()?0:edges.last().toDouble()}};
    }
    double ValueAt(int x) const {
        const auto edges=m_owner.m_data["edges"].toArray();if(edges.size()<2 || width()<=82)return 0;
        const auto low=m_hasRange?m_low:edges.first().toDouble(),high=m_hasRange?m_high:edges.last().toDouble();
        return low+(high-low)*std::clamp((x-62)/(width()-82.),0.,1.);
    }
    int GetBin(const QPoint& point,int count) const {
        const auto edges=m_owner.m_data["edges"].toArray();
        if(count<=0 || edges.size()!=count+1 || width()<=82 || height()<=60 || !GetPlotArea().contains(point))return -1;
        const auto value=ValueAt(point.x());
        int low=0,high=count;while(low+1<high){const auto middle=(low+high)/2;if(value>=edges[middle].toDouble())low=middle;else high=middle;}return low;
    }
};
BusinessStatistics::BusinessStatistics(QWidget* parent):QWidget(parent)
{
    setObjectName("businessStatistics");auto* layout=new QVBoxLayout(this);layout->setContentsMargins(8,4,8,4);layout->setSpacing(4);
    m_title=new QLabel(this);m_title->setObjectName("statisticsTitle");m_description=new QLabel(this);m_description->setWordWrap(false);m_description->setObjectName("statisticsProvenance");m_description->setMaximumHeight(20);
    layout->addWidget(m_title);layout->addWidget(m_description);
    m_tabs=new QTabWidget(this);m_tabs->setObjectName("statisticsContentTabs");m_tabs->setTabBarAutoHide(true);layout->addWidget(m_tabs,1);
    auto* chart=new QWidget(m_tabs);auto* chartLayout=new QVBoxLayout(chart);chartLayout->setContentsMargins(0,0,0,0);chartLayout->setSpacing(2);
    auto* controls=new QHBoxLayout;m_controls=controls;m_interaction=new QComboBox(chart);m_interaction->setObjectName("histogramInteraction");m_interaction->addItem("查看分布");m_interaction->setMinimumContentsLength(2);m_interaction->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);controls->addWidget(m_interaction,1);
    auto* reset=new QToolButton(chart);reset->setText("适配坐标");reset->setAccessibleName("适配直方图坐标轴");reset->setObjectName("resetHistogramRange");controls->addWidget(reset);chartLayout->addLayout(controls);
    m_info=new QToolButton(chart);m_info->setObjectName("histogramInfo");m_info->setText("信息");m_info->setAccessibleName("分布来源、采样与有效范围");controls->addWidget(m_info);
    m_plot=new Plot(*this);m_plot->setToolTip("滚轮缩放坐标轴；先选择用途，再点击区间或拖动可编辑标记。Esc 取消标记编辑。");chartLayout->addWidget(m_plot,1);
    m_markerRow=new QWidget(chart);auto* markerRow=new QHBoxLayout(m_markerRow);m_markerLayout=markerRow;markerRow->setContentsMargins(0,0,0,0);
    m_markerValue=new MarkerInput(m_markerRow);m_markerValue->setObjectName("histogramMarkerValue");m_markerValue->setPlaceholderText("阈值候选值");markerRow->addWidget(m_markerValue,1);
    m_applyMarker=new QPushButton("确认",m_markerRow);m_applyMarker->setAccessibleName("确认当前用途的阈值候选");m_applyMarker->setObjectName("applyHistogramMarker");markerRow->addWidget(m_applyMarker);chartLayout->addWidget(m_markerRow);m_markerRow->hide();m_markerValue->hide();m_applyMarker->hide();
    connect(m_interaction,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{CancelInteraction();const bool editing=GetInteractionMode()!="查看分布";m_markerValue->setEnabled(editing);m_applyMarker->setEnabled(editing);});
    connect(m_markerValue,&QLineEdit::textEdited,this,[this]{if(m_markerEditKey.isEmpty()){for(const auto item:m_data["markers"].toArray()){const auto marker=item.toObject();if(marker["editable"].toBool()){m_markerEditKey=marker["key"].toString(marker["label"].toString());m_markerEditSource=m_plot->SourceStamp();break;}}}});
    connect(m_applyMarker,&QPushButton::clicked,this,[this]{
        bool ok=false;const auto value=static_cast<MarkerInput*>(m_markerValue)->GetNumber(&ok);const auto key=m_markerEditKey;const auto source=m_markerEditSource;
        if(!ok || !std::isfinite(value) || key.isEmpty() || source!=m_plot->SourceStamp() || GetInteractionMode()=="查看分布"){CancelInteraction();return;}
        bool editable=false;for(const auto item:m_data["markers"].toArray()){const auto marker=item.toObject();if(marker["key"].toString(marker["label"].toString())==key && marker["editable"].toBool())editable=true;}
        const auto callback=onMarker;CancelInteraction();if(editable && callback)callback(key,value);
    });
    connect(m_markerValue,&QLineEdit::returnPressed,m_applyMarker,&QPushButton::click);
    connect(reset,&QToolButton::clicked,this,[this]{m_plot->ResetRange();});m_tabs->addTab(chart,"直方图");
    m_table=new QTreeWidget(this);m_table->setObjectName("businessResultTable");m_table->setRootIsDecorated(false);m_table->hide();
    connect(m_table,&QTreeWidget::itemClicked,this,[this](QTreeWidgetItem* item,int){if(onRow)onRow(item->data(0,Qt::UserRole).toInt());});
    m_timer.setSingleShot(true);connect(&m_timer,&QTimer::timeout,this,[this]{ReadBlock();});
}
void BusinessStatistics::SetHistogram(const QJsonObject& data)
{
    StopRead();m_key.clear();ResetPresentation();
    m_state=data["counts"].toArray().isEmpty()?State::Empty:State::Ready;Present(data);
}
void BusinessStatistics::Present(const QJsonObject& data)
{
    if(data!=m_data)CancelInteraction();
    m_data=data;m_title->setText(data["title"].toString());
    const auto description=data["description"].toString()+(data["comparisonCounts"].toArray().isEmpty()?QString():QString(" · 源：实心；候选：纹理"));
    m_description->setText(QFontMetrics(m_description->font()).elidedText(description,Qt::ElideRight,std::max(1,m_description->width())));m_description->setToolTip(description);
    m_info->setToolTip(data["title"].toString()+"\n"+description);m_description->setVisible(!m_compact);
    m_plot->setAccessibleDescription(data["title"].toString()+"；横轴："+data["xUnit"].toString()+"；纵轴："+data["yUnit"].toString()+"；"+description);
    m_plot->setVisible(!data["counts"].toArray().isEmpty() || m_state==State::Loading);m_plot->update();
}
void BusinessStatistics::ResetPresentation(){CancelInteraction();onInterval={};onRow={};onMarker={};m_tableRows={};m_tableHeadings.clear();m_table->clear();if(m_tableTab>=0){m_tabs->removeTab(m_tableTab);m_tableTab=-1;}m_table->hide();m_tabs->tabBar()->hide();}
void BusinessStatistics::SetMessage(const QString& title,const QString& message){SetHistogram({{"title",title},{"description",message}});}
void BusinessStatistics::SetTable(const QStringList& headings,const QJsonArray& rows)
{
    if(rows==m_tableRows && headings.size()==m_tableHeadings.size() && std::equal(headings.cbegin(),headings.cend(),m_tableHeadings.cbegin()))return;
    m_tableRows=rows;m_tableHeadings=headings;
    if(!headings.isEmpty() && m_tableTab<0)m_tableTab=m_tabs->addTab(m_table,"结果列表");
    m_tabs->tabBar()->setVisible(m_tableTab>=0);
    if(headings.isEmpty() && m_tableTab>=0){m_tabs->removeTab(m_tableTab);m_tableTab=-1;return;}
    m_table->clear();m_table->setHeaderLabels(headings);
    for(int i=0;i<rows.size();++i){QStringList cells;for(const auto value:rows[i].toArray())cells.append(value.isString()?value.toString():value.isNull()?"未测":QString::number(value.toDouble(),'g',8));auto* item=new QTreeWidgetItem(m_table,cells);item->setData(0,Qt::UserRole,i);}
    m_table->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    if(m_data["counts"].toArray().isEmpty() && !headings.isEmpty())m_tabs->setCurrentIndex(1);
}
void BusinessStatistics::StopRead(){++m_generation;m_timer.stop();m_read={};}
void BusinessStatistics::Cancel(){StopRead();CancelInteraction();onInterval={};onRow={};onMarker={};if(m_state==State::Loading){m_state=State::Cancelled;m_data["status"]="Cancelled";m_key.clear();}}
void BusinessStatistics::SetCompact(bool compact)
{
    m_compact=compact;m_title->setVisible(!compact);m_description->setVisible(!compact);m_description->setMaximumHeight(m_description->fontMetrics().height()+2);
    m_plot->setMinimumHeight(150);
}
void BusinessStatistics::SetInteractionModes(const QStringList& modes)
{
    QStringList next{"查看分布"};next.append(modes);
    QStringList old;for(int i=0;i<m_interaction->count();++i)old.append(m_interaction->itemText(i));
    if(next.size()==old.size() && std::equal(next.cbegin(),next.cend(),old.cbegin()))return;
    CancelInteraction();m_interactionModes=modes;m_autoModeSelected=false;
    const auto selected=m_interaction->currentText();const QSignalBlocker blocker(m_interaction);m_interaction->clear();m_interaction->addItems(next);
    m_interaction->setCurrentIndex(std::max(0,m_interaction->findText(selected)));
}
void BusinessStatistics::SetInteractionContext(const QString& context){if(m_interactionContext==context)return;m_interactionContext=context;CancelInteraction();}
void BusinessStatistics::CancelInteraction()
{
    m_plot->CancelInteraction();m_markerEditKey.clear();m_markerEditSource={};
    // 重复 Stop/析构只恢复现有标记，避免先清空再回填触发两次数值及可访问性通知。
    for(const auto value:m_data["markers"].toArray()) {
        const auto marker=value.toObject();
        if(marker["editable"].toBool()){static_cast<MarkerInput*>(m_markerValue)->SetNumber(marker["value"].toDouble());return;}
    }
    m_markerValue->clear();
}
void BusinessStatistics::SelectInteractionMode(const QString& mode){if(m_autoModeSelected)return;const auto index=m_interaction->findText(mode);if(index<0)return;m_autoModeSelected=true;m_interaction->setCurrentIndex(index);}
QString BusinessStatistics::GetInteractionMode() const{return m_interaction->currentText();}
void BusinessStatistics::ShowTable(){if(m_tableTab>=0)m_tabs->setCurrentIndex(m_tableTab);}
bool BusinessStatistics::ContainsInterval(double value,double low,double high) const
{
    const auto edges=m_data["edges"].toArray();return value>=low && (value<high || (!edges.isEmpty() && high==edges.last().toDouble() && value==high));
}
void BusinessStatistics::SetMarkers(const QJsonArray& markers){
    m_plot->ValidateMarkers(markers);
    if(!m_markerEditKey.isEmpty()){bool found=false;for(const auto item:markers){const auto marker=item.toObject();if(marker["key"].toString(marker["label"].toString())==m_markerEditKey && marker["editable"].toBool())found=true;}if(!found)CancelInteraction();}
    if(m_markerEditKey.isEmpty())for(const auto item:markers){const auto marker=item.toObject();if(marker["editable"].toBool()){static_cast<MarkerInput*>(m_markerValue)->SetNumber(marker["value"].toDouble());break;}}
    const bool editable=std::any_of(markers.begin(),markers.end(),[](const QJsonValue& marker){return marker.toObject()["editable"].toBool();});
    m_markerValue->setVisible(editable);m_markerValue->setAccessibleName("当前用途的阈值候选值");m_applyMarker->setVisible(editable);m_markerRow->setVisible(editable&&!m_inlineMarker);
    m_markerValue->setEnabled(GetInteractionMode()!="查看分布");m_applyMarker->setEnabled(GetInteractionMode()!="查看分布");
    m_data["markers"]=markers;m_plot->update();
}
void BusinessStatistics::hideEvent(QHideEvent* event){CancelInteraction();QWidget::hideEvent(event);}
void BusinessStatistics::focusOutEvent(QFocusEvent* event){CancelInteraction();QWidget::focusOutEvent(event);}
void BusinessStatistics::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);if(!m_markerValue)return;
    const auto description=m_description->toolTip();m_description->setText(m_description->fontMetrics().elidedText(description,Qt::ElideRight,std::max(1,width()-20)));
    const bool inlineValue=width()>=560;if(inlineValue==m_inlineMarker)return;
    CancelInteraction();m_controls->removeWidget(m_markerValue);m_controls->removeWidget(m_applyMarker);m_markerLayout->removeWidget(m_markerValue);m_markerLayout->removeWidget(m_applyMarker);
    m_inlineMarker=inlineValue;
    if(inlineValue){m_controls->insertWidget(1,m_markerValue,1);m_controls->insertWidget(2,m_applyMarker);}else{m_markerLayout->addWidget(m_markerValue,1);m_markerLayout->addWidget(m_applyMarker);}
    SetMarkers(m_data["markers"].toArray());
}
void BusinessStatistics::StartHistogram(const QString& key,const QString& title,const QString& xUnit,const QString& yUnit,std::function<StatisticsSamples(std::size_t)> read)
{
    if(key==m_key && ((m_state==State::Loading && m_read) || ((m_state==State::Ready || m_state==State::Empty) && m_data["cacheKey"]==key)))return;
    StopRead();ResetPresentation();m_key=key;m_jobTitle=title;m_xUnit=xUnit;m_yUnit=yUnit;m_state=State::Loading;m_read=std::move(read);
    m_offset=0;m_used=0;m_excluded=0;m_maximumOwnerBlockNs=0;m_isCounting=false;m_hasValue=false;m_counts.assign(32,0);m_comparisonCounts.clear();m_jobDescription.clear();
    Present({{"cacheKey",key},{"status","Loading"},{"title",title},{"description","正在读取该精确修订的样本；分块统计不会触发业务算法。"}});m_timer.start(120);
}
void BusinessStatistics::ReadBlock()
{
    if(!m_read || m_state!=State::Loading)return;
    const auto generation=m_generation;
    const auto read=m_read;
    QElapsedTimer blockClock;blockClock.start();
    try {
        const auto block=read(m_offset);
        if(generation!=m_generation || m_state!=State::Loading)return;
        m_jobDescription=block.description;
        for(std::size_t i=0;i<block.values.size();++i) {
            const auto value=block.values[i];if(!std::isfinite(value))continue;
            if(!m_isCounting){if(!m_hasValue){m_minimum=m_maximum=value;m_hasValue=true;}else{m_minimum=std::min(m_minimum,value);m_maximum=std::max(m_maximum,value);}++m_used;}
            else {const auto bin=std::min<std::size_t>(31,static_cast<std::size_t>(std::max(0.0,(value-m_minimum)/(m_maximum-m_minimum))*32));
                if(!block.groups.empty() && block.groups.at(i)==1){if(m_comparisonCounts.empty())m_comparisonCounts.assign(32,0);m_comparisonCounts[bin]+=block.weights.empty()?1:block.weights.at(i);}
                else m_counts[bin]+=block.weights.empty()?1:block.weights.at(i);}
        }
        if(!m_isCounting)m_excluded+=block.excluded;
        m_maximumOwnerBlockNs=std::max(m_maximumOwnerBlockNs,blockClock.nsecsElapsed());
        if(block.next>m_offset && block.next<block.total){m_offset=block.next;m_timer.start(1);return;}
        if(!m_hasValue){m_state=State::Empty;Present({{"cacheKey",m_key},{"status","Empty"},{"title",m_jobTitle},{"description",m_jobDescription+(block.total==0 ? " · 有效结果：0 个对象，分布为空" : " · 抽样未命中有效样本，不能据此判定范围没有体素")}});StopRead();return;}
        if(!m_isCounting){if(m_minimum==m_maximum){const auto half=std::max(0.5,std::abs(m_minimum)*1e-6);m_minimum-=half;m_maximum+=half;}m_isCounting=true;m_offset=0;m_timer.start(1);return;}
        auto data=HistogramData(m_counts,m_minimum,m_maximum,m_jobTitle,m_jobDescription+QString(" · 有效样本 %1 · 排除 %2").arg(m_used).arg(m_excluded),m_xUnit,m_yUnit);data["cacheKey"]=m_key;
        if(!m_comparisonCounts.empty()){QJsonArray other;for(auto value:m_comparisonCounts)other.append(value);data["comparisonCounts"]=other;}
        data["markers"]=m_data["markers"];data["status"]="Ready";data["maximumOwnerBlockMs"]=m_maximumOwnerBlockNs/1000000.;m_state=State::Ready;Present(data);StopRead();
    } catch(const std::exception& error){if(generation!=m_generation)return;m_state=State::Failed;Present({{"cacheKey",m_key},{"status","Failed"},{"title",m_jobTitle},{"description","统计不可用："+QString::fromUtf8(error.what())+"；可按同一来源重试。"}});StopRead();}
}
}
