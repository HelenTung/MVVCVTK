#include "ReportComparison.h"
#include "ReportStorage.h"
#include <QHash>
#include <QUuid>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <optional>
namespace Manual {
namespace {
struct Unit { int dimension; double scale; };
Unit GetUnit(const QString& unit)
{
    if (unit == "1") return {0,1};
    if (unit == "deg") return {4,1};
    if (unit == "rad") return {4,180.0/3.14159265358979323846};
    for (int dimension = 1; dimension <= 3; ++dimension) {
        const auto suffix = dimension == 1 ? QString() : QString::number(dimension);
        if (unit == "mm" + suffix) return {dimension,1};
        if (unit == "cm" + suffix) return {dimension,std::pow(10.,dimension)};
        if (unit == "m" + suffix) return {dimension,std::pow(1000.,dimension)};
        if (unit == "um" + suffix) return {dimension,std::pow(.001,dimension)};
    }
    throw std::invalid_argument("请明确单位：1、mm/cm/m/um、平方/立方单位或 deg/rad；未知模型单位不能换算");
}
bool Valid(const ReportDocument& doc, const QStringList& row)
{
    if (doc.validityColumn < 0) return true;
    const auto value = GetReportCell(row,doc.validityColumn).trimmed();
    for(const auto& valid:doc.validValues)if(value.compare(valid,Qt::CaseInsensitive)==0)return true;
    return false;
}
QString Number(double value) { return std::isfinite(value) ? QString::number(value,'g',17) : QString(); }
using Position = std::array<double,3>;
bool ReadPosition(const ReportDocument& doc, const QStringList& row, double scale, Position& position)
{
    const std::array<int,3> columns{doc.xColumn,doc.yColumn,doc.zColumn};
    for (int axis = 0; axis < 3; ++axis) {
        if (!GetReportNumber(GetReportCell(row,columns[axis]),position[axis])) return false;
        position[axis] *= scale;
        if (!std::isfinite(position[axis])) return false;
    }
    return true;
}
std::optional<std::array<qint64,3>> Grid(const Position& position, double distance)
{
    std::array<qint64,3> cell{};
    for (int axis = 0; axis < 3; ++axis) {
        const auto value = std::floor(position[axis]/distance);
        // 留出邻域 +/-1，同时避免 double 到整数的越界转换。
        if (!std::isfinite(value) || value <= -9e18 || value >= 9e18) return {};
        cell[axis] = static_cast<qint64>(value);
    }
    return cell;
}
struct GridHash {
    std::size_t operator()(const std::array<qint64,3>& cell) const noexcept {
        std::size_t hash=0;for(const auto value:cell)hash^=std::hash<qint64>{}(value)+0x9e3779b97f4a7c15ULL+(hash<<6)+(hash>>2);return hash;
    }
};
struct IdentityHash {
    std::size_t operator()(const QString& text) const noexcept {
        quint64 hash=14695981039346656037ULL;for(const auto c:text){hash^=c.unicode();hash*=1099511628211ULL;}return static_cast<std::size_t>(hash);
    }
};
struct Candidate {ReportRowId first=0;unsigned char count=0;};
struct IdentityGroup {ReportRowId first=0,count=0;};
ReportRowId BucketCount(ReportRowId rows) {
    ReportRowId count=1;while(count<rows/2){if(count>std::numeric_limits<ReportRowId>::max()/2)throw std::overflow_error("报告匹配索引地址溢出");count*=2;}return count;
}
class IdentityIndex final {
    struct Node {quint64 hash=0;ReportRowId next=0,first=0,count=0;};
public:
    IdentityIndex(ReportRowId count,std::function<QStringList(ReportRowId)> read,int column,const ReportCheckpoint& check)
        :m_buckets(BucketCount(count)),m_heads(m_buckets,check),m_nodes(count,check),m_read(std::move(read)),m_column(column),m_check(check){}
    void Add(ReportRowId row,const QString& id){
        if(id.isEmpty())return;const auto hash=IdentityHash{}(id);const auto bucket=hash%static_cast<quint64>(m_buckets);
        for(auto entry=m_heads[bucket];entry;entry=m_nodes[entry-1].next){if(m_check)m_check();auto& node=m_nodes[entry-1];
            if(node.hash==hash && GetReportCell(m_read(node.first),m_column)==id){++node.count;return;}}
        auto& node=m_nodes[row];node={hash,m_heads[bucket],row,1};m_heads[bucket]=row+1;
    }
    IdentityGroup Find(const QString& id) const{
        if(id.isEmpty())return {};const auto hash=IdentityHash{}(id);
        for(auto entry=m_heads[hash%static_cast<quint64>(m_buckets)];entry;entry=m_nodes[entry-1].next){if(m_check)m_check();const auto& node=m_nodes[entry-1];
            if(node.hash==hash && GetReportCell(m_read(node.first),m_column)==id)return {node.first,node.count};}return {};
    }
private:
    ReportRowId m_buckets;
    ReportDiskArray<ReportRowId> m_heads;
    ReportDiskArray<Node> m_nodes;
    std::function<QStringList(ReportRowId)> m_read;
    int m_column;
    ReportCheckpoint m_check;
};
class PositionIndex final {
    struct Node {std::array<qint64,3> cell{};Position position{};ReportRowId next=0;};
public:
    PositionIndex(ReportRowId count,const ReportCheckpoint& check):m_buckets(BucketCount(count)),m_heads(m_buckets,check),m_nodes(count,check),m_check(check){}
    void Add(ReportRowId row,const Position& position,double tolerance){
        const auto cell=Grid(position,tolerance);if(!cell){m_gridAvailable=false;return;}
        const auto bucket=GridHash{}(*cell)%static_cast<quint64>(m_buckets);
        m_nodes[row]={*cell,position,m_heads[bucket]};m_heads[bucket]=row+1;
    }
    bool GetGridAvailable() const{return m_gridAvailable;}
    template<class Found> void Find(const std::array<qint64,3>& cell,const Position& position,double tolerance,Found found) const{
        for(auto entry=m_heads[GridHash{}(cell)%static_cast<quint64>(m_buckets)];entry;entry=m_nodes[entry-1].next){
            if(m_check)m_check();const auto& node=m_nodes[entry-1];const auto& q=node.position;
            if(node.cell==cell && std::hypot(std::hypot(position[0]-q[0],position[1]-q[1]),position[2]-q[2])<=tolerance)found(entry-1);
        }
    }
private:
    ReportRowId m_buckets;
    ReportDiskArray<ReportRowId> m_heads;
    ReportDiskArray<Node> m_nodes;
    ReportCheckpoint m_check;
    bool m_gridAvailable=true;
};
}
ReportDocument CompareReports(const ReportDocument& a, const ReportDocument& b, const ReportComparisonOptions& o, const ReportCheckpoint& checkpoint)
{
    const auto check = [&] { if(checkpoint)checkpoint(); };
    check();
    if((a.mappingRequired || b.mappingRequired) && !o.mappingConfirmed)throw std::invalid_argument("报告存在重名列，比较前必须明确确认列映射及有效性含义");
    if (!a.table || !b.table || o.baselineColumn < 0 || o.baselineColumn >= a.columns.size()
        || o.currentColumn < 0 || o.currentColumn >= b.columns.size()) throw std::invalid_argument("请选择两份报告的比较数值列");
    const auto checkNative=[](const ReportDocument& doc,int column,const QString& quantity,const QString& unit) {
        if(doc.category!="本次会话结果")return;
        const auto& definition=doc.columns[column];
        if(!definition.quantity.isEmpty() && definition.quantity!=quantity)throw std::invalid_argument("不能重新命名原生量值的业务含义来混合比较");
        if(!definition.unit.isEmpty() && definition.unit!=unit)throw std::invalid_argument("不能重新标记原生数据单位；应在比较中换算另一侧值");
    };
    checkNative(a,o.baselineColumn,o.baselineQuantity,o.baselineUnit);checkNative(b,o.currentColumn,o.currentQuantity,o.currentUnit);
    if (o.baselineQuantity.trimmed().isEmpty() || o.baselineQuantity != o.currentQuantity)
        throw std::invalid_argument("比较指标含义必须一致；完整体积、ROI 相交体积及统计体积不能混用");
    const auto au = GetUnit(o.baselineUnit), bu = GetUnit(o.currentUnit);
    if (au.dimension != bu.dimension) throw std::invalid_argument("两侧单位维度不同");
    if (!std::isfinite(o.valueTolerance) || o.valueTolerance < 0) throw std::invalid_argument("数值容差必须为非负有限值");
    const double factor = bu.scale / au.scale;
    ReportDocument result; result.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    result.title = b.title + " − " + a.title; result.category = "对比结果"; result.table = std::make_shared<ReportTable>();
    result.description = "结果差异；基准具有独立参考依据时才可解释为参考误差。";
    const auto associate = [&](const ReportDocument& doc) {
        if(!doc.source.isEmpty())result.relatedSources.append(doc.source);
        result.relatedSources.append(doc.relatedSources);
        if(!doc.result.isEmpty())result.relatedResults.append(doc.result);
        result.relatedResults.append(doc.relatedResults);
        if(!doc.module.isEmpty())result.relatedModules.append(doc.module);
        result.relatedModules.append(doc.relatedModules);
    };
    associate(a); associate(b); result.relatedSources.removeDuplicates(); result.relatedResults.removeDuplicates();
    result.relatedModules.removeDuplicates();
    if(result.relatedSources.size()==1)result.source=result.relatedSources.first();
    if(result.relatedModules.size()==1)result.module=result.relatedModules.first();
    result.conditions = {{"基准报告",a.title},{"待比较报告",b.title},{"基准版本",a.id},{"待比较版本",b.id},
        {"比较指标",o.baselineQuantity},{"输出单位",o.baselineUnit},{"数值容差",o.valueTolerance}};
    const auto ac = a.table->GetCount(), bc = b.table->GetCount();
    const auto readA=a.table->GetIsComplete()?a.table->CreateReader():[&a](ReportRowId row){return a.table->Read(row);};
    const auto readB=b.table->GetIsComplete()?b.table->CreateReader():[&b](ReportRowId row){return b.table->Read(row);};
    if (o.match == ReportMatch::Distribution) {
        result.columns = {{"报告",{},{},false},{"总数",{},"1",true},{"有效数",{},"1",true},
            {"平均值_"+o.baselineUnit,o.baselineQuantity,o.baselineUnit,true},
            {"最小值_"+o.baselineUnit,o.baselineQuantity,o.baselineUnit,true},
            {"最大值_"+o.baselineUnit,o.baselineQuantity,o.baselineUnit,true}};
        const auto append = [&](const ReportDocument& doc, int column, double scale) {
            ReportRowId valid = 0; double mean = 0, low = std::numeric_limits<double>::infinity(), high = -low;
            const auto& read=&doc==&a?readA:readB;
            const auto count=&doc==&a?ac:bc;
            for (ReportRowId i = 0; i < count; ++i) {
                check(); const auto row = read(i); double value = 0;
                if (!Valid(doc,row) || !GetReportNumber(GetReportCell(row,column),value) || !std::isfinite(value *= scale)) continue;
                ++valid; mean = mean*(static_cast<double>(valid-1)/valid)+value/valid;
                low = std::min(low,value); high = std::max(high,value);
            }
            result.table->Append({doc.title,QString::number(count),QString::number(valid),
                valid?Number(mean):QString(),valid?Number(low):QString(),valid?Number(high):QString()});
        };
        append(a,o.baselineColumn,1); append(b,o.currentColumn,factor);
        result.conditions["匹配方式"] = "仅分布统计，不生成逐项差值";
        return result;
    }
    ReportDiskArray<Candidate> candidates(ac,checkpoint);ReportDiskArray<unsigned char> incoming(bc,checkpoint),duplicate(ac,checkpoint);
    if (o.match == ReportMatch::Identity) {
        if (!o.identitiesConfirmed || o.baselineIdentity < 0 || o.currentIdentity < 0
            || o.baselineIdentity >= a.columns.size() || o.currentIdentity >= b.columns.size())
            throw std::invalid_argument("必须确认标识在两份报告之间稳定；行号和临时样本编号不能自动配对");
        IdentityIndex left(ac,readA,o.baselineIdentity,checkpoint),right(bc,readB,o.currentIdentity,checkpoint);
        for(ReportRowId i=0;i<ac;++i){check();left.Add(i,GetReportCell(readA(i),o.baselineIdentity));}
        for(ReportRowId j=0;j<bc;++j){check();right.Add(j,GetReportCell(readB(j),o.currentIdentity));}
        for(ReportRowId i=0;i<ac;++i) {
            check();const auto id=GetReportCell(readA(i),o.baselineIdentity);const auto l=left.Find(id),r=right.Find(id);
            if(!l.count)continue;
            if(l.count!=1 || r.count>1)duplicate[i]=true;
            else if(r.count)candidates[i]={r.first,1};
        }
        for(ReportRowId j=0;j<bc;++j){check();const auto id=GetReportCell(readB(j),o.currentIdentity);const auto l=left.Find(id),r=right.Find(id);
            if(l.count)incoming[j]=l.count!=1 || r.count>1?2:1;}
        result.conditions["匹配方式"] = "已确认稳定业务标识";
    } else {
        if (!o.coordinatesConfirmed || !std::isfinite(o.distanceTolerance) || o.distanceTolerance <= 0)
            throw std::invalid_argument("空间匹配需要确认同一坐标系并设置正的距离容差（mm）");
        const auto ax = GetUnit(o.baselineCoordinateUnit), bx = GetUnit(o.currentCoordinateUnit);
        if (ax.dimension != 1 || bx.dimension != 1) throw std::invalid_argument("坐标单位必须为长度");
        if (a.xColumn < 0 || a.yColumn < 0 || a.zColumn < 0 || b.xColumn < 0 || b.yColumn < 0 || b.zColumn < 0)
            throw std::invalid_argument("请映射两侧三个坐标列");
        PositionIndex positions(bc,checkpoint);
        for (ReportRowId j = 0; j < bc; ++j) { check();Position p;if(ReadPosition(b,readB(j),bx.scale,p))positions.Add(j,p,o.distanceTolerance); }
        for (ReportRowId i = 0; i < ac; ++i) {
            check(); const auto row=readA(i);
            Position p;
            if (!ReadPosition(a,row,ax.scale,p)) continue;
            const auto cell = Grid(p,o.distanceTolerance);
            const auto found=[&](ReportRowId j){auto& candidate=candidates[i];if(!candidate.count)candidate.first=j;if(candidate.count<2)++candidate.count;if(incoming[j]<2)++incoming[j];};
            // 网格是加速结构，量化不能表示的合法坐标仍用完整扫描精确匹配。
            if(!cell || !positions.GetGridAvailable()){
                for(ReportRowId j=0;j<bc;++j){check();Position q;if(ReadPosition(b,readB(j),bx.scale,q) && std::hypot(std::hypot(p[0]-q[0],p[1]-q[1]),p[2]-q[2])<=o.distanceTolerance)found(j);}
                continue;
            }
            for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z) {
                positions.Find({(*cell)[0]+x,(*cell)[1]+y,(*cell)[2]+z},p,o.distanceTolerance,found);
            }
        }
        result.conditions["匹配方式"] = "确认同一坐标系的唯一空间配对";
        result.conditions["距离容差 mm"] = o.distanceTolerance;
    }
    const auto suffix = "_" + o.baselineUnit;
    result.columns = {{"对象/测点",{},{},false},{"指标",{},{},false},
        {"基准值"+suffix,o.baselineQuantity,o.baselineUnit,true},{"当前值"+suffix,o.baselineQuantity,o.baselineUnit,true},
        {"差值"+suffix,o.baselineQuantity,o.baselineUnit,true},{"绝对差"+suffix,o.baselineQuantity,o.baselineUnit,true},{"状态/原因",{},{},false},
        {"当前对象/测点",{},{},false},{"配对距离_mm","配对距离","mm",true}};
    result.columns[0].name="基准对象/测点";
    ReportDiskArray<unsigned char> used(bc,checkpoint);
    ReportRowId pairs = 0, invalid = 0, missingA = 0, missingB = 0, ambiguous = 0, validA = 0, validB = 0;
    double mean = 0, maximum = 0;
    for (ReportRowId i = 0; i < ac; ++i) { check(); double value; const auto row = readA(i); if (Valid(a,row) && GetReportNumber(GetReportCell(row,o.baselineColumn),value)) ++validA; }
    for (ReportRowId i = 0; i < bc; ++i) { check(); double value; const auto row = readB(i); if (Valid(b,row) && GetReportNumber(GetReportCell(row,o.currentColumn),value) && std::isfinite(value*factor)) ++validB; }
    for (ReportRowId i = 0; i < ac; ++i) {
        check(); const auto ar = readA(i); double av = 0, bv = 0;
        const bool va = Valid(a,ar) && GetReportNumber(GetReportCell(ar,o.baselineColumn),av);
        QString current, delta, absolute, status, currentId, distance;
        if(duplicate[i]){status="歧义：重复业务标识";++ambiguous;}
        else if (!candidates[i].count) { status = "当前缺少对应项或标识/坐标缺失"; ++missingB; }
        else if (candidates[i].count != 1 || incoming[candidates[i].first] != 1) { status = "歧义：重复标识或空间多重匹配"; ++ambiguous; }
        else {
            const auto j = candidates[i].first; used[j] = true; const auto br = readB(j);
            const int idColumn=o.currentIdentity>=0?o.currentIdentity:b.identityColumn;
            currentId=idColumn>=0?GetReportCell(br,idColumn):QString::number(j);
            if(o.match==ReportMatch::Position) {
                Position p,q; ReadPosition(a,ar,GetUnit(o.baselineCoordinateUnit).scale,p); ReadPosition(b,br,GetUnit(o.currentCoordinateUnit).scale,q);
                distance=Number(std::hypot(std::hypot(p[0]-q[0],p[1]-q[1]),p[2]-q[2]));
            }
            const bool vb = Valid(b,br) && GetReportNumber(GetReportCell(br,o.currentColumn),bv) && std::isfinite(bv *= factor);
            if (vb) current = Number(bv);
            if (!va || !vb || !std::isfinite(bv-av)) { status = "无效或缺测/差值超出数值范围"; ++invalid; }
            else { const auto d = bv-av, ad = std::abs(d); delta = Number(d); absolute = Number(ad); ++pairs;
                mean = mean*(static_cast<double>(pairs-1)/pairs)+ad/pairs; maximum = std::max(maximum,ad);
                status = ad <= o.valueTolerance ? "已匹配：容差内" : "已匹配：超出容差"; }
        }
        const int idColumn=o.baselineIdentity>=0?o.baselineIdentity:a.identityColumn;
        const auto identity=idColumn>=0?GetReportCell(ar,idColumn):QString::number(i);
        result.table->Append({identity,o.baselineQuantity,va?Number(av):QString(),current,delta,absolute,status,currentId,distance});
    }
    for (ReportRowId j = 0; j < bc; ++j) if (!used[j]) {
        check(); const auto row = readB(j); double value = 0;
        const bool valid = Valid(b,row) && GetReportNumber(GetReportCell(row,o.currentColumn),value) && std::isfinite(value *= factor);
        const int idColumn=o.currentIdentity>=0?o.currentIdentity:b.identityColumn;
        const auto id=idColumn>=0?GetReportCell(row,idColumn):QString::number(j);
        result.table->Append({{},o.baselineQuantity,{},valid?Number(value):QString(),{},{},incoming[j] ? "歧义：无法唯一配对" : "基准缺少对应项",id,{}});
        if (!incoming[j]) ++missingA;
    }
    result.conditions["两侧总数"] = QString("%1 / %2").arg(ac).arg(bc);
    result.conditions["有效数"] = QString("%1 / %2").arg(validA).arg(validB);
    result.conditions["有效配对数"] = GetReportCountValue(pairs); result.conditions["无效/缺测配对数"] = GetReportCountValue(invalid);
    result.conditions["基准未匹配数"] = GetReportCountValue(ac-pairs-invalid); result.conditions["当前未匹配数"] = GetReportCountValue(bc-pairs-invalid);
    result.conditions["基准侧无候选数"] = GetReportCountValue(missingB);result.conditions["当前侧无候选数"] = GetReportCountValue(missingA);
    result.conditions["歧义基准项数"] = GetReportCountValue(ambiguous);
    result.conditions["平均绝对差"] = pairs ? QJsonValue(mean) : QJsonValue();
    result.conditions["最大绝对差"] = pairs ? QJsonValue(maximum) : QJsonValue();
    result.conditions["报告匹配覆盖率"] = ac ? QJsonValue(static_cast<double>(pairs+invalid)/ac) : QJsonValue();
    result.conditions["算法测量有效覆盖率"] = "请查看原生业务统计；与报告匹配覆盖率不同";
    return result;
}
}
