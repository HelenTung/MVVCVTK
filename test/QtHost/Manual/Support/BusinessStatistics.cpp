#include "BusinessStatistics.h"
#include <QLabel>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QHeaderView>
#include <QPainter>
#include <QMouseEvent>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <limits>
namespace Manual {
namespace {
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
    explicit Plot(BusinessStatistics& owner):QWidget(&owner),m_owner(owner){setObjectName("businessHistogramPlot");setMinimumHeight(125);setMouseTracking(true);}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.fillRect(rect(),Qt::white); painter.setPen(QColor("#62758b"));
        const auto counts=m_owner.m_data["counts"].toArray(),edges=m_owner.m_data["edges"].toArray();
        const auto comparison=m_owner.m_data["comparisonCounts"].toArray();
        if(counts.isEmpty() || edges.size()!=counts.size()+1){painter.drawText(rect(),Qt::AlignCenter,"暂无可用分布数据");return;}
        const QRectF area(62,24,width()-82,height()-60);
        double maximum=0;for(const auto value:counts) maximum=std::max(maximum,value.toDouble());
        for(const auto value:comparison)maximum=std::max(maximum,value.toDouble());
        painter.drawText(QRectF(4,2,width()-8,18),Qt::AlignLeft,m_owner.m_data["yUnit"].toString());
        const int gridSteps=area.height()<60?2:3;
        for(int i=0;i<=gridSteps;++i){const auto y=area.bottom()-area.height()*i/gridSteps;painter.setPen(QColor("#e0e8f1"));painter.drawLine(QPointF(area.left(),y),QPointF(area.right(),y));painter.setPen(QColor("#62758b"));painter.drawText(QRectF(0,y-8,56,16),Qt::AlignRight,QString::number(maximum*i/gridSteps,'g',4));}
        painter.setPen(Qt::NoPen);painter.setBrush(QColor("#7ba4cc"));
        const auto span=edges.last().toDouble()-edges.first().toDouble();
        for(int i=0;i<counts.size();++i){const auto h=maximum>0 ? area.height()*counts[i].toDouble()/maximum : 0;const auto x=area.left()+area.width()*(edges[i].toDouble()-edges.first().toDouble())/span;const auto w=area.width()*(edges[i+1].toDouble()-edges[i].toDouble())/span;
            painter.setBrush(QColor("#7ba4cc"));painter.drawRect(QRectF(x,area.bottom()-h,std::max(0.5,(comparison.isEmpty()?w:w*0.5)-0.5),h));
            if(comparison.size()==counts.size()){const auto other=maximum>0?area.height()*comparison[i].toDouble()/maximum:0;painter.setBrush(QColor("#c58d4e"));painter.drawRect(QRectF(x+w*0.5,area.bottom()-other,std::max(0.5,w*0.5-0.5),other));}}
        painter.setPen(QColor("#62758b"));
        painter.drawText(QRectF(area.left()-12,area.bottom()+4,100,18),Qt::AlignLeft,QString::number(edges.first().toDouble(),'g',6));
        painter.drawText(QRectF(area.right()-100,area.bottom()+4,110,18),Qt::AlignRight,QString::number(edges.last().toDouble(),'g',6));
        painter.drawText(QRectF(area.left(),height()-18,area.width(),18),Qt::AlignCenter,m_owner.m_data["xUnit"].toString());
        const auto low=edges.first().toDouble(),high=edges.last().toDouble();
        if(high>low) for(const auto marker:m_owner.m_data["markers"].toArray()) {
            const auto item=marker.toObject(); const auto value=item["value"].toDouble();if(value<low || value>high)continue;
            const auto x=area.left()+area.width()*(value-low)/(high-low);painter.setPen(QColor("#b87724"));painter.drawLine(QPointF(x,area.top()),QPointF(x,area.bottom()));painter.drawText(QRectF(x+2,6,90,18),item["label"].toString());
        }
    }
    void mouseMoveEvent(QMouseEvent* event) override {
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
        if(event->button()!=Qt::LeftButton)return;
        const auto edges=m_owner.m_data["edges"].toArray();const auto i=GetBin(event->pos(),edges.size()-1);
        const auto select=m_owner.onInterval;
        if(i>=0 && select) select(edges[i].toDouble(),edges[i+1].toDouble());
    }
private:
    BusinessStatistics& m_owner;
    int GetBin(const QPoint& point,int count) const {
        const auto edges=m_owner.m_data["edges"].toArray();
        if(count<=0 || edges.size()!=count+1 || width()<=82 || height()<=60 || !QRectF(62,24,width()-82,height()-60).contains(point))return -1;
        const auto x=point.x();
        const auto value=edges.first().toDouble()+(edges.last().toDouble()-edges.first().toDouble())*(x-62)/(width()-82.0);
        int low=0,high=count;while(low+1<high){const auto middle=(low+high)/2;if(value>=edges[middle].toDouble())low=middle;else high=middle;}return low;
    }
};
BusinessStatistics::BusinessStatistics(QWidget* parent):QWidget(parent)
{
    setObjectName("businessStatistics");auto* layout=new QVBoxLayout(this);layout->setContentsMargins(8,4,8,4);layout->setSpacing(4);
    m_title=new QLabel(this);m_title->setObjectName("statisticsTitle");m_description=new QLabel(this);m_description->setWordWrap(true);m_description->setObjectName("statisticsProvenance");
    layout->addWidget(m_title);layout->addWidget(m_description);m_plot=new Plot(*this);layout->addWidget(m_plot);
    m_table=new QTreeWidget(this);m_table->setObjectName("businessResultTable");m_table->setRootIsDecorated(false);m_table->setMaximumHeight(150);m_table->hide();layout->addWidget(m_table);
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
    m_data=data;m_title->setText(data["title"].toString());m_description->setText(data["description"].toString());m_description->setToolTip(data["description"].toString());
    m_plot->setVisible(!data["counts"].toArray().isEmpty() || m_state==State::Loading);m_plot->update();
}
void BusinessStatistics::ResetPresentation(){onInterval={};onRow={};m_table->clear();m_table->hide();}
void BusinessStatistics::SetMessage(const QString& title,const QString& message){SetHistogram({{"title",title},{"description",message}});m_table->hide();}
void BusinessStatistics::SetTable(const QStringList& headings,const QJsonArray& rows)
{
    m_table->clear();m_table->setHeaderLabels(headings);
    for(int i=0;i<rows.size();++i){QStringList cells;for(const auto value:rows[i].toArray())cells.append(value.isString()?value.toString():value.isNull()?"未测":QString::number(value.toDouble(),'g',8));auto* item=new QTreeWidgetItem(m_table,cells);item->setData(0,Qt::UserRole,i);}
    m_table->header()->setSectionResizeMode(QHeaderView::ResizeToContents);m_table->setVisible(!rows.isEmpty() && (!m_compact || m_data["counts"].toArray().isEmpty()));
}
void BusinessStatistics::StopRead(){++m_generation;m_timer.stop();m_read={};}
void BusinessStatistics::Cancel(){StopRead();onInterval={};onRow={};if(m_state==State::Loading){m_state=State::Cancelled;m_data["status"]="Cancelled";m_key.clear();}}
void BusinessStatistics::SetCompact(bool compact)
{
    m_compact=compact;m_title->setMaximumHeight(compact?22:16777215);m_description->setMaximumHeight(compact?32:16777215);
    m_plot->setMinimumHeight(compact?95:125);m_table->setMaximumHeight(compact?115:250);
    m_table->setVisible(m_table->topLevelItemCount()>0 && (!compact || m_data["counts"].toArray().isEmpty()));
}
bool BusinessStatistics::ContainsInterval(double value,double low,double high) const
{
    const auto edges=m_data["edges"].toArray();return value>=low && (value<high || (!edges.isEmpty() && high==edges.last().toDouble() && value==high));
}
void BusinessStatistics::SetMarkers(const QJsonArray& markers){m_data["markers"]=markers;m_plot->update();}
void BusinessStatistics::StartHistogram(const QString& key,const QString& title,const QString& xUnit,const QString& yUnit,std::function<StatisticsSamples(std::size_t)> read)
{
    if(key==m_key && ((m_state==State::Loading && m_read) || ((m_state==State::Ready || m_state==State::Empty) && m_data["cacheKey"]==key)))return;
    StopRead();ResetPresentation();m_key=key;m_jobTitle=title;m_xUnit=xUnit;m_yUnit=yUnit;m_state=State::Loading;m_read=std::move(read);
    m_offset=0;m_used=0;m_excluded=0;m_isCounting=false;m_hasValue=false;m_counts.assign(32,0);m_comparisonCounts.clear();m_jobDescription.clear();
    Present({{"cacheKey",key},{"status","Loading"},{"title",title},{"description","正在读取该精确修订的样本；分块统计不会触发业务算法。"}});m_timer.start(120);
}
void BusinessStatistics::ReadBlock()
{
    if(!m_read || m_state!=State::Loading)return;
    const auto generation=m_generation;
    const auto read=m_read;
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
        if(block.next>m_offset && block.next<block.total){m_offset=block.next;m_timer.start(1);return;}
        if(!m_hasValue){m_state=State::Empty;Present({{"cacheKey",m_key},{"status","Empty"},{"title",m_jobTitle},{"description",m_jobDescription+(block.total==0 ? " · 有效结果：0 个对象，分布为空" : " · 抽样未命中有效样本，不能据此判定范围没有体素")}});StopRead();return;}
        if(!m_isCounting){if(m_minimum==m_maximum){const auto half=std::max(0.5,std::abs(m_minimum)*1e-6);m_minimum-=half;m_maximum+=half;}m_isCounting=true;m_offset=0;m_timer.start(1);return;}
        auto data=HistogramData(m_counts,m_minimum,m_maximum,m_jobTitle,m_jobDescription+QString(" · 有效样本 %1 · 排除 %2").arg(m_used).arg(m_excluded),m_xUnit,m_yUnit);data["cacheKey"]=m_key;
        if(!m_comparisonCounts.empty()){QJsonArray other;for(auto value:m_comparisonCounts)other.append(value);data["comparisonCounts"]=other;}
        data["markers"]=m_data["markers"];data["status"]="Ready";m_state=State::Ready;Present(data);StopRead();
    } catch(const std::exception& error){if(generation!=m_generation)return;m_state=State::Failed;Present({{"cacheKey",m_key},{"status","Failed"},{"title",m_jobTitle},{"description","统计不可用："+QString::fromUtf8(error.what())+"；可按同一来源重试。"}});StopRead();}
}
}
