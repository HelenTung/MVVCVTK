#pragma once
#include "AlignmentGeometry.h"
#include "Host/TrustedDataPort.h"

namespace AlignmentData {
inline const DataTypeId recipeType{"org.mvvcvtk.metrology-alignment.recipe", 1};
inline const DataTypeId geometryType{"org.mvvcvtk.metrology-alignment.geometry", 1};
inline const DataTypeId resultType{"org.mvvcvtk.metrology-alignment.result", 1};

class RecipePayload final : public IDataPayload {
  public:
    explicit RecipePayload(AlignmentRecipe value) : recipe(std::move(value)) {}
    DataTypeId GetDataType() const override {
        return recipeType;
    }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override {
        return std::make_shared<const RecipePayload>(recipe);
    }
    const AlignmentRecipe recipe;
};
class GeometryPayload final : public IDataPayload {
  public:
    explicit GeometryPayload(std::vector<AlignmentGeometry> value) : geometries(std::move(value)) {}
    DataTypeId GetDataType() const override {
        return geometryType;
    }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override {
        return std::make_shared<const GeometryPayload>(geometries);
    }
    const std::vector<AlignmentGeometry> geometries;
};
struct ResultRecord final {
    AlignmentInput input;
    DataRevisionRef recipe, transform, geometry, residuals;
    AlignmentDiagnostics diagnostics;
    std::vector<AlignmentMatrix> initialPoses;
    std::vector<DataExpectation> expectations;
};
class ResultPayload final : public IDataPayload {
  public:
    explicit ResultPayload(ResultRecord value) : record(std::move(value)) {}
    DataTypeId GetDataType() const override {
        return resultType;
    }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override {
        return std::make_shared<const ResultPayload>(record);
    }
    const ResultRecord record;
};
bool SetTypes(TrustedDataPort &data);
bool GetInputValid(const AlignmentInput &input);
bool GetExpectationsCurrent(const DataGraphSnapshot &graph,
                            const std::vector<DataExpectation> &expectations);
std::vector<DataExpectation> BuildExpectations(const DataGraphSnapshot &graph,
                                               const AlignmentInput &input,
                                               const DataRevisionRef &recipe,
                                               const DataRevisionRef &nominal);
std::vector<DataInputRef> BuildInputs(const AlignmentInput &input, const DataRevisionRef &recipe,
                                      const DataRevisionRef &nominal);
std::string GetBindingName(const std::string &scope);
DataBinding GetBinding(const DataGraphSnapshot &graph, const std::string &scope);
DataTransaction BuildTransaction(TrustedDataPort &data, const AlignmentWork &work,
                                 const DataRevisionRef &recipe, const AlignmentCandidate &candidate,
                                 const std::vector<DataExpectation> &expectations,
                                 const DataBinding &active, bool isActivationRequested,
                                 DataRevisionRef &resultRef, DataRevisionRef &transformRef);
std::shared_ptr<const ResultPayload> GetResult(const DataGraphSnapshot &graph,
                                               const DataRevisionRef &ref);
} // namespace AlignmentData
