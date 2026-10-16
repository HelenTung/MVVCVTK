// 仓内可信输入适配器把不可变数据投影成有界统计样本，不扩大 Host 或 Gap SDK。
#include "ReferenceDataSource.h"
#include "Data/DataPayloads.h"
#include "App/Services/FeatureViewService.h"
#include "JsonInput.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <type_traits>
namespace Manual {
namespace {
template<class T> double Scalar(const std::uint8_t* bytes){T value;std::memcpy(&value,bytes,sizeof(value));return static_cast<double>(value);}
double Scalar(const std::uint8_t* bytes,ImageValueType type)
{
    switch(type){
    case ImageValueType::Int8:return Scalar<std::int8_t>(bytes);case ImageValueType::UInt8:return Scalar<std::uint8_t>(bytes);
    case ImageValueType::Int16:return Scalar<std::int16_t>(bytes);case ImageValueType::UInt16:return Scalar<std::uint16_t>(bytes);
    case ImageValueType::Int32:return Scalar<std::int32_t>(bytes);case ImageValueType::UInt32:return Scalar<std::uint32_t>(bytes);
    case ImageValueType::Int64:return Scalar<std::int64_t>(bytes);case ImageValueType::UInt64:return Scalar<std::uint64_t>(bytes);
    case ImageValueType::Float32:return Scalar<float>(bytes);case ImageValueType::Float64:return Scalar<double>(bytes);
    default:throw std::invalid_argument("统计不支持该标量类型");}
}
const RecordColumn* Column(const RecordTablePayload& table,const std::string& name)
{for(const auto& column:table.GetColumns())if(column.name==name)return &column;return nullptr;}
}
std::size_t ReferenceDataSource::GetGraySampleStride(std::size_t total,const std::array<int,3>& dimensions)
{
    constexpr std::size_t sampleLimit=262144;
    auto stride=std::max<std::size_t>(1,total/sampleLimit+(total%sampleLimit!=0));
    const auto plane=static_cast<std::size_t>(dimensions[0])*dimensions[1];
    // 步长不能与行/层周期共因子，否则大卷的样本会固定在同一组 X 列。
    while(stride>1 && std::gcd(stride,plane)!=1)++stride;
    return stride;
}
StatisticsSamples ReferenceDataSource::ReadGraySamples(DataRevisionRef source,DataRevisionRef roi,std::size_t offset,GrayPointFilter filter)
{
    if(!m_data)throw std::runtime_error("统计读取器已停止");const auto graph=m_data->GetDataGraph();const auto data=m_data->GetData(graph,source);
    const auto image=data ? std::dynamic_pointer_cast<const ImageGrid3DPayload>(data->payload) : nullptr;
    if(!image || !image->GetValid() || image->GetComponentCount()!=1)throw std::runtime_error("源修订不可读或不是单分量图像");
    const auto& grid=image->GetGeometry();const auto total=GetGridVoxelCount(grid).value();
    // 仅为界面分布作规则抽样；明确标记上限和步长，不把抽样频数解释为体积。
    constexpr std::size_t blockLimit=4096;
    const auto stride=GetGraySampleStride(total,grid.dimensions);
    StatisticsSamples result;result.total=(total+stride-1)/stride;offset=std::min(offset,result.total);result.next=offset+std::min(result.total-offset,filter ? std::size_t(256) : blockLimit);
    result.description="来源 "+GetRefText(source)+QString(" · %1 · x-fast 步长 %2 · %3").arg(stride==1 ? "全量" : "规则抽样").arg(stride).arg(GetDataRevisionRefValid(roi) ? "ROI ∩ 源有效域" : "源有效域");
    RoiReadSnapshot scope;
    if(GetDataRevisionRefValid(roi)){const auto read=m_data->GetRoi(graph,roi,source);if(!read.roi)throw std::runtime_error("ROI 与源修订不匹配");scope=read.roi;result.description+=" · ROI "+GetRefText(roi);}
    const auto bytes=GetImageValueBytes(image->GetValueType());const auto& values=*image->GetValues();const auto mask=image->GetValidityMask();
    std::vector<std::array<double,3>> points;
    for(std::size_t index=offset;index<result.next;++index) {
        const auto voxel=index*stride;
        if(mask && !(*mask)[voxel]){++result.excluded;continue;}
        std::array<double,3> point{};
        if(scope || filter){const auto x=voxel%grid.dimensions[0],y=(voxel/grid.dimensions[0])%grid.dimensions[1],z=voxel/(static_cast<std::size_t>(grid.dimensions[0])*grid.dimensions[1]);
            const std::array<double,3> ijk{double(x)+grid.extent[0],double(y)+grid.extent[2],double(z)+grid.extent[4]};
            point=grid.origin;for(int row=0;row<3;++row)for(int axis=0;axis<3;++axis)point[row]+=grid.direction[row*3+axis]*grid.spacing[axis]*ijk[axis];
            if(scope && !scope->GetContains(point)){++result.excluded;continue;}}
        const auto value=Scalar(values.data()+voxel*bytes,image->GetValueType());const auto noData=image->GetMetadata().scalar.noData;
        if(std::isfinite(value) && (!noData || value!=*noData)){result.values.push_back(value);if(filter)points.push_back(point);}else ++result.excluded;
    }
    if(filter){const auto kept=filter(points);if(kept.size()!=result.values.size())throw std::runtime_error("预览范围分类数量不一致");std::size_t used=0;for(std::size_t i=0;i<kept.size();++i)if(kept[i])result.values[used++]=result.values[i];else ++result.excluded;result.values.resize(used);}
    return result;
}
StatisticsSamples ReferenceDataSource::ReadRecordSamples(DataRevisionRef ref,const std::string& name,std::size_t offset)
{
    if(!m_data)throw std::runtime_error("统计读取器已停止");const auto data=m_data->GetData(m_data->GetDataGraph(),ref);
    const auto table=data ? std::dynamic_pointer_cast<const RecordTablePayload>(data->payload) : nullptr;
    if(!table || !table->GetValid())throw std::runtime_error("结果表修订不可读");const auto* column=Column(*table,name);
    const auto* values=column ? std::get_if<DataArray<double>>(&column->values) : nullptr;if(!values)throw std::runtime_error("结果表缺少所需数值列");
    StatisticsSamples result;result.total=values->size();result.next=std::min(result.total,offset+4096);
    result.description="来源 "+GetRefText(ref)+" · 已发布结果表 · 全量缺陷 · "+QString::fromStdString(name);
    for(std::size_t i=offset;i<result.next;++i)if(std::isfinite((*values)[i]) && (*values)[i]>=0)result.values.push_back((*values)[i]);else ++result.excluded;
    return result;
}
QJsonObject ReferenceDataSource::ReadRecordPage(DataRevisionRef ref,std::size_t offset,std::size_t count,std::optional<std::array<double,2>> volumeRange)
{
    if(!m_data || count>256)throw std::runtime_error("结果表读取不可用或分页过大");const auto data=m_data->GetData(m_data->GetDataGraph(),ref);
    const auto table=data ? std::dynamic_pointer_cast<const RecordTablePayload>(data->payload) : nullptr;
    if(!table || !table->GetValid())throw std::runtime_error("结果表修订不可读");
    QJsonArray rows;auto next=std::min(offset,table->GetRowCount());const auto end=next+std::min<std::size_t>(8192,table->GetRowCount()-next);
    const auto* volumeColumn=Column(*table,"volume-mm3");const auto* volumes=volumeColumn ? std::get_if<DataArray<double>>(&volumeColumn->values) : nullptr;
    for(std::size_t i=next;i<end && rows.size()<static_cast<int>(count);++i) {
        next=i+1;
        if(volumeRange && (!volumes || (*volumes)[i]<(*volumeRange)[0] || (*volumes)[i]>(*volumeRange)[1]))continue;
        QJsonObject row;
        for(const auto* name:{"void-id","volume-mm3","equivalent-diameter-mm","seed-voxel","parent-volume-mm3","intersection-volume-mm3","roi-partial","whole-object-geometry","metric","value","unit"})if(const auto* column=Column(*table,name))
            row[name]=std::visit([i](const auto& values)->QJsonValue{
                using Value=typename std::decay_t<decltype(values)>::value_type;
                if constexpr(std::is_same_v<Value,double>)return std::isfinite(values[i]) ? QJsonValue(values[i]) : QJsonValue();
                else if constexpr(std::is_integral_v<Value>)return QString::number(values[i]);
                else if constexpr(std::is_same_v<Value,std::string>)return QString::fromStdString(values[i]);
                else return GetValues(values[i]);
            },column->values);
        row["rowIndex"]=QString::number(i);rows.append(row);
    }
    QString source;QJsonArray inputs;
    for(const auto& input:data->inputs){inputs.append(QJsonObject{{"role",QString::fromStdString(input.role)},{"revision",GetRefText(input.source)}});if(source.isEmpty() && input.role=="source-volume")source=GetRefText(input.source);}
    return {{"rows",rows},{"offset",QString::number(offset)},{"total",QString::number(table->GetRowCount())},{"next",QString::number(next)},
        {"source",source},{"inputs",inputs},{"canonicalParameters",data->provenance?QString::fromStdString(data->provenance->canonicalParameters):QString()}};
}
std::array<double,3> ReferenceDataSource::GetRecordLocation(DataRevisionRef ref,std::size_t row,DataRevisionRef source)
{
    if(!m_data || !m_views)throw std::runtime_error("定位不可用");const auto graph=m_data->GetDataGraph();
    const auto tableData=m_data->GetData(graph,ref),sourceData=m_data->GetData(graph,source);
    if(!tableData || std::none_of(tableData->inputs.begin(),tableData->inputs.end(),[source](const auto& input){return input.source==source;}))throw std::runtime_error("缺陷表的实际来源与定位输入不匹配");
    const auto table=tableData ? std::dynamic_pointer_cast<const RecordTablePayload>(tableData->payload) : nullptr;
    const auto image=sourceData ? std::dynamic_pointer_cast<const ImageGrid3DPayload>(sourceData->payload) : nullptr;
    const auto* column=table ? Column(*table,"seed-voxel") : nullptr;
    const auto* seeds=column ? std::get_if<DataArray<std::array<std::int64_t,3>>>(&column->values) : nullptr;
    if(!seeds || row>=seeds->size() || !image)throw std::runtime_error("缺陷定位种子不可读");
    const auto& grid=image->GetGeometry();auto point=grid.origin;
    for(int axis=0;axis<3;++axis)if((*seeds)[row][axis]<grid.extent[2*axis] || (*seeds)[row][axis]>grid.extent[2*axis+1])throw std::runtime_error("缺陷种子越界");
    for(int k=0;k<3;++k)for(int axis=0;axis<3;++axis)point[k]+=grid.direction[k*3+axis]*grid.spacing[axis]*(*seeds)[row][axis];
    return GetModelWorldPosition(source,point);
}
std::array<double,3> ReferenceDataSource::GetModelWorldPosition(DataRevisionRef source,const std::array<double,3>& point)
{
    if(!m_data||!m_views)throw std::runtime_error("定位不可用");
    const auto binding=m_data->GetDataBinding(m_data->GetDataGraph(),primaryVolumeBinding);
    if(!binding||!binding->target||*binding->target!=source)throw std::runtime_error("结果来源与当前输入不同，请先显式采用对应体数据");
    const auto port=m_views->GetFeaturePort("primary-3d");const auto matrix=port ? port->GetModelToWorld() : std::optional<std::array<double,16>>{};
    if(!matrix)throw std::runtime_error("当前模型姿态不可读");
    std::array<double,3> world{};for(int k=0;k<3;++k){world[k]=(*matrix)[4*k+3];for(int axis=0;axis<3;++axis)world[k]+=(*matrix)[4*k+axis]*point[axis];}return world;
}
}
