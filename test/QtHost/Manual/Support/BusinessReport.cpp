#include "BusinessReport.h"
#include "App/TestWorkflow.h"
#include "JsonInput.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#if defined(MANUAL_GAP)
#include "Host/GapHostFeature.h"
#endif
#if defined(MANUAL_WALL)
#include "Host/WallThicknessHostFeature.h"
#include "WallParameters.h"
#endif
#if defined(MANUAL_SURFACE)
#include "Host/SurfaceDeterminationHostFeature.h"
#endif
#if defined(MANUAL_ALIGNMENT)
#include "Host/MetrologyAlignmentHostFeature.h"
#include "InspectionReport.h"
#endif
namespace Manual {
namespace {
QString Number(double value) { return std::isfinite(value)?QString::number(value,'g',17):QString(); }
bool HasRevision(const QString& ref) { return !ref.isEmpty() && !ref.endsWith(":0"); }
QString Cell(const QJsonValue& value)
{
    if(value.isDouble())return Number(value.toDouble());
    if(value.isString())return value.toString();
    if(value.isBool())return value.toBool()?"true":"false";
    return {};
}
ReportDocument Document(const QJsonObject& record,const QString& prefix,const QString& ref,const QString& source,const QString& title)
{
    ReportDocument doc;doc.id=prefix+":"+ref;doc.title=title+" · "+record["operationId"].toString();
    doc.category="本次会话结果";doc.module=record["module"].toString();doc.result=ref;doc.source=source;
    doc.conditions={{"计算参数",record["parameters"]}};return doc;
}
template<class Read>
void SetPagedRows(ReportDocument& doc, const QString& ref, Read read, QStringList keys, QJsonObject first)
{
    const auto count=GetId(first["total"]);
    doc.table->SetRows(static_cast<std::size_t>(count),[ref,read,keys,first,offset=ReportRowId(0)](ReportRowId index) mutable {
        auto rows=first["rows"].toArray();
        if(index<offset || index>=offset+rows.size()) {
            offset=index/256*256; first=read(GetRef(ref),static_cast<std::size_t>(offset),256,std::nullopt);rows=first["rows"].toArray();
        }
        if(index<offset || index>=offset+rows.size())throw std::runtime_error("业务报告分页缺少预期行");
        const auto row=rows[static_cast<int>(index-offset)].toObject();QStringList cells;
        for(const auto& key:keys)cells.append(Cell(row[key]));return cells;
    });
}
}
#if defined(MANUAL_ARTIFACT)
void AttachReportSource(TestWorkflow& workflow,std::shared_ptr<ArtifactReductionHostFeature>)
{
    const auto read=workflow.readRecordPage;
    workflow.readReports["Artifact"]=[read](const QJsonObject& record,const QSet<QString>& known) {
        QVector<ReportDocument> reports;const auto r=record["result"].toObject();const auto ref=r["qualityReport"].toString();
        if(!HasRevision(ref) || known.contains("artifact:"+ref))return reports;
        const auto first=read(GetRef(ref),0,256,std::nullopt);
        if(!HasRevision(first["source"].toString()))throw std::runtime_error("去伪影质量报告缺少原生源修订");
        auto doc=Document(record,"artifact",ref,first["source"].toString(),"去伪影质量明细");
        doc.conditions["计算参数"]=QJsonDocument::fromJson(first["canonicalParameters"].toString().toUtf8()).object();
        doc.conditions["源修订"]=doc.source;doc.conditions["校正结果修订"]=r["correctedVolume"];doc.conditions["质量表修订"]=ref;
        doc.relatedResults.append(r["correctedVolume"].toString());
        doc.description="每行使用原生质量表的指标与单位；灰度变化不能单独证明校正保真，不同指标不混合统计。";
        doc.columns={{"metric",{},{},false},{"value",{},{},true},{"unit",{},{},false}};
        doc.identityColumn=0;doc.identityStable=true;doc.table=std::make_shared<ReportTable>();
        SetPagedRows(doc,ref,read,{"metric","value","unit"},first);
        reports.append(std::move(doc));return reports;
    };
}
#endif
#if defined(MANUAL_GAP)
void AttachReportSource(TestWorkflow& workflow,std::shared_ptr<GapHostFeature> feature)
{
    workflow.readReports["Gap"]=[feature](const QJsonObject& record,const QSet<QString>& known) {
        QVector<ReportDocument> reports;const auto r=record["result"].toObject();const auto table=r["voidTable"].toString();
        if(!HasRevision(table))return reports;
        auto doc=Document(record,"gap",table,r["source"].toString(),"孔隙明细");if(known.contains(doc.id))return reports;
        doc.conditions["汇总"]=QJsonObject{{"对象体积 mm3",r["objectVolumeMM3"]},{"孔隙体积 mm3",r["voidVolumeMM3"]},{"孔隙率",r["porosityRatio"]}};
        doc.result=r["resultSet"].toString();doc.description="一行一个父缺陷/ROI 相交记录；ROI 片段不是新封闭孔隙。";
        const auto fullReader=feature->GetResultReader(GetRef(r["resultSet"]));
        if(!fullReader)throw std::runtime_error("孔隙正式对象目录不可读");
        const auto reader=fullReader->CreateObjectReader();
        doc.identityResult=GetRefText(reader->GetMetadata().result);doc.source=GetRefText(reader->GetMetadata().source);
        doc.relatedResults.append(table);
        doc.columns={{"void_id",{},{},false},{"parent_volume_mm3","完整父缺陷体积","mm3",true},
            {"intersection_volume_mm3","ROI 内相交体积","mm3",true},{"gap_statistics_volume_mm3","本次统计体积","mm3",true},
            {"equivalent_diameter_mm","本次统计等效直径","mm",true},{"roi_partial",{},{},false},{"whole_object_geometry",{},{},false}};
        doc.identityColumn=0;
        doc.table=std::make_shared<ReportTable>(reader->GetMetadata().objectCount,[reader](ReportRowId row){
            const auto page=reader->GetObjects(row,1);
            if(page.error!=GapReadError::None||page.objects.size()!=1)throw std::runtime_error("孔隙报告对象行不可读");
            const auto& object=page.objects[0];
            return QStringList{QString::number(object.id),Number(object.parentVolumeMM3),Number(object.intersectionVolumeMM3),
                Number(object.volumeMM3),Number(object.equivalentDiameterMM),object.isRoiPartial?"1":"0",object.hasWholeObjectGeometry?"1":"0"};
        });
        reports.append(std::move(doc));return reports;
    };
}
#endif
#if defined(MANUAL_WALL)
void AttachReportSource(TestWorkflow& workflow,std::shared_ptr<WallThicknessHostFeature> feature)
{
    workflow.readReports["Wall"]=[feature](const QJsonObject& record,const QSet<QString>& known) {
        QVector<ReportDocument> reports;const auto ref=record["result"].toObject()["result"].toString();if(!HasRevision(ref) || known.contains("wall:"+ref))return reports;
        const auto snapshot=feature->GetResult(GetRef(ref));if(!snapshot || !snapshot->samples)return reports;
        const auto unit=snapshot->archive.input.unit==ThicknessUnit::Millimeter?QString("mm"):snapshot->archive.input.unit==ThicknessUnit::Meter?QString("m"):QString("model");
        auto doc=Document(record,"wall",ref,GetRefText(snapshot->archive.input.source),"壁厚明细");
        doc.conditions["计算参数"]=GetWallMeasurementJson(snapshot->archive.input,snapshot->archive.params);
        doc.conditions["评价参数"]=GetWallEvaluationJson(snapshot->archive.evaluation);
        doc.conditions["算法版本"]=QString::fromStdString(snapshot->archive.algorithmVersion);
        doc.description="聚合节点场查询值；sample_id 只在本报告内编号，不是跨结果稳定测点。无效厚度留空。";doc.coordinateFrame="源模型坐标";
        doc.columns={{"sample_id",{},{},false},{"x_"+unit,"坐标 X",unit,true},{"y_"+unit,"坐标 Y",unit,true},{"z_"+unit,"坐标 Z",unit,true},
            {"thickness_"+unit,"聚合节点厚度场值",unit,true},{"area_"+unit+"2","样本面积",unit+"2",true},{"validity",{},{},false},{"reason",{},{},false}};
        doc.identityColumn=0;doc.valueColumn=4;doc.xColumn=1;doc.yColumn=2;doc.zColumn=3;doc.validityColumn=6;doc.table=std::make_shared<ReportTable>();
        const QStringList reasons{"有效","表面质量不足","对面歧义","尖锐特征","支持截断","达到搜索上限","材料路径无效","评价范围外","无有效源"};
        const auto samples=snapshot->samples;
        doc.table->SetRows(samples->size(),[samples,reasons](ReportRowId index) {
            const auto& sample=(*samples)[index];
            const int validity=static_cast<int>(sample.validity);
            return QStringList{QString::number(index),Number(sample.source[0]),Number(sample.source[1]),Number(sample.source[2]),
                sample.validity==ThicknessValidity::Valid?Number(sample.thickness):QString(),Number(sample.area),QString::number(validity),reasons.value(validity,"未知有效性")};
        });
        doc.conditions["测量有效覆盖率（面积）"]=snapshot->statistics.evaluatedArea>0?QJsonValue(snapshot->statistics.validArea/snapshot->statistics.evaluatedArea):QJsonValue();
        const auto& stats=snapshot->statistics;
        doc.conditions["汇总"]=QJsonObject{{"样本数",QString::number(stats.sampleCount)},{"有效数",QString::number(stats.validCount)},
            {"最小值",stats.minimum?QJsonValue(*stats.minimum):QJsonValue()},{"最大值",stats.maximum?QJsonValue(*stats.maximum):QJsonValue()},
            {"面积加权平均值",stats.mean?QJsonValue(*stats.mean):QJsonValue()},{"单位",unit}};
        reports.append(std::move(doc));return reports;
    };
}
#endif
#if defined(MANUAL_SURFACE)
void AttachReportSource(TestWorkflow& workflow,std::shared_ptr<SurfaceDeterminationHostFeature> feature)
{
    workflow.readReports["Surface"]=[feature](const QJsonObject& record,const QSet<QString>& known) {
        QVector<ReportDocument> reports;const auto r=record["result"].toObject();const auto generation=r["generation"].toString();
        if(!HasRevision(generation) || known.contains("surface:"+generation))return reports;
        const auto snapshot=feature->GetSurfaceSnapshot(GetRef(generation));if(!snapshot || snapshot->purpose!=SurfaceTaskPurpose::Determine)return reports;
        auto doc=Document(record,"surface",generation,GetRefText(snapshot->sourceRevision),"表面对象明细");doc.result=GetRefText(snapshot->meshRevision);
        doc.identityResult=generation;
        doc.relatedResults.append(generation);
        doc.description="一行一个实际表面对象；面积和体积资格分别保存，缺失量值留空。";const auto unit=QString::fromStdString(snapshot->modelUnit);
        doc.columns={{"object_id",{},{},false},{"area_"+unit+"2","表面对象面积",unit+"2",true},{"surface_object_volume_"+unit+"3","表面对象体积",unit+"3",true},
            {"area_validity",{},{},false},{"volume_validity",{},{},false},{"closed",{},{},false},{"manifold",{},{},false}};
        if(!snapshot->objects)throw std::runtime_error("表面正式对象目录不可读");
        const auto objects=snapshot->objects;doc.identityColumn=0;
        doc.table=std::make_shared<ReportTable>(objects->size(),[objects](ReportRowId row){
            const auto& object=objects->at(static_cast<std::size_t>(row));
            return QStringList{QString::number(object.objectIndex),
                // 限定面积仍是正式原生量值，资格单独保留。
                object.areaModelUnit2?Number(*object.areaModelUnit2):QString(),
                object.volumeValidity==SurfaceMetricValidity::Valid&&object.volumeModelUnit3?Number(*object.volumeModelUnit3):QString(),
                QString::number(static_cast<int>(object.areaValidity)),QString::number(static_cast<int>(object.volumeValidity)),object.isClosed?"true":"false",object.isManifold?"true":"false"};
        });
        doc.conditions["正式计算条件"]=QJsonDocument::fromJson(QByteArray::fromStdString(snapshot->canonicalParameters)).object();
        doc.conditions["对象总数"]=QString::number(doc.table->GetCount());
        reports.append(std::move(doc));
        if(snapshot->isoEstimate) {
            auto params=Document(record,"surface-parameters",generation,GetRefText(snapshot->sourceRevision),"表面 BG / M / ISO");params.result=GetRefText(snapshot->meshRevision);
            params.relatedResults.append(generation);
            params.columns={{"parameter",{},{},false},{"value","表面灰度参数","1",true}};params.identityColumn=0;params.identityStable=true;params.table=std::make_shared<ReportTable>();
            const auto& estimate=*snapshot->isoEstimate;
            params.table->Append({"ISO",Number(estimate.isoValue)});
            if(estimate.sampleCount){params.table->Append({"BG",Number(estimate.backgroundValue)});params.table->Append({"M",Number(estimate.materialValue)});}
            params.description="自动估计有样本支持时才报告 BG/M；提供范围不冒充实测均值。";reports.append(std::move(params));
        }
        return reports;
    };
}
#endif
#if defined(MANUAL_ALIGNMENT)
void AttachReportSource(TestWorkflow& workflow,std::shared_ptr<MetrologyAlignmentHostFeature> feature)
{
    workflow.readReports["Alignment"]=[feature](const QJsonObject& record,const QSet<QString>& known) {
        QVector<ReportDocument> reports;const auto r=record["result"].toObject();const auto inspection=r["inspection"].toString(),deviation=r["deviation"].toString(),volume=r["voxelVolume"].toString();
        const auto length=[](AlignmentUnit unit){return unit==AlignmentUnit::Millimeter?QString("mm"):unit==AlignmentUnit::Meter?QString("m"):QString("model");};
        if(HasRevision(inspection) && !known.contains("inspection:"+inspection)) {
            const auto snapshot=feature->GetInspection(GetRef(inspection));
            if(snapshot && snapshot->report && snapshot->plan) {
                auto source=GetRefText(snapshot->sourceMesh);
                if(const auto alignment=feature->GetResult(snapshot->alignmentResult))source=GetRefText(alignment->input.source);
                reports.append(CreateInspectionReport(record,*snapshot,source));
            }
        }
        if(HasRevision(deviation) && !known.contains("deviation:"+deviation)) {
            const auto snapshot=feature->GetDeviation(GetRef(deviation));
            if(snapshot && snapshot->forward) {
                const auto fieldReport=[&](std::shared_ptr<const AlignmentDeviationField> samples,const QString& prefix,const QString& title,const QString& quantity) {
                const auto& field=*samples;
                auto doc=Document(record,prefix,deviation,GetRefText(snapshot->sourceMesh),title);
                doc.relatedResults.append(GetRefText(snapshot->alignmentResult));
                if(const auto alignment=feature->GetResult(snapshot->alignmentResult))doc.source=GetRefText(alignment->input.source);
                const auto unit=length(snapshot->unit);
                doc.columns={{"sample_id",{},{},false},{"value_"+unit,quantity,unit,true},{"distance_"+unit,"最近距离",unit,true},{"validity",{},{},false},{"reason",{},{},false}};
                doc.identityColumn=0;doc.validityColumn=3;doc.table=std::make_shared<ReportTable>();
                const QStringList reasons{"未选择","有效","源无效","目标无效","超过距离","法向不匹配","缺少法向"};
                doc.table->SetRows(field.samples.size(),[samples,reasons](ReportRowId index){const auto& sample=samples->samples[index];const bool valid=sample.state==AlignmentDeviationState::Valid;
                    return QStringList{QString::number(index),valid?Number(sample.value):QString(),valid?Number(sample.distance):QString(),valid?"0":QString::number(static_cast<int>(sample.state)+1),reasons.value(static_cast<int>(sample.state),"未知状态")};});
                const auto& stats=field.statistics;
                doc.conditions["汇总"]=QJsonObject{{"样本数",QString::number(field.samples.size())},{"有效数",QString::number(stats.counts[static_cast<std::size_t>(AlignmentDeviationState::Valid)])},
                    {"最小值",stats.hasValues?QJsonValue(stats.minimum):QJsonValue()},{"最大值",stats.hasValues?QJsonValue(stats.maximum):QJsonValue()},
                    {"平均值",stats.hasValues?QJsonValue(stats.mean):QJsonValue()},{"RMS",stats.hasValues?QJsonValue(stats.rms):QJsonValue()},{"单位",unit}};
                return doc;
                };
                const auto quantity=snapshot->parameters && snapshot->parameters->metric==AlignmentDeviationMetric::NominalNormalProjection?QString("名义法向投影偏差"):QString("无符号最近距离");
                reports.append(fieldReport(snapshot->forward,"deviation","实测到名义偏差明细",quantity));
                if(snapshot->reverse)reports.append(fieldReport(snapshot->reverse,"deviation-reverse","名义到实测覆盖明细","名义到实测无符号覆盖距离"));
            }
        }
        if(HasRevision(volume) && !known.contains("voxel-volume:"+volume)) {
            if(const auto snapshot=feature->GetVoxelVolume(GetRef(volume))) {
                auto doc=Document(record,"voxel-volume",volume,GetRefText(snapshot->source),"选区单元体积");const auto& stats=snapshot->statistics;
                doc.description="完整体积与有效选中部分体积含义不同；不可观测或完整性条件未满足时，完整体积留空。";
                doc.columns={{"item_id",{},{},false},{"selected_cells","选中单元数","1",true},{"selected_cell_volume_mm3","选区单元体积和","mm3",true},
                    {"observed_volume_mm3","有效选中部分体积","mm3",true},{"selection_completeness",{},{},false},{"reason",{},{},false}};
                doc.valueColumn=2;doc.identityColumn=0;doc.table=std::make_shared<ReportTable>();
                const QStringList reasons{"完整","存在不可观测单元","接触索引区域边界","空选区"};
                doc.table->Append({"voxel-volume",QString::number(stats.selected),stats.volumeMm3?Number(*stats.volumeMm3):QString(),Number(stats.observedVolumeMm3),
                    QString::number(static_cast<int>(stats.validity)),reasons.value(static_cast<int>(stats.validity),"未知状态")});
                reports.append(std::move(doc));
            }
        }
        return reports;
    };
}
#endif
}
