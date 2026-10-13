#pragma once
#include "GapAnalysisTypes.h"
#include "Data/DataPayloads.h"
#include <cmath>
inline const DataTypeId gapParentRecordType{"org.mvvcvtk.gap-analysis.parent-record",1};
inline const DataFacetId gapParentRecordFacet{"gap-parent-detection"};
// 私有持久父记录只包含本产品的值和共享不可变载荷，不保存 VTK 或供应商 identity。
class GapParentRecord final:public IDataPayload {
public:
    DataRevisionRef source;
    GapSurfaceParams parameters;
    GapStatistics statistics;
    std::shared_ptr<const LabelMap3DPayload> labels;
    std::shared_ptr<const std::vector<VoidRegion>> voids;
    DataTypeId GetDataType()const override{return gapParentRecordType;}
    std::shared_ptr<const IDataPayload> CreateSnapshot()const override{return std::make_shared<const GapParentRecord>(*this);}
    std::vector<std::shared_ptr<const void>> GetDataResources()const override{std::vector<std::shared_ptr<const void>> result{labels,voids};if(labels){const auto resources=labels->GetDataResources();result.insert(result.end(),resources.begin(),resources.end());}return result;}
    bool GetValid()const{return GetDataRevisionRefValid(source) && labels && labels->GetValid() && voids && std::isfinite(parameters.isoValue) && std::isfinite(parameters.background) && std::isfinite(parameters.material) && std::isfinite(statistics.objectVolumeMM3) && statistics.objectVolumeMM3>=0 && std::isfinite(statistics.voidVolumeMM3) && statistics.voidVolumeMM3>=0;}
};
