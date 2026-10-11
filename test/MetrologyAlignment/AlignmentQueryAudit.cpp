// 查询热点审计：真实 STL 拓扑与显式模拟偏移，仅比较后端，不宣称 CT 名义精度。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include "AlignmentReference.h"
#include "AlignmentMeshQuery.h"
#include "AlignmentCudaQuery.h"
#include <opencv2/core.hpp>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>

namespace {
using Clock=std::chrono::steady_clock;
double Ms(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
void Require(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
}
int wmain(int argc,wchar_t **argv) {
    try {
        Require(argc==3 || argc==4,"Usage: query-audit STL output.json [query-count]");
        AlignmentWork work;work.cancelled=std::make_shared<std::atomic<bool>>(false);
        AlignmentReferenceRequest input;input.filePath=std::filesystem::path(argv[1]).u8string();input.frameId="query-audit";input.coordinateFrame="RAS";
        const auto importStart=Clock::now();auto imported=AlignmentReferenceLoader::Load(input,work);
        Require(imported.status==AlignmentStatus::FullyDetermined && imported.mesh,"STL import failed");
        const double importMs=Ms(importStart);
        const auto &vertices=imported.mesh->GetVertices();const auto available=vertices.size()/3;
        const auto count=std::min(available,argc==4 ? static_cast<std::size_t>(std::stoull(argv[3])) : std::size_t{131072});
        Require(count>0 && count<=static_cast<std::size_t>(INT_MAX),"Invalid audit query count");
        std::vector<AlignmentPoint> points(count);
        for(std::size_t i=0;i<count;++i) {
            const auto index=i*(available/count);
            points[i]={vertices[3*index]+.013,vertices[3*index+1]+.017,vertices[3*index+2]+.019};
        }
        const auto indexStart=Clock::now();AlignmentMeshQuery query(imported.mesh,work);const double indexMs=Ms(indexStart);
        std::vector<AlignmentMeshHit> serial(count),parallel(count),hybrid(count);
        const auto cpu=[&](bool isParallel) {
            if(!isParallel)for(std::size_t i=0;i<count;++i)serial[i]=query.GetClosest(points[i]);
            else cv::parallel_for_(cv::Range(0,static_cast<int>(count)),[&](const cv::Range &range) {
                for(int i=range.start;i<range.end;++i)parallel[static_cast<std::size_t>(i)]=query.GetClosest(points[static_cast<std::size_t>(i)]);
            });
        };
        std::string device;const auto initStart=Clock::now();const bool hasDevice=AlignmentCudaQuery::GetAvailable(device);
        Require(hasDevice,device.c_str());const double initMs=Ms(initStart);
        constexpr std::size_t capacity=8192;
        const auto uploadStart=Clock::now();auto gpu=query.BuildCudaQuery(capacity);const double uploadMs=Ms(uploadStart);
        std::vector<AlignmentCudaHit> hits(capacity);std::size_t verificationCount=0;
        const auto hybridRun=[&] {
          verificationCount=0;
          for(std::size_t base=0;base<count;base+=capacity) {
            const auto size=std::min(capacity,count-base);gpu->Query(points[base].data(),size,hits.data());
            for(std::size_t j=0;j<size;++j) { Require(hits[j].error==0,"CUDA query failed");verificationCount+=hits[j].requiresVerification; }
            cv::parallel_for_(cv::Range(0,static_cast<int>(size)),[&](const cv::Range &range) {
                for(int j=range.start;j<range.end;++j) {
                    const auto offset=static_cast<std::size_t>(j),i=base+offset;
                    hybrid[i]=hits[offset].requiresVerification ? query.GetClosest(points[i]) : query.GetHit(points[i],hits[offset].triangle);
                }
            });
          }
        };
        // 每种后端紧邻一次完整预热后计时，三轮观察波动；不以最快一次抵消慢运行。
        std::array<std::array<double,6>,3> rounds{};
        double error=0;std::size_t mismatch=0;
        for(auto &round:rounds) {
          for(int mode=0;mode<3;++mode) {
            const auto run=[&] { if(mode==2)hybridRun();else cpu(mode==1); };
            auto start=Clock::now();run();round[static_cast<std::size_t>(mode)]=Ms(start);
            start=Clock::now();run();round[static_cast<std::size_t>(mode+3)]=Ms(start);
          }
          for(std::size_t i=0;i<count;++i) {
            Require(serial[i].triangle==parallel[i].triangle && serial[i].distanceSquared==parallel[i].distanceSquared,"CPU scheduling changed a query");
            error=std::max(error,std::abs(serial[i].distanceSquared-hybrid[i].distanceSquared));
            if(serial[i].triangle!=hybrid[i].triangle || serial[i].point!=hybrid[i].point || serial[i].normal!=hybrid[i].normal)++mismatch;
          }
        }
        const auto median=[&](std::size_t column) { std::array<double,3> times{rounds[0][column],rounds[1][column],rounds[2][column]};std::sort(times.begin(),times.end());return times[1]; };
        const double serialMs=median(3),parallelMs=median(4),gpuMs=median(5);
        Require(error==0 && mismatch==0,"Hybrid query differs from canonical CPU result");
        PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
        Require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory)),"Memory counters unavailable");
        std::ofstream out(std::filesystem::path(argv[2]),std::ios::binary);
        out<<std::setprecision(12)<<"{\n\"scope\":\"real STL geometry, synthetic offsets; query hotspot only, no CT accuracy claim\",\n"
           <<"\"status\":\"PASS\",\"sourceSha256\":"<<std::quoted(imported.metadata.sha256)
           <<",\"device\":"<<std::quoted(device)<<",\"points\":"<<available<<",\"triangles\":"<<imported.mesh->GetTriangles().size()/3
           <<",\"queries\":"<<count<<",\"cpuThreads\":"<<cv::getNumThreads()<<",\"importMs\":"<<importMs
           <<",\"cpuIndexMs\":"<<indexMs<<",\"serialQueryMs\":"<<serialMs<<",\"parallelQueryMs\":"<<parallelMs
           <<",\"cudaInitializeMs\":"<<initMs<<",\"cudaUploadMs\":"<<uploadMs<<",\"hybridQueryMs\":"<<gpuMs
           <<",\"timingStatistic\":\"median of three timed rounds; one immediate full warmup per backend per round\",\"roundsMs\":[";
        for(std::size_t i=0;i<rounds.size();++i) { if(i)out<<',';out<<'[';for(std::size_t j=0;j<6;++j){if(j)out<<',';out<<rounds[i][j];}out<<']'; }
        out<<']'
           <<",\"cpuVerificationCount\":"<<verificationCount<<",\"mismatchedQueries\":"<<mismatch<<",\"maxSquaredDistanceError\":"<<error
           <<",\"processPeakWorkingSetBytes\":"<<memory.PeakWorkingSetSize<<",\"processPeakCommitBytes\":"<<memory.PeakPagefileUsage<<"\n}\n";
        Require(bool(out),"Audit output write failed");
        std::cout<<"QUERY_AUDIT count="<<count<<" cpu_serial_ms="<<serialMs<<" cpu_parallel_ms="<<parallelMs<<" hybrid_ms="<<gpuMs<<'\n';
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
