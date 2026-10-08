#include "Host/MetrologyReport.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>

namespace {
int failures=0;
void Check(bool value,const char *message) { if(!value) { ++failures;std::cerr<<"FAILED: "<<message<<'\n'; } }
DataRevisionRef Ref(unsigned char id) { DataRevisionRef ref;ref.entityId.bytes[0]=id;ref.generation=1;return ref; }
MetrologyReport Fixture() {
    MetrologyReport r;r.id="=part,1";r.title=u8"报告 \"A\",\n二";r.capturedCommit=10;r.capturedUnixMilliseconds=1000;
    MetrologyReportAlignment a;a.result=Ref(6);a.recipeRevision=Ref(8);a.transform=Ref(7);a.algorithmVersion="metrology-alignment-3";
    a.input={Ref(2),Ref(3),Ref(4),"scan","RAS","part",AlignmentUnit::Millimeter};a.sourceToTarget[3]=1.5;
    auto recipe=std::make_shared<AlignmentRecipe>();recipe->id="recipe";recipe->targetFrameId="drawing";recipe->nominalData=Ref(5);recipe->unit=AlignmentUnit::Millimeter;
    a.recipe=recipe;a.geometries=std::make_shared<const std::vector<AlignmentGeometry>>();
    a.initialPoses=std::make_shared<const std::vector<AlignmentMatrix>>(1,alignmentIdentity);
    auto diagnostics=std::make_shared<AlignmentDiagnostics>();diagnostics->status=AlignmentStatus::FullyDetermined;
    diagnostics->isQualityPassed=true;a.diagnostics=diagnostics;
    a.residuals=std::make_shared<const RecordTablePayload>(DataTypes::recordTable,"test-residuals",
        std::vector<RecordColumn>{{"value",std::vector<double>{.25}},{"index",std::vector<std::uint64_t>{18446744073709551615ULL}}});
    a.wasCurrentAtCapture=false;r.alignments.push_back(a);r.wereInputsCurrentAtCapture=false;
    auto under=a;under.result=Ref(11);under.transform=Ref(12);under.recipeRevision=Ref(13);
    auto underDiagnostics=std::make_shared<AlignmentDiagnostics>();underDiagnostics->status=AlignmentStatus::Underconstrained;
    underDiagnostics->condition=std::numeric_limits<double>::infinity();under.diagnostics=underDiagnostics;r.alignments.push_back(under);
    InspectionSnapshot inspection;inspection.result=Ref(9);inspection.alignmentResult=a.result;inspection.transform=a.transform;
    inspection.sourceMesh=a.input.mesh;inspection.referenceData=recipe->nominalData;inspection.unit=a.input.unit;inspection.sourceToTarget=a.sourceToTarget;
    auto plan=std::make_shared<InspectionPlan>();plan->id="plan";plan->alignmentResult=a.result;
    auto result=std::make_shared<InspectionReport>();result->evaluatedUnixMilliseconds=900;
    for(int i=0;i<2;++i) {
        InspectionGeometrySpec spec;spec.fit.id="point-"+std::to_string(i);spec.fit.kind=AlignmentGeometryKind::Point;
        plan->geometries.push_back(spec);InspectionGeometry g;g.fitted.id=spec.fit.id;g.fitted.kind=spec.fit.kind;
        g.status=i ? AlignmentStatus::Degenerate : AlignmentStatus::FullyDetermined;result->geometries.push_back(g);
        InspectionItem item;item.id="dimension-"+std::to_string(i);item.name=i ? "missing" : u8"位置,\"X\"\n下一行";
        item.measured.metric=i ? InspectionMetric::CenterY : InspectionMetric::CenterX;item.measured.first.geometry=i;
        item.nominal=i ? 3 : 1;item.nominalSource="fixture";item.deviationLimits=std::array<double,2>{-.1,.1};plan->items.push_back(item);
        InspectionItemResult row;row.id=item.id;row.nominal=item.nominal;
        if(!i) { row.measured=1.5;row.deviation=.5;row.verdict=AlignmentInspectionVerdict::Fail; }
        else { row.message=u8"无有效拟合";row.verdict=AlignmentInspectionVerdict::Indeterminate; }
        result->items.push_back(row);
    }
    result->verdict=AlignmentInspectionVerdict::Fail;inspection.plan=plan;inspection.report=result;r.inspections.push_back(inspection);
    AlignmentDeviationSnapshot deviation;deviation.result=Ref(10);deviation.alignmentResult=a.result;deviation.transform=a.transform;
    deviation.sourceMesh=a.input.mesh;deviation.referenceMesh=recipe->nominalData;deviation.unit=a.input.unit;deviation.sourceToTarget=a.sourceToTarget;
    auto params=std::make_shared<AlignmentDeviationRequest>();params->alignmentResult=a.result;params->tolerance=std::array<double,2>{0,.2};params->reverseCoverage=false;
    deviation.parameters=params;auto field=std::make_shared<AlignmentDeviationField>();field->samples.resize(3);
    field->samples[0]={.1,.1,2,7,AlignmentDeviationState::Valid};field->samples[1].state=AlignmentDeviationState::InvalidSource;field->samples[1].area=1;
    auto &s=field->statistics;s.counts={1,1,1,0,0,0,0};s.areas={0,2,1,0,0,0,0};s.selectedArea=3;s.evaluatedArea=2;
    s.minimum=s.maximum=s.mean=s.rms=s.absoluteP95=.1;s.hasValues=true;
    deviation.forward=field;deviation.verdict=AlignmentInspectionVerdict::Indeterminate;r.deviations.push_back(deviation);
    MetrologyReportInput input;input.revision=Ref(2);input.type=DataTypes::imageGrid3D;input.image=MetrologyReportImage{};
    input.image->geometry.dimensions={2,2,2};input.image->geometry.extent={0,1,0,1,0,1};input.image->geometry.spacing={.1375,.1375,.1375};
    input.image->metadata.source.uri=u8"F:\\数据\\测试.raw";input.image->metadata.scalar.noData=std::numeric_limits<double>::quiet_NaN();
    input.image->valueType=ImageValueType::Float32;input.image->valueBytes=32;input.image->components=1;r.inputs.push_back(input);return r;
}
class Counter final : public std::streambuf {
public:
    std::size_t count=0,largest=0;bool shortWrite=false,throws=false,overreports=false;
    std::streamsize xsputn(const char *,std::streamsize n) override {
        if(throws)throw std::runtime_error("sink fault");largest=std::max(largest,static_cast<std::size_t>(n));
        const auto accepted=shortWrite ? std::min<std::streamsize>(5,n) : n;count+=static_cast<std::size_t>(accepted);return overreports ? n+1 : accepted;
    }
};
class CommaLocale final : public std::numpunct<char> { char do_decimal_point() const override { return ','; } };
}
int main(int argc,char **argv) {
    try {
        auto report=Fixture();MetrologyReportWriteOptions options;std::ostringstream json;
        json.imbue(std::locale(std::locale::classic(),new CommaLocale));json.precision(3);const auto flags=json.flags();
        auto result=WriteMetrologyReport(report,json,options);
        Check(result.status==MetrologyReportWriteStatus::Succeeded && result.bytesWritten==json.str().size(),"JSON bytes confirmed");
        Check(json.str().find("\"measured\":1.5")!=std::string::npos && json.precision()==3 && json.flags()==flags,"JSON is locale independent and preserves stream formatting");
        Check(json.str().find("PositiveInfinity")!=std::string::npos && json.str().find("\"samplesIncluded\":false")!=std::string::npos,"nonfinite diagnostics and omitted samples are explicit");
        options.maxBytes=json.str().size();std::ostringstream exact;
        Check(WriteMetrologyReport(report,exact,options).status==MetrologyReportWriteStatus::Succeeded,"exact output budget succeeds");
        --*options.maxBytes;std::ostringstream limited;
        result=WriteMetrologyReport(report,limited,options);
        Check(result.status==MetrologyReportWriteStatus::BudgetExceeded && result.bytesWritten<=*options.maxBytes,"output budget never emits beyond cap");
        options={};options.isCancelled=[] { return true; };std::ostringstream cancelled;
        Check(WriteMetrologyReport(report,cancelled,options).status==MetrologyReportWriteStatus::Cancelled && cancelled.str().empty(),"cancel before output");
        options={};Counter shortSink;shortSink.shortWrite=true;std::ostream shortStream(&shortSink);
        result=WriteMetrologyReport(report,shortStream,options);
        Check(result.status==MetrologyReportWriteStatus::IoError && result.bytesWritten==5,"short write has accurate confirmed count");
        Counter throwing;throwing.throws=true;std::ostream badStream(&throwing);
        Check(WriteMetrologyReport(report,badStream,options).status==MetrologyReportWriteStatus::IoError,"streambuf exception is IO failure");
        Counter lying;lying.overreports=true;std::ostream lyingStream(&lying);
        result=WriteMetrologyReport(report,lyingStream,options);
        Check(result.status==MetrologyReportWriteStatus::IoError && result.bytesWritten==0,"invalid stream byte count is not accepted as confirmed output");
        auto invalid=report;invalid.title=std::string("\xC0\xAF",2);std::ostringstream malformed;
        Check(WriteMetrologyReport(invalid,malformed,options).status==MetrologyReportWriteStatus::InvalidInput,"overlong UTF8 rejected");
        invalid=report;invalid.title=std::string(40000,'x')+std::string("\xC0\xAF",2);Counter prefix;std::ostream prefixStream(&prefix);
        result=WriteMetrologyReport(invalid,prefixStream,options);
        Check(result.status==MetrologyReportWriteStatus::InvalidInput && result.bytesWritten==prefix.count && prefix.count>=16384,
            "late text failure reports the emitted prefix; caller must discard it");
        invalid=report;invalid.inspections[0].sourceToTarget[3]=99;std::ostringstream mismatched;
        Check(WriteMetrologyReport(invalid,mismatched,options).status==MetrologyReportWriteStatus::InvalidInput,"mismatched row alignment rejected");
        options.includeDeviationSamples=true;std::ostringstream full;
        Check(WriteMetrologyReport(report,full,options).status==MetrologyReportWriteStatus::Succeeded,"full sample JSON writes");
        options.format=MetrologyReportFormat::Csv;std::ostringstream csv;
        Check(WriteMetrologyReport(report,csv,options).status==MetrologyReportWriteStatus::Succeeded && csv.str().find("'="+std::string("part,1"))!=std::string::npos,"CSV formula-like text protected");
        invalid=report;invalid.id=" =1+1";std::ostringstream spaced;
        Check(WriteMetrologyReport(invalid,spaced,options).status==MetrologyReportWriteStatus::Succeeded && spaced.str().find("' =1+1")!=std::string::npos,"leading-space formula-like text protected");
        if(argc==2) {
            const std::filesystem::path folder(argv[1]);std::filesystem::create_directories(folder);
            std::ofstream a(folder/"fixture.json",std::ios::binary),b(folder/"fixture.csv",std::ios::binary);
            a<<full.str();b<<csv.str();a.close();b.close();Check(bool(a)&&bool(b),"parser fixtures written");
        }
        auto large=std::make_shared<AlignmentDeviationField>();large->samples.resize(100000);large->statistics.counts[0]=large->samples.size();
        report.deviations[0].forward=large;options={};std::ostringstream noValues;
        Check(WriteMetrologyReport(report,noValues,options).status==MetrologyReportWriteStatus::Succeeded &&
            noValues.str().find("\"minimum\":null")!=std::string::npos,"unavailable statistics are null, never fabricated zero");
        Counter counter;std::ostream largeStream(&counter);options.includeDeviationSamples=true;
        result=WriteMetrologyReport(report,largeStream,options);
        Check(result.status==MetrologyReportWriteStatus::Succeeded && counter.count>1000000 && counter.largest<=16384,"dense field streamed in bounded chunks");
        Counter midway;std::ostream middleStream(&midway);options.isCancelled=[&] { return midway.count>=32768; };
        result=WriteMetrologyReport(report,middleStream,options);
        Check(result.status==MetrologyReportWriteStatus::Cancelled && result.bytesWritten==midway.count && midway.count>=32768,"dense export cancels after a bounded prefix");
        std::cout<<"DenseBytes="<<counter.count<<" LargestChunk="<<counter.largest<<'\n';
    } catch(const std::exception &e) { ++failures;std::cerr<<e.what()<<'\n'; }
    return failures ? 1 : 0;
}
