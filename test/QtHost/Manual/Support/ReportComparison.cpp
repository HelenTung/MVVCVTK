#include "ReportComparison.h"
#include "MappedArrayStorage.h"
#include <QUuid>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
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
std::array<qint64,3> Grid(const Position& position, double distance)
{
    std::array<qint64,3> cell{};
    for (int axis = 0; axis < 3; ++axis) {
        const auto value = std::floor(position[axis]/distance);
        // 留出邻域 +/-1，同时避免 double 到整数的越界转换。
        if (value <= -9e18 || value >= 9e18) throw std::invalid_argument("空间坐标与容差比例超出可匹配范围");
        cell[axis] = static_cast<qint64>(value);
    }
    return cell;
}
QJsonValue Count(quint64 count) {return count<=9007199254740991ULL?QJsonValue(static_cast<double>(count)):QJsonValue(QString::number(count));}
}
ReportDocument CompareReports(const ReportDocument& a, const ReportDocument& b, const ReportComparisonOptions& o)
{
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
    result.conditions = {{"基准报告",a.title},{"待比较报告",b.title},{"基准版本",a.id},{"待比较版本",b.id},
        {"比较指标",o.baselineQuantity},{"输出单位",o.baselineUnit},{"数值容差",o.valueTolerance}};
    const quint64 ac = a.table->GetCount(), bc = b.table->GetCount();
    if (o.match == ReportMatch::Distribution) {
        result.columns = {{"报告",{},{},false},{"总数",{},"1",true},{"有效数",{},"1",true},
            {"平均值_"+o.baselineUnit,o.baselineQuantity,o.baselineUnit,true},
            {"最小值_"+o.baselineUnit,o.baselineQuantity,o.baselineUnit,true},
            {"最大值_"+o.baselineUnit,o.baselineQuantity,o.baselineUnit,true}};
        const auto append = [&](const ReportDocument& doc, int column, double scale) {
            quint64 valid = 0; double mean = 0, low = std::numeric_limits<double>::infinity(), high = -low;
            for (quint64 i = 0; i < doc.table->GetCount(); ++i) {
                const auto row = doc.table->Read(i); double value = 0;
                if (!Valid(doc,row) || !GetReportNumber(GetReportCell(row,column),value) || !std::isfinite(value *= scale)) continue;
                ++valid; mean = mean*(static_cast<double>(valid-1)/valid)+value/valid;
                low = (std::min)(low,value); high = (std::max)(high,value);
            }
            result.table->Append({doc.title,QString::number(doc.table->GetCount()),QString::number(valid),
                valid?Number(mean):QString(),valid?Number(low):QString(),valid?Number(high):QString()});
        };
        append(a,o.baselineColumn,1); append(b,o.currentColumn,factor);
        result.conditions["匹配方式"] = "仅分布统计，不生成逐项差值";
        return result;
    }
    if(o.match==ReportMatch::Identity && (!a.identityResult.isEmpty() || !b.identityResult.isEmpty())
        && a.identityResult!=b.identityResult)
        throw std::runtime_error("对象/点编号的正式结果身份不同或缺失，不能按相同编号直接配对；请使用明确的跨结果匹配。");
    ResultArrayScope comparisonStorage;
    struct Candidate {quint64 row=0;std::uint8_t count=0;};
    DataArray<Candidate> candidates(static_cast<std::size_t>(ac));
    DataArray<std::uint8_t> incoming(static_cast<std::size_t>(bc),0),duplicate(static_cast<std::size_t>(ac),0);
    if (o.match == ReportMatch::Identity) {
        if (!o.identitiesConfirmed || o.baselineIdentity < 0 || o.currentIdentity < 0
            || o.baselineIdentity >= a.columns.size() || o.currentIdentity >= b.columns.size())
            throw std::invalid_argument("必须确认标识在两份报告之间稳定；行号和临时样本编号不能自动配对");
        auto left=ReportRows::Natural(ac),right=ReportRows::Natural(bc);
        left.Sort(a,o.baselineIdentity,true,false);right.Sort(b,o.currentIdentity,true,false);
        quint64 i=0,j=0;
        while(i<ac) {
            const auto key=GetReportCell(a.table->Read(left[i]),o.baselineIdentity);
            auto end=i+1;while(end<ac&&GetReportCell(a.table->Read(left[end]),o.baselineIdentity)==key)++end;
            if(key.isEmpty()){i=end;continue;}
            while(j<bc&&QString::compare(GetReportCell(b.table->Read(right[j]),o.currentIdentity),key)<0)++j;
            auto matchEnd=j;while(matchEnd<bc&&GetReportCell(b.table->Read(right[matchEnd]),o.currentIdentity)==key)++matchEnd;
            if(end-i!=1 || matchEnd-j>1) {
                for(auto p=i;p<end;++p)duplicate[left[p]]=1;
                for(auto p=j;p<matchEnd;++p)incoming[right[p]]=2;
            } else if(matchEnd>j){candidates[left[i]]={right[j],1};incoming[right[j]]=1;}
            i=end;j=matchEnd;
        }
        result.conditions["匹配方式"] = "已确认稳定业务标识";
    } else {
        if (!o.coordinatesConfirmed || !std::isfinite(o.distanceTolerance) || o.distanceTolerance <= 0)
            throw std::invalid_argument("空间匹配需要确认同一坐标系并设置正的距离容差（mm）");
        const auto ax = GetUnit(o.baselineCoordinateUnit), bx = GetUnit(o.currentCoordinateUnit);
        if (ax.dimension != 1 || bx.dimension != 1) throw std::invalid_argument("坐标单位必须为长度");
        if (a.xColumn < 0 || a.yColumn < 0 || a.zColumn < 0 || b.xColumn < 0 || b.yColumn < 0 || b.zColumn < 0)
            throw std::invalid_argument("请映射两侧三个坐标列");
        struct SpatialEntry {std::array<qint64,3> cell;Position point;quint64 row;};
        DataArray<SpatialEntry> positions;
        for(quint64 j=0;j<bc;++j){Position p;if(ReadPosition(b,b.table->Read(j),bx.scale,p))positions.push_back({Grid(p,o.distanceTolerance),p,j});}
        std::sort(positions.begin(),positions.end(),[](const auto& x,const auto& y){return x.cell!=y.cell?x.cell<y.cell:x.row<y.row;});
        for (quint64 i = 0; i < ac; ++i) {
            Position p;
            if (!ReadPosition(a,a.table->Read(i),ax.scale,p)) continue;
            const auto cell = Grid(p,o.distanceTolerance);
            for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z)
            {
                const std::array<qint64,3> key{cell[0]+x,cell[1]+y,cell[2]+z};
                auto found=std::lower_bound(positions.begin(),positions.end(),key,[](const auto& entry,const auto& k){return entry.cell<k;});
                for(;found!=positions.end()&&found->cell==key;++found) {
                    const auto& q=found->point;const auto j=found->row;
                    if(std::hypot(std::hypot(p[0]-q[0],p[1]-q[1]),p[2]-q[2])<=o.distanceTolerance) {
                        if(!candidates[i].count)candidates[i].row=j;
                        candidates[i].count=std::min<unsigned>(2,candidates[i].count+1);
                        incoming[j]=std::min<unsigned>(2,incoming[j]+1);
                    }
                }
            }
        }
        result.conditions["匹配方式"] = "确认同一坐标系的唯一空间配对";
        result.conditions["距离容差 mm"] = o.distanceTolerance;
    }
    const auto suffix = "_" + o.baselineUnit;
    result.columns = {{"对象/测点",{},{},false},{"指标",{},{},false},
        {"基准值"+suffix,o.baselineQuantity,o.baselineUnit,true},{"当前值"+suffix,o.baselineQuantity,o.baselineUnit,true},
        {"差值"+suffix,o.baselineQuantity,o.baselineUnit,true},{"绝对差"+suffix,o.baselineQuantity,o.baselineUnit,true},{"状态/原因",{},{},false}};
    DataArray<std::uint8_t> used(static_cast<std::size_t>(bc),0);
    quint64 pairs = 0, invalid = 0, missingA = 0, missingB = 0, ambiguous = 0, validA = 0, validB = 0;
    double mean = 0, maximum = 0;
    for (quint64 i = 0; i < ac; ++i) { double value; const auto row = a.table->Read(i); if (Valid(a,row) && GetReportNumber(GetReportCell(row,o.baselineColumn),value)) ++validA; }
    for (quint64 i = 0; i < bc; ++i) { double value; const auto row = b.table->Read(i); if (Valid(b,row) && GetReportNumber(GetReportCell(row,o.currentColumn),value) && std::isfinite(value*factor)) ++validB; }
    for (quint64 i = 0; i < ac; ++i) {
        const auto ar = a.table->Read(i); double av = 0, bv = 0;
        const bool va = Valid(a,ar) && GetReportNumber(GetReportCell(ar,o.baselineColumn),av);
        QString current, delta, absolute, status;
        if(duplicate[i]){status="歧义：重复业务标识";++ambiguous;}
        else if (!candidates[i].count) { status = "当前缺少对应项或标识/坐标缺失"; ++missingB; }
        else if (candidates[i].count != 1 || incoming[candidates[i].row] != 1) { status = "歧义：重复标识或空间多重匹配"; ++ambiguous; }
        else {
            const auto j = candidates[i].row; used[j] = 1; const auto br = b.table->Read(j);
            const bool vb = Valid(b,br) && GetReportNumber(GetReportCell(br,o.currentColumn),bv) && std::isfinite(bv *= factor);
            if (vb) current = Number(bv);
            if (!va || !vb || !std::isfinite(bv-av)) { status = "无效或缺测/差值超出数值范围"; ++invalid; }
            else { const auto d = bv-av, ad = std::abs(d); delta = Number(d); absolute = Number(ad); ++pairs;
                mean = mean*(static_cast<double>(pairs-1)/pairs)+ad/pairs; maximum = (std::max)(maximum,ad);
                status = ad <= o.valueTolerance ? "已匹配：容差内" : "已匹配：超出容差"; }
        }
        result.table->Append({o.match==ReportMatch::Identity?GetReportCell(ar,o.baselineIdentity):QString::number(i),o.baselineQuantity,va?Number(av):QString(),current,delta,absolute,status});
    }
    for (quint64 j = 0; j < bc; ++j) if (!used[j]) {
        const auto row = b.table->Read(j); double value = 0;
        const bool valid = Valid(b,row) && GetReportNumber(GetReportCell(row,o.currentColumn),value) && std::isfinite(value *= factor);
        const auto id = o.match == ReportMatch::Identity ? GetReportCell(row,o.currentIdentity) : QString::number(j);
        result.table->Append({id,o.baselineQuantity,{},valid?Number(value):QString(),{},{},incoming[j] ? "歧义：无法唯一配对" : "基准缺少对应项"});
        if (!incoming[j]) ++missingA;
    }
    result.conditions["两侧总数"] = QString("%1 / %2").arg(ac).arg(bc);
    result.conditions["有效数"] = QString("%1 / %2").arg(validA).arg(validB);
    result.conditions["有效配对数"] = Count(pairs); result.conditions["无效/缺测配对数"] = Count(invalid);
    result.conditions["基准未匹配数"] = Count(ac-pairs-invalid); result.conditions["当前未匹配数"] = Count(bc-pairs-invalid);
    result.conditions["基准侧无候选数"] = Count(missingB);result.conditions["当前侧无候选数"] = Count(missingA);
    result.conditions["歧义基准项数"] = Count(ambiguous);
    result.conditions["平均绝对差"] = pairs ? QJsonValue(mean) : QJsonValue();
    result.conditions["最大绝对差"] = pairs ? QJsonValue(maximum) : QJsonValue();
    result.conditions["报告匹配覆盖率"] = ac ? QJsonValue(static_cast<double>(pairs+invalid)/ac) : QJsonValue();
    result.conditions["算法测量有效覆盖率"] = "请查看原生业务统计；与报告匹配覆盖率不同";
    return result;
}
}
