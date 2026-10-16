// 私有协议负例、有效域统计与 owner 生命周期；不包含供应商类型。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "GapKernelBridge.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <vector>

namespace {
struct Batch final {
    GapKernelHeader header{};
    std::vector<GapKernelRegion> regions;
    std::vector<std::int32_t> labels;
    void* owner=nullptr;
    GapKernelLabelOwnerRelease release=nullptr;
    int calls=0;
    ~Batch(){if(owner && release)release(owner);}
};
std::int32_t MVVCVTK_GAP_KERNEL_CALL Receive(const GapKernelResultView* view,void* context) noexcept {
    try {
        auto& batch=*static_cast<Batch*>(context);++batch.calls;batch.header=*view->header;
        if(view->regionCount)batch.regions.assign(view->regions,view->regions+view->regionCount);
        batch.labels.assign(view->labels,view->labels+view->labelCount);
        batch.owner=view->cloneLabelOwner(view->labelOwner);batch.release=view->releaseLabelOwner;
        return batch.owner ? 1:0;
    }catch(...){return 0;}
}
}
int GetGapMaskBridgeFailCount() {
    int failures=0;
    const auto expect=[&](bool condition,const char* text){if(!condition){++failures;std::cerr<<"FAILED: "<<text<<'\n';}};
    std::array<wchar_t,32768> executable{};const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if(!length || length>=executable.size())return 1;
    auto directory=std::filesystem::path(executable.data()).parent_path();
    std::array<wchar_t,32768> runtime{};const auto runtimeLength=GetEnvironmentVariableW(L"MVVCVTK_GAP_RUNTIME_DIR",runtime.data(),static_cast<DWORD>(runtime.size()));
    if(runtimeLength && runtimeLength<runtime.size())directory=runtime.data();
    auto module=LoadLibraryExW((directory/L"GapKernel.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module)return 1;
    const auto entry=reinterpret_cast<GapKernelEntry>(GetProcAddress(module,"BuildGapResult"));
    if(!entry){FreeLibrary(module);return 1;}
    {
        constexpr std::size_t count=1331;
        std::vector<float> values(count,100);std::vector<std::uint8_t> mask(count,255);
        const auto index=[](int x,int y,int z){return x+11*(y+11*z);};
        int ordinal=0;
        // 13 个 0 与 14 个 10，共 27 个孔体素；0 是合法背景下界，不能被样本均值裁掉。
        for(int z=4;z<=6;++z)for(int y=4;y<=6;++y)for(int x=2;x<=4;++x)values[index(x,y,z)]=ordinal++<13?0.f:10.f;
        for(int z=4;z<=6;++z)for(int y=4;y<=6;++y)for(int x=7;x<=8;++x)mask[index(x,y,z)]=0;
        GapKernelRequest request{};request.abiVersion=GapKernelAbiVersion;request.structSize=static_cast<std::uint32_t>(sizeof(request));
        request.voxelData=values.data();request.voxelCount=count;request.validityMask=mask.data();request.validityCount=count;
        for(int a=0;a<3;++a){request.dims[a]=11;request.extent[a*2]=0;request.extent[a*2+1]=10;request.direction[a*3+a]=1;}
        request.spacing[0]=2;request.spacing[1]=.5;request.spacing[2]=3;
        request.backgroundMean=0;request.materialMean=100;request.isoThreshold=50;request.numberRuns=1;
        std::vector<int> first;
        for(int variant=0;variant<2;++variant) {
            for(std::size_t i=0;i<count;++i)if(!mask[i])values[i]=variant?std::numeric_limits<float>::quiet_NaN():-1000000.f;
            const auto before=values;Batch result;
            expect(entry(&request,&Receive,&result)==1 && result.calls==1,"masked private result consumed exactly once");
            expect(result.regions.size()==1 && result.regions[0].voxelCount==27 && result.regions[0].boundaryStatus==0,
                "explicit recipe keeps all 27 independently defined valid pore voxels");
            expect(std::count_if(result.labels.begin(),result.labels.end(),[](int value){return value>0;})==27,"private labels match valid pore set");
            bool positions=result.labels.size()==count;
            if(positions)for(std::size_t i=0;i<count;++i) {
                const auto x=i%11,y=(i/11)%11,z=i/121;
                const bool expected=x>=2 && x<=4 && y>=4 && y<=6 && z>=4 && z<=6;
                if((result.labels[i]>0)!=expected)positions=false;
            }
            expect(positions,"private labels match every independently defined pore position");
            expect(result.header.computedThreshold==50,"explicit iso remains the valid-domain detection threshold");
            expect(std::abs(result.header.materialMean-100)<1e-6 && result.header.materialStd==0
                && std::abs(result.header.airMean-140.f/27)<1e-6,"header material and background moments exclude invalid values and fill");
            expect(std::abs(result.header.defectVolumeMM3-81)<1e-6,"27 voxels times 3 mm3 is 81 mm3");
            expect(std::memcmp(before.data(),values.data(),count*sizeof(float))==0,"private bridge preserves source including invalid NaN bits");
            if(!variant)first=result.labels;else expect(first==result.labels,"invalid gray values do not change valid labels");
            std::cout<<"GAP_MASK_BRIDGE variant="<<variant<<" air_mean="<<result.header.airMean
                <<" computed_threshold="<<result.header.computedThreshold<<" voxels="<<(result.regions.empty()?0:result.regions[0].voxelCount)<<'\n';
        }
        {auto invalid=request;invalid.validityCount=0;Batch result;expect(entry(&invalid,&Receive,&result)==0 && result.calls==0,"mismatched mask shape rejected before result");}
        {auto invalid=request;invalid.abiVersion=GapKernelAbiVersion-1;Batch result;expect(entry(&invalid,&Receive,&result)==0 && result.calls==0,"old private protocol rejected");}
        {auto cancelled=request;cancelled.getCancelled=[](void*) noexcept -> std::int32_t{return 1;};Batch result;expect(entry(&cancelled,&Receive,&result)==0 && result.calls==0,"pre-cancelled private operation publishes nothing");}
        {
            constexpr std::size_t largeCount=21*21*21;
            std::vector<float> large(largeCount,100);std::vector<std::uint8_t> valid(largeCount,255);valid[0]=0;
            // 19^3=6859 个连通孔体素，跨越遍历的4096体素取消检查点。
            for(int z=1;z<=19;++z)for(int y=1;y<=19;++y)for(int x=1;x<=19;++x)large[x+21*(y+21*z)]=0;
            auto cancelled=request;cancelled.voxelData=large.data();cancelled.validityMask=valid.data();
            cancelled.voxelCount=largeCount;cancelled.validityCount=largeCount;
            for(int a=0;a<3;++a){cancelled.dims[a]=21;cancelled.extent[a*2+1]=20;}
            // 入口1、三块统计/填值各3、DefX后1、三块标签校验3、种子扫描1、组件起点1，
            // 第14次查询发生在该组件已访问4096体素时；故必须丢弃已有的派生片段。
            int polls=0;cancelled.cancelContext=&polls;
            cancelled.getCancelled=[](void* context) noexcept -> std::int32_t{return ++*static_cast<int*>(context)>13?1:0;};
            Batch result;expect(entry(&cancelled,&Receive,&result)==0 && result.calls==0 && polls==14,
                "mid-component cancellation publishes no partially relabeled result");
        }
        {Batch recovered;expect(entry(&request,&Receive,&recovered)==1 && recovered.labels==first,"private result recovers after cancellation with owner retained through callback return");}
    }
    FreeLibrary(module);return failures;
}
