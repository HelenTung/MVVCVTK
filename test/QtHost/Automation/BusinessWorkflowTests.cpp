// 文档中的阶段转换及统计来源回归，配合实际 Qt 业务操作检查。
#include "BusinessWorkflowTests.h"
#include "App/TestWindow.h"
#include "Support/BusinessStep.h"
#include "Support/BusinessStatistics.h"
#include "Support/ReferenceDataSource.h"
#if defined(MANUAL_ALIGNMENT)
#include "Modules/AlignmentInput.h"
#endif
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <limits>
namespace Manual {
namespace {
void Require(bool valid,const char* message){if(!valid)throw std::runtime_error(message);}
bool Await(const std::function<bool()>& ready){QElapsedTimer timer;timer.start();while(timer.elapsed()<3000){QCoreApplication::processEvents(QEventLoop::AllEvents,10);if(ready())return true;}return false;}
double Sum(const QJsonObject& data){double sum=0;for(const auto value:data["counts"].toArray())sum+=value.toDouble();return sum;}
}
void CheckBusinessWorkflow(TestWindow& window)
{
#if defined(MANUAL_ALIGNMENT)
    AlignmentRegion selection;selection.vertexIds={1,3,8};
    DataRevisionRef mesh;mesh.entityId.bytes[0]=1;mesh.generation=1;
    SetRegionRoi(selection,{1,2,3},mesh);
    Require(selection.pinnedMesh==mesh && selection.vertexIds==std::vector<std::uint64_t>{1,3},
        "shared spatial ROI intersects the existing revision-pinned vertex selection");
    bool rejected=false;try{SetRegionRoi(selection,{},mesh);}catch(const std::invalid_argument&){rejected=true;}
    Require(rejected && selection.vertexIds==std::vector<std::uint64_t>{1,3},
        "empty ROI selection cannot become the complete mesh or overwrite the prior selection");
#endif
    // 与完整真实 CT 相同的网格周期；无需分配十余 GB 就能复现抽样列偏置。
    const std::array<int,3> dimensions{1536,1536,1536};
    const std::size_t total=std::size_t(1536)*1536*1536;
    const auto stride=ReferenceDataSource::GetGraySampleStride(total,dimensions);
    std::array<bool,3> low{},high{};std::size_t central=0,samples=0;
    for(std::size_t voxel=0;voxel<total;voxel+=stride){
        const std::array<std::size_t,3> point{voxel%1536,(voxel/1536)%1536,voxel/(std::size_t(1536)*1536)};
        bool inside=true;for(int axis=0;axis<3;++axis){low[axis]=low[axis] || point[axis]<384;high[axis]=high[axis] || point[axis]>=1152;inside=inside && point[axis]>=384 && point[axis]<1152;}
        central+=inside;++samples;
    }
    Require(samples<=262144 && central>10000 && std::all_of(low.begin(),low.end(),[](bool value){return value;}) && std::all_of(high.begin(),high.end(),[](bool value){return value;}),"full CT statistics sampling spans every axis and its central ROI rather than repeated X columns");
    const QJsonObject input{{"available",true},{"dataRevision","source-A"}};
    Require(GetBusinessStep("Crop",{{"output","00000000000000000000000000000000:0"}},input,"Start").action=="Start","an empty crop revision cannot masquerade as a published result before the first crop");
    Require(GetBusinessStep("Data",{{"acceptedRevision","B"},{"activeRevision","A"}},input,"Load").action=="ActivateAccepted","accepted data must offer activation retry without reloading");
    const auto confirmed=GetBusinessStep("Crop",{{"isConfirmed",true}},input,"Start");
    Require(confirmed.action=="BuildResult" && confirmed.usesOutput,"confirmed crop offers generation followed by exact adoption");
    Require(GetBusinessStep("Crop",{{"isConfirmed",false},{"operationCount","3"}},input,"BuildResult").action=="FinishEditing","changed history returns to confirmation instead of publishing");
    Require(GetBusinessStep("Crop",{{"output","crop-B"}},input,"Start").action=="SelectOutput","published crop offers adoption retry");
    Require(GetBusinessStep("Crop",{{"output","source-A"}},input,"Start").title.contains("继续裁剪"),"adopted crop continues from current input");
    Require(GetBusinessStep("Artifact",{{"hasCandidate",true},{"isCandidateCurrent",true}},input,"Ring").usesOutput,"artifact candidate offers publish and adoption");
    Require(GetBusinessStep("Artifact",{{"canSelectOutput",true}},input,"Ring").action=="SelectOutput","published artifact does not recompute for adoption retry");
    Require(GetBusinessStep("Roi",{{"hasDraft",true}},input,"Begin").action=="Commit","ROI draft offers formal save");
    Require(GetBusinessStep("Part",{{"hasPreview",true}},input,"Paint").action=="Commit","part editing candidate remains distinct from formal labels");
    Require(GetBusinessStep("Surface",{{"hasPreview",true}},input,"GlobalAutomatic").action=="Determine","surface preview must precede formal measurement qualification");
    Require(GetBusinessStep("Alignment",{},input,"EvaluateVoxelVolume").action=="EvaluateVoxelVolume","voxel volume remains independent of alignment preparation");
    Require(GetBusinessStep("Gap",{{"hasResult",true},{"isCurrent",false}},input,"Start").action=="Start","old input defect results cannot appear as current analysis");
    Require(GetBusinessStep("Gap",{{"hasResult",true},{"isCurrent",true},{"parametersChanged",true}},input,"SetDisplay").action=="Start","algorithm filter edits require analysis rather than list refresh");
    Require(GetBusinessStep("Gap",{{"hasResult",true},{"isCurrent",true}},input,"Start").action=="Defects","current defect result opens its catalog");
    for(const auto* name:{"Data","View","Rotation","Artifact","Roi","Crop","Part","Surface","Alignment","Wall","Gap"}) {
        auto* page=window.GetModule(name);Require(page!=nullptr,"all eleven business pages are present");
        auto* primary=page->findChild<QPushButton*>("businessPrimary");auto* stage=page->findChild<QLabel*>("businessStage");auto* scroll=page->findChild<QScrollArea*>("operationScroll");
        Require(primary && stage && scroll && !scroll->isAncestorOf(primary),"every business has a fixed stage primary outside parameter scrolling");
    }
    Require(window.findChild<QLabel*>("currentInputIdentity") && window.findChild<QLabel*>("currentInputGeometry"),"input identity and geometry have dedicated persistent widgets");
    const auto weighted=BuildHistogram({-1,0,1},{2,3,5},"area","fixed result","mm","mm2");
    Require(Sum(weighted)==10,"area histogram sums area rather than sample count");
    Require(BuildHistogram({std::numeric_limits<double>::quiet_NaN()},{},"invalid","invalid","gray","count")["counts"].toArray().isEmpty(),"invalid samples are not inserted in zero bin");
    BusinessStatistics statistics;
    const auto reader=[](std::size_t){StatisticsSamples samples;samples.values={0,0,1,1};samples.total=4;samples.next=4;samples.description="revision R1";return samples;};
    statistics.StartHistogram("R1","gray","gray","count",reader);
    Require(Await([&]{return !statistics.GetHistogram()["counts"].toArray().isEmpty();}) && Sum(statistics.GetHistogram())==4,"bounded histogram conserves the known samples");
    statistics.SetHistogram(weighted);
    statistics.StartHistogram("R1","gray","gray","count",reader);
    Require(Await([&]{return statistics.GetHistogram()["title"]=="gray" && !statistics.GetHistogram()["counts"].toArray().isEmpty();}) && Sum(statistics.GetHistogram())==4,"static chart cannot impersonate a cached histogram from the same revision");
    statistics.StartHistogram("zero","defects","mm3","count",[](std::size_t){StatisticsSamples samples;samples.description="published empty defect table";return samples;});
    Require(Await([&]{return statistics.GetHistogram()["description"].toString().contains("有效结果：0");}),"valid empty defect table is distinct from invalid samples");
    bool oldRead=false;
    statistics.StartHistogram("old","old","gray","count",[&](std::size_t){oldRead=true;return StatisticsSamples{};});
    statistics.StartHistogram("new","new","gray","count",reader);
    Require(Await([&]{return !statistics.GetHistogram()["counts"].toArray().isEmpty();}) && !oldRead && statistics.GetHistogram()["cacheKey"]=="new","replaced histogram job cannot publish a late old-source curve");
}
}
