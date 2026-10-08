#pragma once
#include "AlignmentDeviation.h"
#include "Host/TrustedDataPort.h"

namespace AlignmentDeviationDataStore {
inline const DataTypeId resultType{"org.mvvcvtk.metrology-alignment.deviation", 1};
struct Context final {
    AlignmentWork work;
    AlignmentDeviationSnapshot snapshot;
    std::vector<DataExpectation> expectations;
};
class Payload final : public IDataPayload {
  public:
    explicit Payload(AlignmentDeviationSnapshot value, std::vector<DataExpectation> dependencies)
        : snapshot(std::move(value)), expectations(std::move(dependencies)) {}
    DataTypeId GetDataType() const override { return resultType; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override { return std::make_shared<const Payload>(*this); }
    std::vector<std::shared_ptr<const void>> GetDataResources() const override {
        if (snapshot.reverse) return {snapshot.forward, snapshot.reverse};
        return {snapshot.forward};
    }
    const AlignmentDeviationSnapshot snapshot;
    const std::vector<DataExpectation> expectations;
};
bool SetType(TrustedDataPort &data);
std::optional<Context> Prepare(const DataGraphSnapshot &graph, const AlignmentDeviationRequest &request,
                              const AlignmentConfig &config);
DataTransaction BuildTransaction(TrustedDataPort &data, Context &context, const AlignmentDeviationData &result,
                                 DataRevisionRef &resultRef);
std::optional<AlignmentDeviationSnapshot> GetResult(const DataGraphSnapshot &graph, const DataRevisionRef &ref);
} // namespace AlignmentDeviationDataStore
