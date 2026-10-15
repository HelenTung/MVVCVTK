#include "FeatureSupport/SpatialMeshCache.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <Psapi.h>

namespace {
void Require(bool valid,const char* message){if(!valid)throw std::runtime_error(message);}
using Source=FeatureInternal::SpatialMeshSource;
Source::Planes Box(double low,double high)
{
    return {{{1,0,0,-low},{-1,0,0,high},{0,1,0,1},{0,-1,0,2},{0,0,1,1},{0,0,-1,1}}};
}
bool Drain(const std::shared_ptr<FeatureInternal::ReadTaskQueue>& queue)
{
    std::optional<FeatureInternal::ReadTaskQueue::Job> job;
    {const std::lock_guard<std::mutex> lock(queue->mutex);job=queue->TakeLocked();}
    if(!job)return false;
    job->task([cancelled=job->cancelled]{return cancelled->load(std::memory_order_acquire);});
    {const std::lock_guard<std::mutex> lock(queue->mutex);queue->FinishLocked();}
    return true;
}
}
// 显式大规模存储/访问验证；不把合成几何冒充供应商检测或真实 CT 全链验证。
int CheckLargeAccess(std::uint64_t count)
{
    if(count<3||count>UINT32_MAX)throw std::invalid_argument("large point count is outside the formal index range");
    using Clock=std::chrono::steady_clock;const auto start=Clock::now();
    const auto elapsed=[&]{return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();};
    IO_COUNTERS before{};GetProcessIoCounters(GetCurrentProcess(),&before);
    ResultArrayScope storage(0);
    DataArray<double> vertices(static_cast<std::size_t>(count)*3);
    DataArray<std::uint64_t> faces(static_cast<std::size_t>(count/3)*3);
    for(std::uint64_t point=0;point<count;++point){const auto face=point/3,corner=point%3;
        vertices[3*point]=double(face%8192)*2+(corner==1);vertices[3*point+1]=double(face/8192)*2+(corner==2);vertices[3*point+2]=0;
        if(point<faces.size())faces[point]=point;
    }
    const auto* coordinates=vertices.data();const auto* topology=faces.data();
    auto mesh=std::make_shared<const SurfaceMeshPayload>(std::move(vertices),std::move(faces));storage.Finish();
    Require(mesh->GetVertices().data()==coordinates&&mesh->GetTriangles().data()==topology,"large formal arrays were copied at adoption");
    const auto formalMs=elapsed();std::cout<<"large formal ready points="<<count<<" faces="<<count/3<<" elapsedMs="<<formalMs<<std::endl;
    auto source=Source::Build(mesh);std::weak_ptr<const SurfaceMeshPayload> weak=mesh;mesh.reset();
    const auto indexMs=elapsed()-formalMs;std::cout<<"large spatial index ready indexMs="<<indexMs<<std::endl;
    const auto query=source->Find(Box(199.9,201.1),8);
    Require(query.isComplete&&!query.batches.empty(),"large local view exceeded its exact page target");
    const auto viewStart=elapsed();auto page=source->Read(query.batches.front());
    Require(page.sourceFacesRead<=Source::facesPerBatch&&page.sourcePointsRead<=3*Source::facesPerBatch,"large view decoded full geometry");
    for(int i=0;i<100;++i)Require(source->Find(Box(199.9,201.1),8).batches==query.batches,"large query identity changed");
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    Require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))!=0,"cannot read large-run memory counters");
    IO_COUNTERS after{};GetProcessIoCounters(GetCurrentProcess(),&after);
    std::cout<<"large-access points="<<count<<" faces="<<source->GetFaceCount()<<" formalBytes="<<(count*3*sizeof(double)+(count/3)*3*sizeof(std::uint64_t))
        <<" formalMs="<<formalMs<<" indexMs="<<indexMs<<" local100QueriesAndReadMs="<<elapsed()-viewStart
        <<" queryNodes="<<query.visitedNodes<<" visibleBatches="<<query.batches.size()<<" decodedFaces="<<page.sourceFacesRead<<" decodedPoints="<<page.sourcePointsRead
        <<" peakWorkingSetBytes="<<memory.PeakWorkingSetSize<<" currentPrivateBytes="<<memory.PrivateUsage
        <<" processReadTransferBytes="<<after.ReadTransferCount-before.ReadTransferCount<<" processWriteTransferBytes="<<after.WriteTransferCount-before.WriteTransferCount<<std::endl;
    source.reset();Require(weak.expired(),"large formal owner leaked after source release");return 0;
}
int main(int argc,char** argv)
{
    try {
        if(argc==3&&std::string(argv[1])=="--large-points")return CheckLargeAccess(std::stoull(argv[2]));
        if(argc!=1)throw std::invalid_argument("use --large-points <count> for explicit large-result validation");
        Require(FeatureInternal::SpatialFaceOrder::GetUses32BitForCount(0)
            &&FeatureInternal::SpatialFaceOrder::GetUses32BitForCount(std::uint64_t{UINT32_MAX}+1)
            &&!FeatureInternal::SpatialFaceOrder::GetUses32BitForCount(std::uint64_t{UINT32_MAX}+2),"index width changes only when an actual face ID exceeds 32 bits");
        constexpr std::size_t batches=128,selected=73;
        ResultArrayScope storage;
        DataArray<double> vertices;DataArray<std::uint64_t> faces;
        vertices.reserve(batches*9);faces.reserve(batches*Source::facesPerBatch*3);
        for(std::size_t batch=0;batch<batches;++batch) {
            const double x=static_cast<double>(batch*10);
            for(const auto value:{x,0.,0.,x+1,0.,0.,x,1.,0.})vertices.push_back(value);
            for(std::size_t i=0;i<Source::facesPerBatch;++i)for(unsigned j=0;j<3;++j)faces.push_back(batch*3+(j+i%3)%3);
        }
        auto mesh=std::make_shared<const SurfaceMeshPayload>(std::move(vertices),std::move(faces));
        storage.Finish();std::weak_ptr<const SurfaceMeshPayload> weak=mesh;
        unsigned totalBuildChecks=0;
        auto source=Source::Build(mesh,[&]{++totalBuildChecks;return false;});mesh.reset();
        Require(source->GetIs32BitIndex(),"display permutation and reverse lookup retain 32-bit indices when sufficient");
        Require(!weak.expired()&&source->GetBatchCount()==batches,"source shares formal owner and complete batch directory");
        const auto query=source->Find(Box(selected*10-.1,selected*10+1.1),8);
        Require(query.isComplete&&query.batches==std::vector<std::uint64_t>{selected}&&query.visitedNodes<32,"small view visits BVH path, not whole directory");
        const auto all=source->Find(Box(-1,2000),4);
        Require(!all.isComplete&&all.batches.size()==4,"capacity reports incomplete exact visibility explicitly");
        Require(source->Find(Box(-20,-1),4).batches.empty(),"outside view reads no geometry");
        auto batch=source->Read(selected);
        Require(batch.sourceFacesRead==Source::facesPerBatch&&batch.sourcePointsRead==3&&batch.mesh->GetNumberOfPoints()==3,"one batch reads only its referenced points");
        Require(batch.mesh->GetPolys()->IsStorage64Bit()==false,"batch connectivity remains 32 bit");
        auto* points=vtkIdTypeArray::SafeDownCast(batch.mesh->GetPointData()->GetArray(Source::pointIds));
        auto* ids=vtkIdTypeArray::SafeDownCast(batch.mesh->GetCellData()->GetArray(Source::faceIds));
        Require(points&&ids&&ids->GetNumberOfValues()==Source::facesPerBatch,"exact integer picking maps exist");
        for(vtkIdType i=0;i<ids->GetNumberOfValues();++i)Require(ids->GetValue(i)==selected*Source::facesPerBatch+i,"face IDs preserve formal order");
        for(vtkIdType i=0;i<3;++i){double p[3];batch.mesh->GetPoint(i,p);Require(points->GetValue(i)==selected*3+i,"point IDs preserve global identity");const auto locked=weak.lock();Require(std::equal(p,p+3,locked->GetVertices().data()+3*points->GetValue(i)),"coordinates exactly equal formal data");}
        for(vtkIdType face=0;face<10;++face){vtkIdType count=0;const vtkIdType* corners=nullptr;batch.mesh->GetPolys()->GetCellAtId(face,count,corners);Require(count==3,"triangle remains triangular");for(unsigned j=0;j<3;++j)Require(points->GetValue(corners[j])==selected*3+(j+face%3)%3,"local connectivity restores exact global corner order");}
        const auto first=selected*Source::facesPerBatch+10;
        const auto detail=source->Read(selected,{},first,first+3);
        Require(detail.firstFace==first&&detail.sourceFacesRead==3&&detail.mesh->GetNumberOfCells()==3,"object range can cover a partial shared batch");
        Require(source->Find(Box(-1,2000),8,first,first+3).batches==query.batches,"object face range restricts the query");
        Require(source->Find(Box(-1,2000),8,Source::facesPerBatch-1,Source::facesPerBatch+1).batches==std::vector<std::uint64_t>{0,1},"half-open face range crosses the batch seam without loss");
        Require(source->Find(Box(-1,2000),8,100,100).batches.empty()&&source->Find(Box(-1,2000),8,100,50).batches.empty(),"empty and reversed ranges read no batches");
        Require(source->Read(0,{},Source::facesPerBatch-1,Source::facesPerBatch).sourceFacesRead==1&&source->Read(1,{},Source::facesPerBatch,Source::facesPerBatch+1).sourceFacesRead==1,"seam endpoints belong to exactly one batch");
        bool cancelled=false;try{source->Read(selected,[]{return true;});}catch(const std::runtime_error&){cancelled=true;}
        Require(cancelled,"cancelled read cannot publish a partial batch");
        unsigned partialCalls=0;cancelled=false;try{source->Read(selected,[&]{return ++partialCalls>6;});}catch(const std::runtime_error&){cancelled=true;}
        Require(cancelled&&partialCalls>6,"cancellation during batch preparation discards partial VTK objects");
        unsigned calls=0;cancelled=false;try{Source::Build(weak.lock(),[&]{return ++calls>5;});}catch(const std::runtime_error&){cancelled=true;}
        Require(cancelled&&calls>5,"index construction checks cancellation while reading faces");
        calls=0;cancelled=false;try{Source::Build(weak.lock(),[&]{return ++calls>totalBuildChecks-2;});}catch(const std::runtime_error&){cancelled=true;}
        Require(cancelled&&calls>totalBuildChecks-2,"late spatial-index cancellation refuses candidate publication");
        calls=0;cancelled=false;try{source->Find(Box(-1,2000),batches,0,UINT64_MAX,[&]{return ++calls>5;});}catch(const std::runtime_error&){cancelled=true;}
        Require(cancelled&&calls>5,"large visibility queries observe cancellation");
        const auto queue=std::make_shared<FeatureInternal::ReadTaskQueue>();
        auto cache=std::make_shared<FeatureInternal::SpatialMeshCache>(source,queue,16);
        const FeatureInternal::SpatialMeshCache::Key key{selected};
        Require(cache->Get(key).isPending&&cache->GetCounters().batchesRead==0,"owner request never decodes geometry synchronously");
        Require(Drain(queue),"existing worker accepts display read");
        const auto ready=cache->Get(key);
        Require(ready.mesh&&cache->Get(key).mesh==ready.mesh&&cache->GetCounters().batchesRead==1,"views share one decoded batch and cache hits read no formal records");
        auto* originalPoints=ready.mesh->GetPoints();auto* originalCells=ready.mesh->GetPolys();
        cache->SetDecoration([](auto& mesh,const auto&,const auto&){vtkNew<vtkIntArray> colors;colors->SetName("display.test");colors->SetNumberOfValues(mesh.GetNumberOfCells());colors->FillValue(17);mesh.GetCellData()->SetScalars(colors);});
        Require(cache->Get(key).isPending&&Drain(queue),"palette update is prepared by existing worker");
        const auto colored=cache->Get(key);
        Require(colored.mesh&&colored.mesh->GetPoints()==originalPoints&&colored.mesh->GetPolys()==originalCells
            &&colored.mesh->GetCellData()->GetScalars()->GetComponent(0,0)==17&&cache->GetCounters().batchesRead==1,"recolor shares points and cells without rereading source geometry");
        cache->SetDecoration([](auto&,const auto&,const auto&){throw std::runtime_error("injected display failure");});
        Require(cache->Get(key).isPending&&Drain(queue)&&!cache->Get(key).error.empty()&&weak.lock()->GetValid(),"display read failure preserves the formal result");
        cache->SetDecoration({});cache->Get(key);
        {const std::lock_guard<std::mutex> lock(queue->mutex);queue->CancelLocked();}
        Require(cache->Get(key).isPending&&Drain(queue)&&cache->Get(key).mesh,"abandoned queued read can be requested again");
        cache->Clear();for(std::uint64_t i=0;i<16;++i){const auto read=cache->Get({i});Require(i<8?read.isPending:(!read.isPending&&read.isThrottled),"backpressure is distinct from an admitted pending read");}
        unsigned inFlight=0;while(Drain(queue))++inFlight;
        Require(inFlight==8&&cache->GetCounters().entries<=16,"in-flight decoding queue and decoded directory are bounded");
        cache->Get({30});cache->Clear();const auto before=cache->GetCounters().batchesRead;
        while(Drain(queue)){}
        Require(cache->GetCounters().batchesRead==before,"expired display request cannot republish old geometry");
        cache.reset();
        {
            const auto abandonedQueue=std::make_shared<FeatureInternal::ReadTaskQueue>();
            const auto abandonedCache=std::make_shared<FeatureInternal::SpatialMeshCache>(source,abandonedQueue,1);
            for(std::uint64_t i=0;i<64;++i){
                {const auto request=abandonedCache->Get({i});Require(request.isPending,"cancel test page was not admitted");}
                {const std::lock_guard<std::mutex> lock(abandonedQueue->mutex);abandonedQueue->CancelLocked();}
                Require(abandonedCache->GetCounters().entries<=1,"cancelled pages outside the next view remain permanently pending in LRU");
            }
            Require(abandonedCache->Get({64}).isPending&&Drain(abandonedQueue)&&abandonedCache->Get({64}).mesh
                &&abandonedCache->GetCounters().batchesRead==1,"a new view completes after repeatedly cancelling disjoint pages");
        }
        {
            DataArray<std::uint64_t> interleaved;interleaved.reserve(batches*Source::facesPerBatch*3);
            for(std::size_t i=0;i<batches*Source::facesPerBatch;++i)for(unsigned corner=0;corner<3;++corner)interleaved.push_back((i%batches)*3+corner);
            auto shuffledMesh=std::make_shared<const SurfaceMeshPayload>(source->GetMesh().GetVertices(),std::move(interleaved));
            const auto* formal=shuffledMesh->GetTriangles().data();const auto shuffled=Source::Build(shuffledMesh);
            const auto local=shuffled->Find(Box(selected*10-.1,selected*10+1.1),4);
            Require(local.isComplete&&local.batches==query.batches&&local.visitedNodes<32,"interleaved formal faces still produce genuinely spatial batches");
            const auto geometry=shuffled->Read(selected);auto* exact=vtkIdTypeArray::SafeDownCast(geometry.mesh->GetCellData()->GetArray(Source::faceIds));
            Require(geometry.sourceFacesRead==Source::facesPerBatch&&geometry.sourcePointsRead==3&&exact,"spatial reorder reads only local geometry");
            for(vtkIdType i=0;i<exact->GetNumberOfValues();++i)Require(exact->GetValue(i)==selected+batches*i,"display permutation preserves stable original face IDs");
            const auto object=shuffled->Find(Box(-1,2000),1,17,18);
            Require(object.isComplete&&object.batches==std::vector<std::uint64_t>{17}&&object.indexRecordsRead==1,"small object lookup uses one inverse record rather than scanning full face order");
            const auto one=shuffled->Read(17,{},17,18);Require(one.sourceFacesRead==1&&one.sourcePointsRead==3,"single-object display range keeps exact formal membership after reordering");
            Require(formal==shuffledMesh->GetTriangles().data()&& &shuffled->GetMesh()==shuffledMesh.get(),"display indexing never replaces the formal mesh arrays");
            for(std::size_t i=0;i<shuffledMesh->GetTriangles().size();++i)Require(shuffledMesh->GetTriangles()[i]==((i/3)%batches)*3+i%3,"formal face order remains unchanged");
        }
        source.reset();Require(weak.expired()&&batch.mesh->GetNumberOfCells()==Source::facesPerBatch,"returned batch owns bounded geometry independently");
        auto empty=Source::Build(std::make_shared<const SurfaceMeshPayload>(DataArray<double>{},DataArray<std::uint64_t>{}));
        Require(empty->GetBatchCount()==0&&empty->Find(Box(-1,1),8).batches.empty(),"empty formal mesh is a complete empty display");
        const auto compact=std::make_shared<const SurfaceMeshPayload>(DataArray<double>{0,0,0,1,0,0,0,1,0,0,0,1},
            DataArray<std::uint64_t>{0,1,2,0,2,3,0,3,1,1,3,2,2,1,0,3,2,0});
        for(const std::size_t page:{1,2,3,64,16384}) {
            const auto split=Source::Build(compact,{},page);std::array<bool,6> seen{};
            const auto all=split->Find({},64);Require(all.isComplete,"all display page sizes keep the complete formal face set");
            for(const auto id:all.batches) {
                const auto chunk=split->Read(id);auto* faceIds=vtkIdTypeArray::SafeDownCast(chunk.mesh->GetCellData()->GetArray(Source::faceIds));
                auto* pointIds=vtkIdTypeArray::SafeDownCast(chunk.mesh->GetPointData()->GetArray(Source::pointIds));
                Require(chunk.sourceFacesRead<=page&&faceIds&&pointIds,"display workspace follows configured page size");
                for(vtkIdType i=0;i<faceIds->GetNumberOfValues();++i){const auto formal=static_cast<std::size_t>(faceIds->GetValue(i));Require(formal<seen.size()&&!seen[formal],"cross-page formal face identity is unique");seen[formal]=true;
                    vtkIdType count=0;const vtkIdType* corners=nullptr;chunk.mesh->GetPolys()->GetCellAtId(i,count,corners);Require(count==3,"page size preserves triangles");
                    for(unsigned c=0;c<3;++c)Require(static_cast<std::uint64_t>(pointIds->GetValue(corners[c]))==compact->GetTriangles()[3*formal+c],"page size never changes formal connectivity");}
            }
            Require(std::all_of(seen.begin(),seen.end(),[](bool value){return value;}),"page size never loses formal faces");
        }
        const auto maximum=std::numeric_limits<double>::max();
        const auto extreme=Source::Build(std::make_shared<const SurfaceMeshPayload>(
            DataArray<double>{maximum,0,0,maximum*(1-1e-15),1,0,maximum*(1-2e-15),0,1},DataArray<std::uint64_t>{0,1,2}));
        Require(extreme->Find({},1).batches.size()==1&&extreme->Read(0).sourceFacesRead==1,"finite extreme coordinates produce no invalid Morton conversion");
        std::cout<<"Spatial display: faces="<<batches*Source::facesPerBatch<<" queryNodes="<<query.visitedNodes<<" readFaces="<<batch.sourceFacesRead<<" readPoints="<<batch.sourcePointsRead<<'\n';
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
