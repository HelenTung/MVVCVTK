#include "Host/MetrologyReport.h"
#include "AlignmentDeviationColors.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <chrono>
#include <cmath>
#include <atomic>
#include <thread>

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
    input.image->valueType=ImageValueType::Float32;input.image->valueBytes=32;input.image->components=1;r.inputs.push_back(input);
    input.revision=Ref(21); input.image->geometry.spacing={1,1,1}; r.inputs.push_back(input);
    VoxelVolumeSnapshot v; v.result=Ref(20); v.source=Ref(21); v.geometry=input.image->geometry;
    auto vp=std::make_shared<VoxelVolumeRequest>(); vp->id="volume-range"; vp->source=v.source; vp->storedValueRange=std::array<double,2>{.25,.75};
    v.parameters=vp; v.statistics.evaluatedExtent=v.geometry.extent; v.statistics.total=8; v.statistics.selected=3;
    v.statistics.unselected=5; v.statistics.cellVolumeMm3=1; v.statistics.observedVolumeMm3=3; v.statistics.volumeMm3=3;
    v.statistics.touchesRegionBoundary=true; v.statistics.validity=VoxelVolumeValidity::Complete; r.voxelVolumes.push_back(v);
    MetrologyReportInput label;label.revision=Ref(22);label.type=DataTypes::labelMap3D;r.inputs.push_back(label);
    v.result=Ref(23);v.source=label.revision;vp=std::make_shared<VoxelVolumeRequest>();vp->source=v.source;vp->id="volume-labels";
    vp->method=VoxelVolumeMethod::Labels;vp->labels={9007199254740993ULL,UINT64_MAX};vp->validityImage=Ref(21);v.parameters=vp;
    v.statistics.unselected=4;v.statistics.masked=1;v.statistics.volumeMm3.reset();v.statistics.validity=VoxelVolumeValidity::IncompleteSamples;r.voxelVolumes.push_back(v);
    v=r.voxelVolumes.front();v.result=Ref(24);vp=std::make_shared<VoxelVolumeRequest>(*v.parameters);vp->requireEnclosedSelection=true;v.parameters=vp;
    v.statistics.volumeMm3.reset();v.statistics.validity=VoxelVolumeValidity::TouchesBoundary;r.voxelVolumes.push_back(v);
    r.presentation.source=MetrologyReportDisplaySource::ExplicitConfiguration;
    MetrologyReportDeviationDisplay display;display.parameters.result=deviation.result;display.parameters.rangeMode=AlignmentRangeMode::Manual;display.parameters.range={0,.5};display.mappingRange=display.parameters.range;display.parameters.selectedVertex=0;
    display.legendRgb=AlignmentDeviationColors::GetLegend(display.parameters,params->tolerance);
    AlignmentDeviationSelection selection;selection.result=deviation.result;selection.vertex=0;selection.sourcePoint={0,0,0};selection.alignedPoint={1.5,0,0};selection.sample=field->samples[0];display.selection=selection;r.presentation.deviations.push_back(display);
    MetrologyReportSceneDisplay scene;auto layout=std::make_shared<AlignmentSceneDisplay>();layout->anchorAlignment=a.result;
    layout->parts={{u8"零件,\"A\"",a.result,false,false,inspection.result,"dimension-0"}};layout->selectedPart=layout->parts[0].id;layout->roiMarkerLimit=7;
    scene.parameters=layout;scene.targetToModel[3]=-1.5;scene.glyphLength=12;scene.wasCurrentAtCapture=false;r.presentation.scene=scene;
    return r;
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
std::string Read(const std::filesystem::path &path) { std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}}; }
void FileCases(const MetrologyReport &report,const std::filesystem::path &base) {
    using Status=MetrologyReportWriteStatus;
    const auto folder=base/("files-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto clean=[&] {
        for(const auto &entry:std::filesystem::directory_iterator(folder))
            if(entry.path().filename().u8string().find(".mvvcvtk-report-")==0)return false;
        return true;
    };
    const auto target=folder/std::filesystem::u8path(u8"报告_尺寸_😀.json");
    std::ostringstream expected;Check(WriteMetrologyReport(report,expected).status==Status::Succeeded,"file reference serialization");
    std::size_t baselineChecks=0;MetrologyReportWriteOptions observed;observed.isCancelled=[&] { ++baselineChecks;return false; };
    auto result=WriteMetrologyReportFile(report,target.u8string(),observed);
    if(!result.isCommitted)std::cerr<<result.write.message<<'\n';
    Check(result.isCommitted && result.write.status==Status::Succeeded && Read(target)==expected.str() &&
        result.write.bytesWritten==expected.str().size() && clean(),"Unicode file commits exact bytes without temporary residue");
    const auto original=Read(target);result=WriteMetrologyReportFile(report,target.u8string());
    Check(result.write.status==Status::AlreadyExists && !result.isCommitted && Read(target)==original && clean(),"existing report is never replaced");
    std::size_t lateChecks=0;MetrologyReportWriteOptions late;late.isCancelled=[&] { return ++lateChecks==baselineChecks; };
    const auto beforeCommit=folder/"before-commit.json";result=WriteMetrologyReportFile(report,beforeCommit.u8string(),late);
    Check(result.write.status==Status::Cancelled && result.write.bytesWritten==expected.str().size() && !result.isCommitted &&
        !std::filesystem::exists(beforeCommit) && clean(),"last cancellation checkpoint discards a fully written temporary before commit");
    MetrologyReportWriteOptions options;
    auto bad=report;bad.title=std::string(40000,'x')+std::string("\xC0\xAF",2);
    const auto malformed=folder/"malformed.json";result=WriteMetrologyReportFile(bad,malformed.u8string());
    Check(result.write.status==Status::InvalidInput && result.write.bytesWritten>0 && !std::filesystem::exists(malformed) && clean(),"late encoding failure discards written prefix");
    auto large=report;large.presentation={};auto field=std::make_shared<AlignmentDeviationField>();field->samples.resize(100000);field->statistics.counts[0]=field->samples.size();large.deviations[0].forward=field;
    std::size_t calls=0;options={};options.includeDeviationSamples=true;options.isCancelled=[&] { return ++calls>=500000; };
    const auto cancelled=folder/"cancelled.json";result=WriteMetrologyReportFile(large,cancelled.u8string(),options);
    Check(result.write.status==Status::Cancelled && result.write.bytesWritten>0 && !result.isCommitted &&
        !std::filesystem::exists(cancelled) && clean(),"mid-file cancellation leaves no final file or temporary prefix");
    calls=0;options={};const auto raced=folder/"raced.json";
    options.isCancelled=[&] { if(++calls==2) { std::ofstream sentinel(raced,std::ios::binary);sentinel<<"concurrent owner"; }return false; };
    result=WriteMetrologyReportFile(report,raced.u8string(),options);
    Check(result.write.status==Status::AlreadyExists && !result.isCommitted && Read(raced)=="concurrent owner" && clean(),"target created during serialization wins without replacement");
    const auto concurrent=folder/"concurrent.json";std::atomic<int> ready{0};std::array<MetrologyReportFileResult,2> writers;
    const auto run=[&](std::size_t i) { ready.fetch_add(1);while(ready.load()<2)std::this_thread::yield();writers[i]=WriteMetrologyReportFile(report,concurrent.u8string()); };
    std::thread first(run,0),second(run,1);first.join();second.join();
    Check(writers[0].isCommitted!=writers[1].isCommitted &&
        (writers[0].isCommitted ? writers[1].write.status : writers[0].write.status)==Status::AlreadyExists &&
        Read(concurrent)==expected.str() && clean(),"two concurrent exporters commit exactly one complete report");
    Check(WriteMetrologyReportFile(report,(folder/"missing"/"a.json").u8string()).write.status==Status::IoError && clean(),"missing output parent is reported without creating directories");
    for(const auto &name:{"NUL.json","report.json:alternate","trailing.json ","dot.json."})
        Check(WriteMetrologyReportFile(report,(folder/name).u8string()).write.status==Status::InvalidInput,"ambiguous device/ADS/trailing path rejected");
    Check(WriteMetrologyReportFile(report,std::string("bad\0path",8)).write.status==Status::InvalidInput && clean(),"embedded path NUL rejected");
    options={};calls=0;options.isCancelled=[&] { if(++calls==2)throw std::runtime_error("callback fault");return false; };
    const auto fault=folder/"callback-fault.json";result=WriteMetrologyReportFile(report,fault.u8string(),options);
    Check(result.write.status==Status::InternalError && !std::filesystem::exists(fault) && clean(),"callback exception cleans its exclusive temporary file");
    std::filesystem::path ownedTemporary;options={};calls=0;
    options.isCancelled=[&] {
        if(++calls!=2)return false;
        for(const auto &entry:std::filesystem::directory_iterator(folder))if(entry.path().filename().u8string().find(".mvvcvtk-report-")==0)ownedTemporary=entry.path();
        if(ownedTemporary.empty())throw std::runtime_error("test temporary not found");
        std::filesystem::permissions(ownedTemporary,std::filesystem::perms::owner_read,std::filesystem::perm_options::replace);
        return true;
    };
    const auto cleanupFailure=folder/"cleanup-failure.json";result=WriteMetrologyReportFile(report,cleanupFailure.u8string(),options);
    Check(result.write.status==Status::Cancelled && !result.isCommitted && !std::filesystem::exists(cleanupFailure) &&
        !ownedTemporary.empty() && !result.remainingTemporaryPath.empty() && std::filesystem::exists(ownedTemporary) &&
        std::filesystem::equivalent(ownedTemporary,std::filesystem::u8path(result.remainingTemporaryPath)),
        "cleanup failure exposes the exact owned temporary path instead of claiming cleanup succeeded");
    std::cout<<"CleanupFailureStatus="<<static_cast<unsigned>(result.write.status)<<" CleanupPathPresent="<<!result.remainingTemporaryPath.empty()
             <<" PathSpellingIdentical="<<(result.remainingTemporaryPath==ownedTemporary.u8string())<<'\n';
    if(!ownedTemporary.empty()) {
        // 仅恢复并删除本用例独立目录内、刚刚由回调标记只读的已知临时文件。
        std::filesystem::permissions(ownedTemporary,std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
        Check(std::filesystem::remove(ownedTemporary) && clean(),"known injected read-only temporary is recovered");
    }
}
}
int main(int argc,char **argv) {
    try {
        auto report=Fixture();MetrologyReportWriteOptions options;std::ostringstream json;
        json.imbue(std::locale(std::locale::classic(),new CommaLocale));json.precision(3);const auto flags=json.flags();
        auto result=WriteMetrologyReport(report,json,options);
        Check(result.status==MetrologyReportWriteStatus::Succeeded && result.bytesWritten==json.str().size(),"JSON bytes confirmed");
        Check(json.str().find("\"measured\":1.5")!=std::string::npos && json.precision()==3 && json.flags()==flags,"JSON is locale independent and preserves stream formatting");
        Check(json.str().find("PositiveInfinity")!=std::string::npos && json.str().find("\"samplesIncluded\":false")!=std::string::npos,"nonfinite diagnostics and omitted samples are explicit");
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
        invalid=report;invalid.voxelVolumes[0].statistics.volumeMm3=999;std::ostringstream badVolume;
        Check(WriteMetrologyReport(invalid,badVolume,options).status==MetrologyReportWriteStatus::InvalidInput,"inconsistent voxel volume rejected");
        invalid=report;invalid.voxelVolumes[0].source=Ref(99);std::ostringstream badSource;
        Check(WriteMetrologyReport(invalid,badSource,options).status==MetrologyReportWriteStatus::InvalidInput,"voxel source mismatch rejected");
        auto volumeOnly=report;volumeOnly.presentation={};volumeOnly.alignments.clear();volumeOnly.inspections.clear();volumeOnly.deviations.clear();std::ostringstream standalone;
        Check(WriteMetrologyReport(volumeOnly,standalone,options).status==MetrologyReportWriteStatus::Succeeded,"voxel report has no fictitious alignment dependency");
        invalid=report;invalid.presentation.deviations[0].legendRgb[128][0]^=1;std::ostringstream wrongPalette;
        Check(WriteMetrologyReport(invalid,wrongPalette,options).status==MetrologyReportWriteStatus::InvalidInput,"tampered colorbar table rejected");
        invalid=report;invalid.presentation.deviations[0].selection->alignedPoint[0]=99;std::ostringstream wrongSelection;
        Check(WriteMetrologyReport(invalid,wrongSelection,options).status==MetrologyReportWriteStatus::InvalidInput,"inconsistent selected point coordinate rejected");
        invalid=report;auto wrongScene=std::make_shared<AlignmentSceneDisplay>(*invalid.presentation.scene->parameters);wrongScene->parts[0].selectedItem="missing";invalid.presentation.scene->parameters=wrongScene;std::ostringstream wrongItem;
        Check(WriteMetrologyReport(invalid,wrongItem,options).status==MetrologyReportWriteStatus::InvalidInput,"report scene item must belong to captured inspection");
        options.includeDeviationSamples=true;std::ostringstream full;
        Check(WriteMetrologyReport(report,full,options).status==MetrologyReportWriteStatus::Succeeded,"full sample JSON writes");
        options.format=MetrologyReportFormat::Csv;std::ostringstream csv;
        Check(WriteMetrologyReport(report,csv,options).status==MetrologyReportWriteStatus::Succeeded && csv.str().find("'="+std::string("part,1"))!=std::string::npos,"CSV formula-like text protected");
        const auto configRowEnd=csv.str().find("\r\n",csv.str().find("deviation-display"));Check(configRowEnd!=std::string::npos && configRowEnd>20,"configuration row is present");
        auto cancelledConfig=options;unsigned configPolls=0;cancelledConfig.isCancelled=[&]{return ++configPolls>2000;};std::ostringstream nestedCancelled;
        Check(WriteMetrologyReport(report,nestedCancelled,cancelledConfig).status==MetrologyReportWriteStatus::Cancelled,"nested CSV configuration cancellation remains explicit");
        invalid=report;invalid.id=" =1+1";std::ostringstream spaced;
        Check(WriteMetrologyReport(invalid,spaced,options).status==MetrologyReportWriteStatus::Succeeded && spaced.str().find("' =1+1")!=std::string::npos,"leading-space formula-like text protected");
        if(argc==2) {
            const std::filesystem::path folder(argv[1]);std::filesystem::create_directories(folder);
            std::ofstream a(folder/"fixture.json",std::ios::binary),b(folder/"fixture.csv",std::ios::binary);
            a<<full.str();b<<csv.str();a.close();b.close();Check(bool(a)&&bool(b),"parser fixtures written");
            FileCases(report,folder);
        }
        auto large=std::make_shared<AlignmentDeviationField>();large->samples.resize(100000);large->statistics.counts[0]=large->samples.size();
        report.presentation={};report.deviations[0].forward=large;options={};std::ostringstream noValues;
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
