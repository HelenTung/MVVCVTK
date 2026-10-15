// 发布不变量的舍入反例；性能模式只测残差事务组织，不代表真实 CT 端到端性能。
#include "../TestDataPort.h"
#include "AlignmentData.h"
#include "AlignmentInspectionData.h"
#include "AlignmentMath.h"
#include <Windows.h>
#include <psapi.h>
#include <chrono>
#include <iostream>

namespace {
class SchemaPort final : public TestDataPort {
public:
    std::vector<DataTypeDescriptor> types;
    bool SetDataType(DataTypeDescriptor descriptor) override { types.push_back(std::move(descriptor)); return true; }
    bool Validate(const IDataPayload &payload) const {
        for (const auto &type : types) if (type.id == payload.GetDataType()) {
            std::string message; return type.validate(payload, message);
        }
        return false;
    }
};
void Require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void RoundingPublication() {
    AlignmentWork work; work.recipe.lengthScale = 1;
    AlignmentGeometrySpec spec; spec.id = "plane"; spec.kind = AlignmentGeometryKind::Plane; spec.maxFitRms = 1;
    AlignmentSamples samples;
    for (double x : {-2.0, 0.0, 2.0}) for (double y : {-1.0, 1.0}) for (double z : {-.1, .1})
        samples.points.push_back({x,y,z});
    auto geometry = AlignmentGeometryFit::BuildGeometry(work, spec, samples, alignmentIdentity);
    Require(std::abs(geometry.fitRms-.1) < 1e-12 && geometry.fitRms <= geometry.fitMax,
            "Symmetric plane RMS must equal its analytic 0.1 residual and remain below the maximum.");
    SchemaPort data; Require(AlignmentData::SetTypes(data) && AlignmentInspectionData::SetType(data), "Schemas must register.");
    geometry.fitRms = std::nextafter(geometry.fitMax, std::numeric_limits<double>::infinity());
    Require(data.Validate(AlignmentData::GeometryPayload({geometry})), "One rounding unit cannot reject geometry publication.");
    InspectionPlan plan; plan.id = "rounding"; plan.alignmentResult = GetTestDataRef(1);
    InspectionGeometrySpec fit; fit.fit = spec; fit.fit.region = {GetTestDataRef(2),{0,1,2},{}};
    plan.geometries.push_back(fit);
    InspectionItem item; item.id = "rms"; item.name = "rms"; item.measured.first.geometry = 0;
    item.measured.metric = InspectionMetric::FitRms; plan.items.push_back(item);
    auto report = std::make_shared<InspectionReport>(); report->algorithmVersion = "metrology-inspection-1";
    report->evaluatedUnixMilliseconds = 1;
    InspectionGeometry fitted; fitted.fitted = geometry; fitted.status = AlignmentStatus::FullyDetermined;
    fitted.alignedDirection = {0,0,1}; report->geometries.push_back(fitted);
    InspectionItemResult row; row.id = "rms"; row.measured = geometry.fitRms;
    row.verdict = AlignmentInspectionVerdict::NotEvaluated; report->items.push_back(row);
    InspectionSnapshot snapshot; snapshot.result = GetTestDataRef(3); snapshot.sourceMesh = GetTestDataRef(2);
    snapshot.referenceData = GetTestDataRef(4); snapshot.transform = GetTestDataRef(5);
    snapshot.alignmentResult = plan.alignmentResult; snapshot.plan = std::make_shared<const InspectionPlan>(plan);
    snapshot.report = report;
    Require(data.Validate(AlignmentInspectionData::Payload(snapshot,{})), "One rounding unit cannot reject inspection publication.");
    geometry.fitRms = geometry.fitMax + .001;
    Require(!data.Validate(AlignmentData::GeometryPayload({geometry})), "A material RMS/max violation must still reject publication.");
    report->geometries[0].fitted = geometry;
    Require(!data.Validate(AlignmentInspectionData::Payload(snapshot,{})), "Inspection must reject a material RMS/max violation.");
}
void Benchmark(std::size_t count, std::size_t repeats) {
    Require(count > 0 && repeats > 0, "Benchmark requires nonzero row and repeat counts.");
    TestDataPort data; AlignmentWork work; AlignmentCandidate candidate;
    candidate.residuals.reserve(count);
    for (std::size_t i=0; i<count; ++i) candidate.residuals.push_back({"surface-"+std::to_string(i),
        AlignmentConstraintKind::Soft,63,static_cast<double>(i%17),1,true});
    for (std::size_t iteration=0; iteration<repeats; ++iteration) {
        DataRevisionRef result, transform;
        const auto start = std::chrono::steady_clock::now();
        auto tx = AlignmentData::BuildTransaction(data,work,{},candidate,{}, {},false,result,transform);
        const auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        const auto table = std::dynamic_pointer_cast<const RecordTablePayload>(tx.outputs[2].payload);
        Require(table && table->GetValid() && table->GetRowCount()==count && table->GetColumns().size()==6,
                "Published residual table must retain all six columns and all rows.");
        Require(std::get<DataArray<double>>(table->GetColumns()[1].values).back()==static_cast<double>((count-1)%17),
                "Published last residual must retain its value.");
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
        Require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)),
                "Process memory counters must be available.");
        std::cout << "rows=" << count << " iteration=" << iteration << " transactionMs=" << elapsed
                  << " privateBytes=" << memory.PrivateUsage << " peakPrivateBytes=" << memory.PeakPagefileUsage << '\n';
    }
}
}
int main(int argc, char **argv) {
    try {
        if (argc==4 && std::string(argv[1])=="--benchmark") Benchmark(std::stoull(argv[2]),std::stoull(argv[3]));
        else RoundingPublication();
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
