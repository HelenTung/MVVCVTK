#include "Data/DataPayloads.h"
#include "PagedArray.h"
#include <iostream>
#include <future>
#include <stdexcept>

bool GetMeshPagedStorageTests()
{
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    try {
        MeshReadArray<double> frozen;
        {PagedArray<double> values;
            for(std::size_t i=0;i<100000;++i)values.push_back(double(i)*.25);
            auto copy=values;copy[0]=-1;require(values[0]==0,"paged writable copy changed its source");
            frozen=values.Finish();require(values.empty(),"published buffer retained a writable owner");}
        const auto read=[&]{return frozen.GetChunk(32765,11);};
        auto left=std::async(std::launch::async,read),right=std::async(std::launch::async,read);
        const auto a=left.get(),b=right.get();
        require(a.error==MeshReadError::None && a.values.size()==11 && a.nextOffset==32776 && a.values==b.values,"concurrent blocks changed values or continuation");
        for(std::size_t i=0;i<a.values.size();++i)require(a.values[i]==double(32765+i)*.25,"block boundary changed a value");
        require(frozen.GetChunk(100001,1).error==MeshReadError::InvalidRange,"invalid range became an empty mesh");
        const auto cancelled=frozen.GetChunk(0,100,[]{return true;});
        require(cancelled.error==MeshReadError::Cancelled && cancelled.values.empty() && !cancelled.isComplete,"cancelled read published partial values");
        const auto tail=frozen.GetChunk(99999,100);require(tail.values.size()==1 && tail.isComplete && tail.nextOffset==100000,"tail did not stop at exact count");
        {PagedArray<std::uint64_t> values{7};values.resize(values.capacity(),7);values.push_back(values.front());
            require(values.back()==7,"growth lost an aliased input value");
            while(values.size()<65536/sizeof(std::uint64_t)+1)values.push_back(values.front());
            require(values.back()==7,"small-to-file spill lost an aliased input value");}
        {PagedArray<double> values;values.reserve(2000000);
            for(std::size_t i=0;i<2000000;++i)values.push_back(double(i)*.25);
            const auto large=values.Finish();const auto chunk=large.GetChunk(0,SIZE_MAX);
            require(chunk.error==MeshReadError::None && chunk.values.size()*sizeof(double)<=imageChunkLimit
                && !chunk.isComplete && chunk.nextOffset==chunk.values.size(),"large read exceeded the SPI block bound");
            for(std::size_t i=0;i<large.size();i+=8191)require(large[i]==double(i)*.25,"page eviction changed stored geometry");
            const auto sparse=[&] {
                for(std::size_t i=0;i<12000;++i)for(const auto base:{std::size_t(0),std::size_t(900000),std::size_t(1900000)}) {
                    const auto index=base+i%1000;
                    if(large[index]!=double(index)*.25)return false;
                    if(!(i%257)) {const auto page=large.GetChunk(index,8193);
                        if(page.error!=MeshReadError::None || page.values.size()!=8193)return false;
                        for(std::size_t j=0;j<page.values.size();++j)if(page.values[j]!=double(index+j)*.25)return false;}
                }
                return true;
            };
            auto first=std::async(std::launch::async,sparse),second=std::async(std::launch::async,sparse);
            require(first.get()&&second.get(),"distant concurrent point reads changed cached values or block boundaries");
            // 跨整个映射交错读取，验证无共享窗口的并发读取和转换字段仍保持原值。
            const auto converted=ConvertMeshArray<double>(large,3,[](double value,std::size_t component){return value+double(component);});
            const auto windows=[&](bool reverse) {
                const auto pages=large.size()/8192;
                for(std::size_t round=0;round<2;++round)for(std::size_t page=0;page<pages;++page) {
                    const auto selected=reverse?pages-1-page:page;
                    const auto index=selected*8192+8190;
                    const auto block=converted.GetChunk(index*3+2,7);
                    if(block.error!=MeshReadError::None || block.values.size()!=7)return false;
                    for(std::size_t j=0;j<block.values.size();++j){
                        const auto offset=index*3+2+j;
                        if(block.values[j]!=double(offset/3)*.25+double(offset%3))return false;
                    }
                }
                return true;
            };
            auto forward=std::async(std::launch::async,[&]{return windows(false);});
            auto backward=std::async(std::launch::async,[&]{return windows(true);});
            require(forward.get()&&backward.get(),"frozen mapped-page eviction changed shared converted field values");
            const auto bound=2000000*sizeof(double);
            require(large.GetResidentBytes()<=bound && converted.GetResidentBytes()<=bound,
                "mapped residency exceeded its allocated storage");}
        {const std::size_t count=2000000;PagedArray<std::uint64_t> values(count,7);
            std::vector<std::uint64_t> expected(count,7);
            for(std::size_t i=0;i<4000;++i){const auto index=(i*7919)%count;values[index]=i;expected[index]=i;
                if(i==2000)values.ReleasePages();}
            values.ReleasePages();const auto snapshot=values.Finish();
            for(std::size_t offset=0;offset<count;){const auto block=snapshot.GetChunk(offset,count-offset);
                require(block.error==MeshReadError::None && !block.values.empty(),"released writable mapping could not be read");
                require(std::equal(block.values.begin(),block.values.end(),expected.begin()+offset),"batch release changed random writes");
                offset=block.nextOffset;}}
        {PagedMap<std::array<std::uint64_t,2>,std::uint64_t,PagedArrayHash<std::uint64_t,2>> lookup;
            for(std::uint64_t i=0;i<5000;++i)require(lookup.Insert({i,i+1},i*3)==i*3,"lookup insert changed point identity");
            for(std::uint64_t i=0;i<5000;++i)require(lookup.Find({i,i+1})==i*3 && lookup.Insert({i,i+1},9)==i*3,"lookup growth changed deduplication");
            lookup.clear();require(!lookup.Find({0,1}),"cleared component retained an earlier object's point");}
        {PagedArray<std::uint64_t> values;
            for(std::uint64_t i=0;i<10001;++i)values.push_back((10000-i)%103);
            SortPagedArray(values,std::less<std::uint64_t>{},{},128);
            require(std::is_sorted(values.begin(),values.end()) && values.front()==0 && values.back()==102,"external run merge changed edge ordering");}
        PagedArray<double> points{1000000000.0,0,0,1000000000.0001,1,0,1000000000.0,0,1};
        PagedArray<std::uint64_t> triangles{0,1,2};PagedArray<double> flags{1};
        auto mesh=MeshPayloadBuilder::Build(points.Finish(),triangles.Finish(),{},"LPS",{{"measurement.valid",1,flags.Finish(),0}});
        require(mesh && mesh->GetValid() && mesh->GetPointValues().size()==9 && mesh->GetTriangleIndices()[2]==2
            && mesh->GetCellFields().front().values[0]==1 && mesh->GetCoordinateFrame()=="LPS","streamed geometry or qualification changed");
        const auto snapshot=std::dynamic_pointer_cast<const SurfaceMeshPayload>(mesh->CreateSnapshot());
        require(snapshot && snapshot->GetPointValues()[3]==1000000000.0001 && &snapshot->GetPointValues()==&mesh->GetPointValues(),"snapshot lost double precision or shared owner");
        class Broken final:public MeshValueSource<double>{public:std::size_t GetCount()const noexcept override{return 3;}void ReadValues(std::size_t,std::size_t,double*)const override{throw std::runtime_error("injected read failure");}};
        MeshReadArray<double> broken(std::make_shared<const Broken>());
        require(broken.GetChunk(0,3).error==MeshReadError::ReadFailed,"I/O failure became successful geometry");
        std::cout<<"PASS: immutable paged geometry, exact blocks, qualifiers and cancellation\n";return true;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return false;}
}
