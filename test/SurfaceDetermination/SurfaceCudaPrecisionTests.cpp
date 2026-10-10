#include "SurfaceCudaCompute.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
#include <future>
#include <tuple>
#include <string>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {
class GpuProfileSetting final {
public:
    GpuProfileSetting() {
        const auto size=GetEnvironmentVariableA("MVVCVTK_SURFACE_GPU_PROFILE",nullptr,0);
        if(size){m_previous.resize(size);GetEnvironmentVariableA("MVVCVTK_SURFACE_GPU_PROFILE",m_previous.data(),size);
            m_previous.resize(size-1);m_present=true;}
    }
    ~GpuProfileSetting(){SetEnvironmentVariableA("MVVCVTK_SURFACE_GPU_PROFILE",m_present?m_previous.c_str():nullptr);}
    void Set(bool enabled){SetEnvironmentVariableA("MVVCVTK_SURFACE_GPU_PROFILE",enabled?"1":"0");}
private:
    std::string m_previous;
    bool m_present=false;
};
bool EqualSample(const SurfaceCudaSample&a,const SurfaceCudaSample&b) {
    return std::tie(a.gradient[0],a.gradient[1],a.gradient[2],a.magnitude,a.gradientValid,a.support)==
           std::tie(b.gradient[0],b.gradient[1],b.gradient[2],b.magnitude,b.gradientValid,b.support);
}
}

int main() {
    GpuProfileSetting profiling;profiling.Set(false);
    SurfaceCudaVolume v;
    const int extent[6]{-3, 12, 2, 16, -4, 7};
    std::copy_n(extent, 6, v.extent);
    v.dimensions[0] = 16;
    v.dimensions[1] = 15;
    v.dimensions[2] = 12;
    v.scalarBytes = 8;
    v.origin[0] = 11.1;
    v.origin[1] = -4.6;
    v.origin[2] = 7;
    // 非零 extent、旋转和各向异性；解析灰度梯度为 (1,2,4)。
    v.modelToIndex[1] = .5;
    v.modelToIndex[3] = -1;
    v.modelToIndex[8] = 2;
    std::vector<double> values(16U * 15U * 12U);
    for (int z = -4; z <= 7; ++z)
        for (int y = 2; y <= 16; ++y)
            for (int x = -3; x <= 12; ++x)
                values[(std::size_t(z + 4) * 15 + (y - 2)) * 16 + (x + 3)] = x + 2. * y + 4. * z;
    v.values = values.data();
    constexpr auto hostCredit = std::max<std::size_t>(MVVCVTK_SURFACE_TEST_HOST_BYTES,
                                                     MVVCVTK_SURFACE_TEST_DEVICE_BYTES);
    constexpr auto testCredit = std::max<std::size_t>(1024U * 1024U * 1024U, hostCredit);
    SurfaceCudaCompute limited;
    if (limited.Start(v, MVVCVTK_SURFACE_TEST_HOST_BYTES - 1U, 4U * 1024U * 1024U))
        return 1;
    const std::array<double, 6> points{1.5, 5, 2, 2.5, 7, 4};
    SurfaceCudaCompute compute;
    if (!compute.Start(v, testCredit, 4U * 1024U * 1024U)) {
        std::cerr << "CUDA device/backend unavailable\n";
        return 77;
    }
    std::array<SurfaceCudaSample, 2> samples;
    if (!compute.Read(points.data(), samples.size(), samples.data()))
        return 1;
    for (const auto &sample : samples) {
        if (sample.support != 1 || sample.gradientValid != 1 || sample.gradient[0] != -2 ||
            sample.gradient[1] != .5 || sample.gradient[2] != 8 || sample.magnitude != std::sqrt(68.25))
            return 1;
    }
    if(compute.GetProfile().requested)return 1;
    {
        profiling.Set(true);
        SurfaceCudaCompute traced;
        std::array<SurfaceCudaSample,2> profiled;
        if(!traced.Start(v,testCredit,4U*1024U*1024U) || traced.GetProfile().complete ||
            !traced.Read(points.data(),profiled.size(),profiled.data()) ||
            !std::equal(samples.begin(),samples.end(),profiled.begin(),EqualSample))return 1;
        const auto trace=traced.GetProfile();
        if(!trace.requested || !trace.eventsAvailable || !trace.complete || trace.failed ||
            trace.submittedBatchCount!=1 || trace.measuredBatchCount!=1 || trace.measuredPointCount!=2 ||
            trace.slabUploadCount!=1 || !trace.slabUploadBytes || trace.eventSetupMs<0 ||
            !std::isfinite(trace.pointUploadMs) || trace.pointUploadMs<0 ||
            !std::isfinite(trace.kernelMs) || trace.kernelMs<0 ||
            !std::isfinite(trace.sampleDownloadMs) || trace.sampleDownloadMs<0)return 1;
        for(unsigned fault=4;fault<=6;++fault){
            SurfaceCudaCompute diagnosticFailure;
            if(!diagnosticFailure.Start(v,testCredit,4U*1024U*1024U) || !diagnosticFailure.SetTestFault(fault) ||
                !diagnosticFailure.Read(points.data(),profiled.size(),profiled.data()) ||
                !diagnosticFailure.GetActive() ||
                !std::equal(samples.begin(),samples.end(),profiled.begin(),EqualSample))return 1;
            const auto failed=diagnosticFailure.GetProfile();
            if(!failed.requested || !failed.failed || failed.complete || failed.submittedBatchCount!=1 ||
                failed.measuredBatchCount || (fault==4 && failed.eventsAvailable))return 1;
            diagnosticFailure.Reset();
            if(diagnosticFailure.GetProfile().requested)return 1;
        }
    }
    // 非线性解析场使两批结果不同，能识别异步缓冲串批、覆写与重载错误。
    for(bool enabled:{false,true}) {
        profiling.Set(enabled);
        auto input=v;
        std::vector<double> quadratic(values.size());
        for(int z=-4;z<=7;++z)for(int y=2;y<=16;++y)for(int x=-3;x<=12;++x)
            quadratic[(std::size_t(z+4)*15+(y-2))*16+(x+3)]=x*x+2.*y*y+4.*z*z;
        input.values=quadratic.data();
        SurfaceCudaCompute pipeline;
        const auto deviceLimit=surfaceCudaHostBufferBytes+4U*16U*15U*sizeof(double);
        if(!pipeline.Start(input,testCredit,deviceLimit)||pipeline.GetDeviceBytes()>deviceLimit) return 1;
        const auto fill=[&](unsigned slot,bool upper,std::size_t count) {
            auto *p=pipeline.Prepare(slot);if(!p)return false;
            for(std::size_t i=0;i<count;++i){p[3*i]=upper?2.5:1.5;p[3*i+1]=upper?7:5;p[3*i+2]=upper?6.5:-3.5;}
            return true;
        };
        const auto verify=[&](const SurfaceCudaTicket &ticket,bool upper) {
            const auto *p=pipeline.Collect(ticket);if(!p)return false;
            const double gx=upper?-28:-20,gy=upper?2.5:1.5,gz=upper?104:-56;
            for(std::size_t i=0;i<ticket.count;++i)
                if(p[i].support!=1||!p[i].gradientValid||p[i].gradient[0]!=gx||p[i].gradient[1]!=gy||
                   p[i].gradient[2]!=gz||p[i].magnitude!=std::sqrt(gx*gx+gy*gy+gz*gz))return false;
            return true;
        };
        SurfaceCudaTicket a,b;
        if(!fill(0,false,surfaceCudaBatchCapacity)||!pipeline.Submit(0,surfaceCudaBatchCapacity,a)||
           !fill(1,true,surfaceCudaBatchCapacity)||!pipeline.Submit(1,surfaceCudaBatchCapacity,b))return 1;
        auto wrong=b;--wrong.count;
        if(pipeline.Collect(wrong)||pipeline.Prepare(0))return 1;
        if(enabled && pipeline.GetProfile().complete)return 1;
        if(!verify(b,true)||!verify(a,false)||pipeline.Collect(a))return 1;
        if(enabled){
            const auto trace=pipeline.GetProfile();
            if(!trace.complete || trace.measuredBatchCount!=2 || trace.submittedBatchCount!=2 ||
                trace.measuredPointCount!=2*surfaceCudaBatchCapacity || trace.slabUploadCount!=2 ||
                std::async(std::launch::async,[&]{return pipeline.GetProfile().requested;}).get())return 1;
        }
        // 小 slab 只提交可容纳的连续前缀，后半段随后重载，不漏点。
        auto *p=pipeline.Prepare(0);if(!p)return 1;
        const double split[]{1.5,5,-3.5,2.5,7,6.5};std::copy_n(split,6,p);
        SurfaceCudaTicket prefix;
        if(!pipeline.Submit(0,2,prefix)||prefix.count!=1||!verify(prefix,false)||
           !fill(0,true,1)||!pipeline.Submit(0,1,b)||pipeline.Collect(prefix)||!verify(b,true))return 1;
        if(std::async(std::launch::async,[&]{return pipeline.Prepare(0);}).get())return 1;
        // Reset 必须等待 pending 工作；旧 token 在重启后也不能收集新批次。
        if(!fill(0,false,1)||!pipeline.Submit(0,1,a))return 1;
        pipeline.Reset();
        if(pipeline.GetProfile().requested||pipeline.Collect(a)||!pipeline.Start(input,testCredit,deviceLimit)||!fill(0,true,1)||
           !pipeline.Submit(0,1,b)||pipeline.Collect(a)||!verify(b,true))return 1;
        for(unsigned fault=1;fault<=3;++fault) {
            if(!fill(0,false,1)||!pipeline.SetTestFault(fault))return 1;
            const bool submitted=pipeline.Submit(0,1,a);
            if(fault==1 ? submitted : !submitted)return 1;
            if(fault==2 && pipeline.Collect(a))return 1;
            pipeline.Reset();
            if(!pipeline.Start(input,testCredit,deviceLimit)||!fill(0,true,1)||
               !pipeline.Submit(0,1,b)||!verify(b,true))return 1;
        }
    }
    profiling.Set(false);
    // 饱和发生在源节点；全饱和线性场应得到零梯度。
    v.hasMaterialRange = 1;
    v.materialRange[0] = -100;
    v.materialRange[1] = -50;
    SurfaceCudaCompute saturation;
    if (!saturation.Start(v, testCredit, 4U * 1024U * 1024U) ||
        !saturation.Read(points.data(), samples.size(), samples.data()))
        return 1;
    for (const auto &sample : samples)
        if (sample.gradientValid || sample.magnitude != 0 || sample.support != 1)
            return 1;
    // 缺失节点不能被饱和处理改成有效支持。
    std::vector<unsigned char> mask(values.size(), 0);
    v.validity = mask.data();
    SurfaceCudaCompute missing;
    if (!missing.Start(v, testCredit, 4U * 1024U * 1024U) ||
        !missing.Read(points.data(), samples.size(), samples.data()))
        return 1;
    for (const auto &sample : samples)
        if (sample.support != 2 || sample.gradientValid)
            return 1;
    // float32 输入保持原类型；精确线性场应给出同一解析梯度。
    std::vector<float> floats(values.size());
    std::transform(values.begin(), values.end(), floats.begin(),
                   [](double x) { return static_cast<float>(x); });
    v.values = floats.data();
    v.validity = nullptr;
    v.scalarBytes = 4;
    v.hasMaterialRange = 0;
    SurfaceCudaCompute single;
    if (!single.Start(v, testCredit, 4U * 1024U * 1024U) ||
        !single.Read(points.data(), samples.size(), samples.data()))
        return 1;
    for (const auto &sample : samples)
        if (sample.support != 1 || !sample.gradientValid || sample.gradient[0] != -2 ||
            sample.gradient[1] != .5 || sample.gradient[2] != 8)
            return 1;
    if (single.Read(points.data(), surfaceCudaBatchCapacity + 1, samples.data()))
        return 1;
    single.Reset();
    floats[(6U * 15U + 3U) * 16U + 4U] = std::numeric_limits<float>::quiet_NaN();
    SurfaceCudaCompute nonFinite;
    if (!nonFinite.Start(v, testCredit, 4U * 1024U * 1024U) ||
        !nonFinite.Read(points.data(), samples.size(), samples.data()) || samples[0].gradientValid)
        return 1;
    std::cout << "CUDA analytic gradients, node saturation and missing support PASS; buffers="
              << compute.GetDeviceBytes() << '\n';
    return 0;
}
