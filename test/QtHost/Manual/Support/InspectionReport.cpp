#include "InspectionReport.h"
#include "JsonInput.h"
#include <QMap>
#include <cmath>
#include "Data/Support/PagedArray.h"
#include <vector>

namespace Manual {
namespace {
QString Number(const std::optional<double>& value)
{
    return value && std::isfinite(*value) ? QString::number(*value,'g',17) : QString();
}
struct InspectionRows final {
    struct Row {std::size_t first=0,second=0;};
    std::size_t planIndex=0,reportIndex=0;
    struct Node {std::size_t index=0;std::uint64_t next=0;};
    PagedMap<std::uint64_t,std::uint64_t> specifications;
    PagedArray<Node> nodes;
    QMap<int,std::shared_ptr<PagedArray<Row>>> groups;
    bool released=false;
    std::optional<std::size_t> Find(const InspectionPlan& plan,const std::string& id) {
        const auto head=specifications.Find(std::hash<std::string>{}(id));if(!head)return {};
        for(auto node=*head;node;node=nodes[static_cast<std::size_t>(node-1)].next){const auto index=nodes[static_cast<std::size_t>(node-1)].index;if(plan.items[index].id==id)return index;}
        return {};
    }
    void Add(const InspectionPlan& plan,std::size_t index) {
        const auto& id=plan.items[index].id;if(Find(plan,id))return;
        const auto hash=std::hash<std::string>{}(id);const auto head=specifications.Find(hash);
        std::uint64_t last=head?*head:0;
        while(last&&nodes[static_cast<std::size_t>(last-1)].next)last=nodes[static_cast<std::size_t>(last-1)].next;
        nodes.push_back({index,0});const auto value=static_cast<std::uint64_t>(nodes.size());
        if(last)nodes[static_cast<std::size_t>(last-1)].next=value;else specifications.Insert(hash,value);
    }
};
}
ReportDocument CreateInspectionReport(const QJsonObject& record,const InspectionSnapshot& snapshot,const QString& source)
{
    ReportDocument base;base.id="inspection:"+GetRefText(snapshot.result);base.title="计量明细（空结果） · "+record["operationId"].toString();
    base.category="本次会话结果";base.module=record["module"].toString();base.result=GetRefText(snapshot.result);base.source=source;
    base.relatedResults.append(GetRefText(snapshot.alignmentResult));base.conditions={{"计算参数",record["parameters"]}};
    const auto plan=snapshot.plan;
    const auto report=snapshot.report;
    const auto state=std::make_shared<InspectionRows>();
    const auto length=snapshot.unit==AlignmentUnit::Millimeter?QString("mm"):snapshot.unit==AlignmentUnit::Meter?QString("m"):QString("model");
    base.prepareReports=[base,plan,report,state,length](QVector<ReportDocument>& documents) {
        // 保留按 id 查找以及计划中首次出现项的语义，不依赖计划/结果排列一致。
        if(state->planIndex<plan->items.size()) {
            const auto index=state->planIndex++;state->Add(*plan,index);return false;
        }
        if(state->reportIndex<report->items.size()) {
            const auto index=state->reportIndex++;const auto spec=state->Find(*plan,report->items[index].id);
            if(spec) {
                const int metric=static_cast<int>(plan->items[*spec].measured.metric);
                auto& rows=state->groups[metric];if(!rows)rows=std::make_shared<PagedArray<InspectionRows::Row>>();
                rows->push_back({index,*spec});
            }
            return false;
        }
        // 关联键、链节点及分组目录均在外存中，不保留 N 项字符串哈希表。
        if(!state->released){state->specifications.clear();state->nodes=PagedArray<InspectionRows::Node>{};state->released=true;return false;}
        const QStringList metrics{"半径","直径","中心 X","中心 Y","中心 Z","中心距离","有向角度","平行平面有符号距离","拟合 RMS","拟合最大残差"};
        for(auto it=state->groups.cbegin();it!=state->groups.cend();++it) {
            auto doc=base;const auto quantity=metrics.value(it.key(),"项目");
            const auto unit=it.key()==static_cast<int>(InspectionMetric::OrientedDirectionAngleDegrees)?QString("deg"):length;
            doc.id="inspection-"+QString::number(it.key())+":"+base.result;doc.title="计量 · "+quantity+base.title.mid(base.title.indexOf(" · "));
            doc.columns={{"item_id",{},{},false},{"name",{},{},false},{"nominal_"+unit,quantity,unit,true},
                {"measured_"+unit,quantity,unit,true},{"deviation_"+unit,quantity+"相对名义偏差",unit,true},{"verdict",{},{},false},{"reason",{},{},false}};
            doc.identityColumn=0;doc.valueColumn=3;doc.identityStable=true;doc.table=std::make_shared<ReportTable>();
            const auto rows=std::make_shared<const MeshReadArray<InspectionRows::Row>>(it.value()->GetReadSnapshot());doc.table->SetRows(rows->size(),[plan,report,rows](ReportRowId index) {
                const auto& mapping=(*rows)[index];const auto& item=report->items[mapping.first];const auto& spec=plan->items[mapping.second];
                return QStringList{QString::fromStdString(item.id),QString::fromStdString(spec.name),Number(item.nominal),Number(item.measured),Number(item.deviation),
                    QString::number(static_cast<int>(item.verdict)),QString::fromStdString(item.message)};
            });
            documents.append(std::move(doc));
        }
        if(documents.isEmpty()){auto empty=base;empty.columns={{"item_id",{},{},false}};empty.table=std::make_shared<ReportTable>();documents.append(std::move(empty));}
        documents.first().id=base.id;return true;
    };
    return base;
}
}
