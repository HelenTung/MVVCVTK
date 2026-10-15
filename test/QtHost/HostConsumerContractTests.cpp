// 测试用途：从普通 Host API 验证接纳/完成边界、历史值查询和快照独立性。
#include "QtHostMethodCases.h"
#include "Host/VtkAppHostSession.h"
#include "Data/VtkDataBridge.h"
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCryptographicHash>
#include <set>
#include <deque>
#include <cstring>
#include <iostream>
#include <functional>
#include <cmath>

namespace {
void CheckRealRoiOperations(VtkAppHostSession& session,const ImageDescriptor& image,const QJsonObject& manifest,
    const std::function<void(bool,const char*)>& check)
{
    using Cell=std::array<int,3>;using Cells=std::set<Cell>;
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    const auto read=session.GetImageReadResult();
    require(read.error==ImageReadError::None && read.state && read.state->values,"real ROI source read failed");
    QFile input(manifest["filePath"].toString());require(input.open(QIODevice::ReadOnly),"real ROI source unavailable");const auto raw=input.readAll();
    require(read.state->valueType==ImageValueType::Float32 && read.state->componentBytes==sizeof(float)
        && read.state->componentCount==1 && std::size_t(raw.size())==read.state->values->size(),"real RAW read layout differs from Float32 input");
    // 输入是 LPS、公开数据是 RAS：同一 Z 层的 X/Y 索引反向，值本身不改变。
    const auto rawOffset=[&](std::size_t voxel){const auto nx=std::size_t(image.dims[0]),ny=std::size_t(image.dims[1]);
        const auto x=voxel%nx,y=(voxel/nx)%ny,z=voxel/(nx*ny);return (z*ny+ny-1-y)*nx+nx-1-x;};
    const auto rawValue=[&](std::size_t voxel){float value;std::memcpy(&value,raw.constData()+rawOffset(voxel)*sizeof(float),sizeof(float));return value;};
    for(std::size_t voxel=0;voxel<std::size_t(raw.size())/sizeof(float);++voxel)
        require(!std::memcmp(read.state->values->data()+voxel*sizeof(float),raw.constData()+rawOffset(voxel)*sizeof(float),sizeof(float)),"public RAS source values differ from mapped real LPS RAW");
    const auto inside=[&](Cell p){for(int a=0;a<3;++a)if(p[a]<image.extent[a*2] || p[a]>image.extent[a*2+1])return false;return true;};
    const std::vector<Cell> kernel{{0,0,0},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const auto offset=[](Cell a,Cell b){return Cell{a[0]+b[0],a[1]+b[1],a[2]+b[2]};};
    Cells expected;std::size_t index=0;
    for(int z=0;z<image.dims[2];++z)for(int y=0;y<image.dims[1];++y)for(int x=0;x<image.dims[0];++x,++index) {
        const float value=rawValue(index);
        if(value>=0 && value<=image.scalarRange[1])expected.insert({x+image.extent[0],y+image.extent[2],z+image.extent[4]});
    }
    const auto members=[&](const RoiDescriptor& roi){
        require(roi.definition.nodes.size()==1 && roi.definition.nodes.front().primitive.voxels,"real ROI output has no voxel payload");
        const auto& primitive=roi.definition.nodes.front().primitive;Cells cells;
        for(const auto& run:primitive.voxels->GetRuns())if(primitive.labels.empty() || std::find(primitive.labels.begin(),primitive.labels.end(),run.label)!=primitive.labels.end())
            for(int x=run.first[0];x<=run.last;++x)cells.insert({x,run.first[1],run.first[2]});
        return cells;
    };
    RoiOperationRequest request;request.source=image.dataRevision;request.output=RoiOutputMode::CreateNew;
    request.params=RoiThresholdParams{RoiThresholdMode::Threshold,{0,image.scalarRange[1]}};
    const auto threshold=session.SetRoiOperation(request);require(threshold.error==RoiError::None && threshold.rois.size()==1,"real threshold failed");
    check(members(threshold.rois.front())==expected,"real CT threshold preserves independently selected RAW membership");
    // 真实 CT 派生的标签输入验证通用 VTK 转换；外部数组修改后仍保持正式快照。
    auto labels=vtkSmartPointer<vtkImageData>::New();labels->SetExtent(image.extent[0],image.extent[1],image.extent[2],image.extent[3],image.extent[4],image.extent[5]);
    labels->SetSpacing(image.spacing[0],image.spacing[1],image.spacing[2]);labels->SetOrigin(image.origin[0],image.origin[1],image.origin[2]);
    labels->SetDirectionMatrix(image.direction.data());labels->AllocateScalars(VTK_INT,1);
    auto* labelValues=static_cast<std::int32_t*>(labels->GetScalarPointer());
    std::fill(labelValues,labelValues+index,0);
    for(const auto& cell:expected){const auto at=std::size_t(cell[0]-image.extent[0])+std::size_t(image.dims[0])*(std::size_t(cell[1]-image.extent[2])+std::size_t(image.dims[1])*std::size_t(cell[2]-image.extent[4]));labelValues[at]=1;}
    VtkDataBridge bridge;const auto frozenLabels=bridge.CreateLabelPayload(labels);require(frozenLabels && frozenLabels->GetValueType()==ImageValueType::Int32,"real-derived label import failed");
    check(frozenLabels->GetGeometry().extent==image.extent && frozenLabels->GetGeometry().origin==image.origin
        && frozenLabels->GetGeometry().spacing==image.spacing && frozenLabels->GetGeometry().direction==image.direction,"real-derived label import preserves CT geometry");
    std::fill(labelValues,labelValues+index,99);const auto* imported=static_cast<const std::int32_t*>(frozenLabels->GetValueData());
    std::size_t foreground=0;for(std::size_t i=0;i<index;++i){const float value=rawValue(i);const bool selected=value>=0 && value<=image.scalarRange[1];require(imported[i]==int(selected),"real-derived label membership or snapshot isolation changed");foreground+=selected;}
    check(foreground==expected.size(),"real-derived labels preserve the complete CT selection");
    request.target=threshold.rois.front().revision;
    const auto morph=[&](const Cells& cells,bool dilate){Cells result;
        for(const auto& cell:cells){bool keep=true;for(const auto& delta:kernel){const auto point=offset(cell,delta);if(dilate)result.insert(point);else if(!cells.count(point))keep=false;}if(!dilate && keep)result.insert(cell);}return result;};
    for(const auto operation:{RoiMorphologyOperation::Dilate,RoiMorphologyOperation::Erode,RoiMorphologyOperation::Open,RoiMorphologyOperation::Close}) {
        auto truth=morph(expected,operation==RoiMorphologyOperation::Dilate || operation==RoiMorphologyOperation::Close);
        if(operation==RoiMorphologyOperation::Open || operation==RoiMorphologyOperation::Close)truth=morph(truth,operation==RoiMorphologyOperation::Open);
        for(auto it=truth.begin();it!=truth.end();)if(!inside(*it))it=truth.erase(it);else ++it;
        request.params=RoiMorphologyParams{operation,1,RoiRadiusUnit::Voxels};const auto result=session.SetRoiOperation(request);
        require(result.error==RoiError::None && result.rois.size()==1,"real morphology failed");
        check(members(result.rois.front())==truth,"real CT morphology matches discrete set truth with final-only clipping");
    }
    const auto components=[&](Cells remaining,const std::vector<Cell>& neighbours){std::vector<Cells> result;
        while(!remaining.empty()){const auto seed=*remaining.begin();remaining.erase(seed);Cells component{seed};std::deque<Cell> queue{seed};
            while(!queue.empty()){const auto cell=queue.front();queue.pop_front();for(const auto& delta:neighbours){const auto next=offset(cell,delta);if(remaining.erase(next)){component.insert(next);queue.push_back(next);}}}result.push_back(std::move(component));}
        return result;};
    for(const auto connectivity:{RoiConnectivity::Faces,RoiConnectivity::Full}) {
        std::vector<Cell> neighbours;
        for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
            if((x||y||z) && (connectivity==RoiConnectivity::Full || std::abs(x)+std::abs(y)+std::abs(z)==1))neighbours.push_back({x,y,z});
        const auto truth=components(expected,neighbours);RoiComponentParams split;split.operation=RoiComponentOperation::Split;split.connectivity=connectivity;request.params=split;
        const auto result=session.SetRoiOperation(request);require(result.error==RoiError::None && result.rois.size()==truth.size(),"real component split failed");
        for(std::size_t i=0;i<truth.size();++i)check(result.componentVoxelCounts[i]==truth[i].size() && members(result.rois[i])==truth[i],"real CT split preserves connectivity, membership and lexicographic numbering");
    }
    Cells background;
    for(int z=image.extent[4];z<=image.extent[5];++z)for(int y=image.extent[2];y<=image.extent[3];++y)for(int x=image.extent[0];x<=image.extent[1];++x)if(!expected.count({x,y,z}))background.insert({x,y,z});
    auto filled=expected;
    for(const auto& component:components(std::move(background),kernel)){bool exterior=false;
        for(const auto& cell:component){for(int a=0;a<3;++a)if(cell[a]==image.extent[a*2] || cell[a]==image.extent[a*2+1])exterior=true;if(exterior)break;}
        if(!exterior)filled.insert(component.begin(),component.end());}
    RoiComponentParams fill;fill.operation=RoiComponentOperation::FillHoles;request.params=fill;
    const auto holes=session.SetRoiOperation(request);require(holes.error==RoiError::None && holes.rois.size()==1,"real fill failed");
    check(members(holes.rois.front())==filled,"real CT fill retains exterior background and fills only enclosed cavities");
    const auto before=session.GetDataCatalog();request.getCancelled=[]{return true;};const auto cancelled=session.SetRoiOperation(request);const auto after=session.GetDataCatalog();
    check(cancelled.error==RoiError::Cancelled && before && after && before->commitId==after->commitId,"cancelled real ROI transaction preserves all published revisions");
    check(session.GetImageDescriptor()->dataRevision==image.dataRevision,"real ROI operations retain the exact active source");
    std::cout<<"Real CT ROI threshold/morphology/components/fill members="<<expected.size()<<std::endl;
}
}

int GetConsumerContractFailCount()
{
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        failures += !GetCaseResult(value, message);
    };
    HostSessionConfig config;
    config.driveMode = HostDriveMode::HostDriven;
    HostRenderViewConfig view;
    view.id = "consumer-contract";
    view.role = HostRenderViewRole::Primary3D;
    view.renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    view.renderWindow->SetOffScreenRendering(1);
    config.renderViews.push_back(view);
    auto ownedSession = std::make_shared<VtkAppHostSession>(std::move(config));
    auto& session = *ownedSession;
    check(!session.GetDataCatalog(), "catalog does not build an unready Session");
    if (!session.BuildSession()) return failures + 1;
    const auto owner = std::this_thread::get_id();
    QJsonObject real;
    const auto manifestPath = qgetenv("MVVCVTK_HOST_CONSUMER_REAL_MANIFEST");
    if (!manifestPath.isEmpty()) {
        QFile file(QString::fromUtf8(manifestPath));
        if (!file.open(QIODevice::ReadOnly)) return failures + 1;
        real = QJsonDocument::fromJson(file.readAll()).object();
        QFile input(real["filePath"].toString());
        if (real["evidenceKind"].toString() != "real-data-roi" || real["inputFormat"].toString() != "float32-le-xfastest"
            || !input.open(QIODevice::ReadOnly)
            || QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256).toHex()
                != real["sha256"].toString().toLatin1()) return failures + 1;
    }
    const auto load = [&](const char* id, float value) {
        std::unique_ptr<HostRequest> request;
        if (real.isEmpty()) {
            auto reload = std::make_unique<HostReloadRequest>();
            reload->geometry.dimensions = {2, 2, 2};
            reload->geometry.spacing = {.7F, 1.1F, 1.3F};
            reload->geometry.origin = {12.F, -17.F, 23.F};
            reload->metadata.identity.datasetId = id;
            reload->metadata.source.kind = ImageSourceKind::Memory;
            reload->metadata.source.uri = std::string("memory://") + id;
            reload->voxels.assign(8, value);
            request = std::move(reload);
        } else {
            auto raw = std::make_unique<HostLoadRequest>();
            raw->filePath = real["filePath"].toString().toStdString();
            for (int axis = 0; axis < 3; ++axis) {
                raw->geometry.dimensions[axis] = real["dimensions"].toArray()[axis].toInt();
                raw->geometry.spacing[axis] = real["spacingLPS"].toArray()[axis].toDouble();
                raw->geometry.origin[axis] = real["originLPS"].toArray()[axis].toDouble();
            }
            for (int index = 0; index < 9; ++index)
                raw->geometry.direction[index] = real["directionLPS"].toArray()[index].toDouble();
            raw->metadata.identity.datasetId = id;
            raw->metadata.source.kind = ImageSourceKind::RawFile;
            raw->metadata.source.uri = raw->filePath;
            raw->metadata.source.digest = real["sha256"].toString().toStdString();
            request = std::move(raw);
        }
        struct CompleteState final {
            std::mutex mutex;
            int count = 0;
            HostResult result;
            bool isOwner = false;
            bool queryReady = false;
        };
        auto state = std::make_shared<CompleteState>();
        const std::weak_ptr<VtkAppHostSession> weak = ownedSession;
        const auto admission = session.SendRequest(std::move(*request), [state, weak, owner](HostResult completed) {
            const auto host = weak.lock();
            const bool ready = host && host->GetDataCatalog().has_value();
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->isOwner = std::this_thread::get_id() == owner;
            state->queryReady = ready;
            state->result = std::move(completed);
            ++state->count;
        });
        if (admission.status != HostAdmissionStatus::Accepted) throw std::runtime_error(admission.message);
        const auto count = [&] { const std::lock_guard<std::mutex> lock(state->mutex); return state->count; };
        // 仅为测试等待设置安全 deadline，不作为产品时延指标。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!count() && std::chrono::steady_clock::now() < deadline) {
            session.SendUpdates();
            std::this_thread::yield();
        }
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            if (state->count != 1 || !state->result.isSucceeded) throw std::runtime_error("load completion failed");
            check(state->isOwner, "completion runs on owner thread");
            check(state->queryReady, "completion can reenter a value query");
        }
        session.SendUpdates();
        check(count() == 1, "accepted completion is delivered exactly once");
        return *session.GetImageDescriptor();
    };
    try {
        const auto first = load("consumer-first", 7.F);
        const auto frozen = session.GetDataCatalog();
        check(frozen && std::any_of(frozen->data.begin(), frozen->data.end(), [&](const auto& item) {
            return item.revision == first.dataRevision && item.type == DataTypes::imageGrid3D;
        }), "catalog includes the exact published image revision");
        DataQuery query;
        query.entityId = first.dataRevision.entityId;
        query.type = DataTypes::imageGrid3D;
        const auto filtered = session.GetDataCatalog(query);
        check(filtered && filtered->data.size() == 1 && filtered->data.front().revision == first.dataRevision,
            "catalog applies entity and type filters");
        const auto historical = session.GetImageDescriptor(first.dataRevision);
        check(historical && historical->dims == first.dims && historical->spacing == first.spacing
            && historical->origin == first.origin && historical->scalarRange == first.scalarRange
            && historical->bindingRevision == first.bindingRevision
            && historical->componentBytes == sizeof(float) && historical->componentCount == 1
            && historical->componentBytes == first.componentBytes && historical->byteOrder == first.byteOrder
            && historical->tupleOrder == first.tupleOrder, "exact image query preserves geometry, layout and current binding");
        if (!real.isEmpty()) check(first.metadata.source.digest == real["sha256"].toString().toStdString()
            && first.metadata.source.kind == ImageSourceKind::RawFile,
            "real input keeps its verified source identity through the public loader");
        if(!real.isEmpty())CheckRealRoiOperations(session,first,real,check);
        const auto second = load("consumer-second", 42.F);
        const auto old = session.GetImageDescriptor(first.dataRevision);
        check(second.dataRevision != first.dataRevision && old && old->dataRevision == first.dataRevision
            && old->scalarRange == first.scalarRange && old->bindingRevision == 0,
            "historical query does not silently select the new primary image");
        const auto now = session.GetDataCatalog();
        check(frozen && now && frozen->commitId != now->commitId && frozen->data.size() < now->data.size(),
            "catalog snapshots remain independent after publication");
        check(!session.GetImageDescriptor(DataRevisionRef{{"unknown-image"}, 1}), "unknown revision has no image descriptor");
        HostAdmission wrong;
        int rejectedCompletions = 0;
        bool wrongQueriesRejected = false;
        std::thread thread([&] {
            HostViewSetRequest request;
            request.targetView.viewId = view.id;
            request.opacity = .5;
            wrong = session.SendRequest(std::move(request), [&](HostResult) { ++rejectedCompletions; });
            wrongQueriesRejected = !session.GetDataCatalog() && !session.GetImageDescriptor(first.dataRevision);
        });
        thread.join();
        check(wrong.status == HostAdmissionStatus::Rejected && wrong.errorCode == HostErrorCode::WrongThread
            && rejectedCompletions == 0 && wrongQueriesRejected, "wrong-thread rejection never calls user completion or reads payload");
        HostReloadRequest invalid;
        invalid.geometry.dimensions = {2, 2, 2};
        invalid.voxels.assign(7, 1.F);
        const auto rejected = session.SendRequest(std::move(invalid), [&](HostResult) { ++rejectedCompletions; });
        session.SendUpdates();
        check(rejected.status == HostAdmissionStatus::Rejected && rejected.errorCode == HostErrorCode::RequestRejected
            && rejectedCompletions == 0, "invalid request has admission diagnostics and no later completion");
        check(session.Stop(), "consumer Session stops");
        check(!session.GetDataCatalog() && !session.GetImageDescriptor(first.dataRevision), "stopped Session rejects historical queries");
        check(frozen && std::any_of(frozen->data.begin(), frozen->data.end(), [&](const auto& item) {
            return item.revision == first.dataRevision;
        }),
            "value catalog survives Session Stop");
    }
    catch (const std::exception& error) {
        GetCaseResult(false, error.what());
        ++failures;
        (void)session.Stop();
    }
    return failures;
}
