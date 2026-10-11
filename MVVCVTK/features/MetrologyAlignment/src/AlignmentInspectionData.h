#pragma once
#include "AlignmentInspection.h"
#include "Host/TrustedDataPort.h"

namespace AlignmentInspectionData {
inline const DataTypeId resultType{"org.mvvcvtk.metrology-alignment.inspection",1};
struct Context final {
    AlignmentWork work;
    InspectionSnapshot snapshot;
    std::vector<DataExpectation> expectations;
};
class Payload final : public IDataPayload {
  public:
    Payload(InspectionSnapshot value,std::vector<DataExpectation> dependencies)
        : snapshot(std::move(value)),expectations(std::move(dependencies)) {}
    DataTypeId GetDataType() const override { return resultType; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override { return std::make_shared<const Payload>(*this); }
    std::vector<std::shared_ptr<const void>> GetDataResources() const override {
        std::vector<std::shared_ptr<const void>> resources{snapshot.plan,snapshot.report};
        if (snapshot.report) for (const auto &geometry:snapshot.report->geometries)
            if (geometry.section) resources.push_back(geometry.section);
        return resources;
    }
    const InspectionSnapshot snapshot;
    const std::vector<DataExpectation> expectations;
};
bool SetType(TrustedDataPort &data);
std::optional<Context> Prepare(const DataGraphSnapshot &graph,InspectionPlan plan,const AlignmentConfig &config);
DataTransaction BuildTransaction(TrustedDataPort &data,Context &context,const InspectionData &result,DataRevisionRef &resultRef);
std::optional<InspectionSnapshot> GetResult(const DataGraphSnapshot &graph,const DataRevisionRef &ref);
} // namespace AlignmentInspectionData
