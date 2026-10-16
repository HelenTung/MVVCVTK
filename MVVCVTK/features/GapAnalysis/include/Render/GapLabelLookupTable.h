#pragma once

#include "Data/DataPayloads.h"
#include "Render/Support/AnalysisColorStyle.h"
#include <vtkLookupTable.h>
#include <vtkObjectFactory.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

// 共享、按正式编号排序的紧凑目录。不为全体对象生成颜色项、注释文字或哈希节点。
using GapRegionVolumes = DataArray<std::pair<std::int32_t, double>>;

class GapLabelLookupTable final : public vtkLookupTable {
public:
    static GapLabelLookupTable* New();
    vtkTypeMacro(GapLabelLookupTable, vtkLookupTable);
    void SetOnlyRegion(std::optional<std::int32_t> id){m_onlyRegion=id;this->Modified();}
    void SetBoundaryRegions(std::shared_ptr<const DataArray<std::int32_t>> regions)
    {
        m_boundaryRegions=std::move(regions);this->Modified();
    }

    void SetRegions(std::shared_ptr<const GapRegionVolumes> regions,
        std::array<double, 2> range, AnalysisColorStyle::RampParams ramp,
        vtkSmartPointer<vtkLookupTable> initialColors = {})
    {
        m_regions=std::move(regions);m_range=range;m_ramp=std::move(ramp);
        m_onlyRegion.reset();
        m_initialColors=std::move(initialColors);
        this->SetNumberOfTableValues(1);
        this->SetTableValue(0,0,0,0,0);
        this->SetNanColor(.5,.5,.5,1);
        this->Modified();
    }
    void DeepCopy(vtkScalarsToColors* source) override
    {
        this->Superclass::DeepCopy(source);
        if(auto* labels=GapLabelLookupTable::SafeDownCast(source)) {
            m_regions=labels->m_regions;m_range=labels->m_range;m_ramp=labels->m_ramp;
            m_boundaryRegions=labels->m_boundaryRegions;
            m_initialColors=labels->m_initialColors;m_onlyRegion=labels->m_onlyRegion;
        }else {m_regions.reset();m_boundaryRegions.reset();m_initialColors=nullptr;m_onlyRegion.reset();}
    }
    const unsigned char* MapValue(double value) override
    {
        // VTK 可并行映射切片；返回缓冲按线程隔离，目录和配色均只读。
        thread_local std::array<unsigned char,4> color;
        const auto invalid=[&]() -> const unsigned char* {
            this->GetColorAsUnsignedChars(this->GetNanColor(),color.data());return color.data();
        };
        if(!std::isfinite(value)||value<std::numeric_limits<std::int32_t>::min()
            ||value>std::numeric_limits<std::int32_t>::max()||std::trunc(value)!=value)
            return invalid();
        const auto id=static_cast<std::int32_t>(value);
        if(id==0)return this->GetPointer(0);
        if(m_onlyRegion&&id!=*m_onlyRegion)return this->GetPointer(0);
        if(!m_regions)return invalid();
        const auto found=std::lower_bound(m_regions->begin(),m_regions->end(),id,
            [](const auto& entry,std::int32_t key){return entry.first<key;});
        if(found==m_regions->end()||found->first!=id)return invalid();
        if(m_boundaryRegions && std::binary_search(m_boundaryRegions->begin(),m_boundaryRegions->end(),id)) {
            color={255,166,0,255};return color.data();
        }
        if(m_initialColors) {
            // 初次显示保持既有 256 项体积色表的量化规则。
            std::copy_n(m_initialColors->MapValue(found->second),4,color.begin());
        }else {
            const auto rgba=AnalysisColorStyle::GetMappedColor(m_range,m_ramp,found->second);
            for(int c=0;c<4;++c)color[c]=static_cast<unsigned char>(255*rgba[c]+.5);
        }
        return color.data();
    }
    void GetColor(double value,double rgb[3]) override
    {
        const auto* color=MapValue(value);
        for(int c=0;c<3;++c)rgb[c]=color[c]/255.;
    }
    double GetOpacity(double value) override {return MapValue(value)[3]/255.;}
    void MapScalarsThroughTable2(void* input,unsigned char* output,int type,
        int count,int increment,int format) override
    {
        if(format<VTK_LUMINANCE||format>VTK_RGBA) {
            this->Superclass::MapScalarsThroughTable2(input,output,type,count,increment,format);return;
        }
        switch(type) {
            vtkTemplateMacro(MapRows(static_cast<const VTK_TT*>(input),output,count,increment,format));
        default:this->Superclass::MapScalarsThroughTable2(input,output,type,count,increment,format);break;
        }
    }

protected:
    GapLabelLookupTable()=default;
    ~GapLabelLookupTable() override=default;
private:
    template<class Value>
    void MapRows(const Value* input,unsigned char* output,int count,int increment,int format)
    {
        const auto alpha=this->GetAlpha();
        bool hasLast=false;double lastValue=0;std::array<unsigned char,4> cached;
        for(int i=0;i<count;++i,input+=increment,output+=format) {
            const double value=static_cast<double>(*input);
            if(!hasLast||value!=lastValue) {
                std::copy_n(MapValue(value),4,cached.begin());hasLast=true;lastValue=value;
            }
            const auto* color=cached.data();
            if(format==VTK_RGB||format==VTK_RGBA) {
                std::copy_n(color,3,output);
                if(format==VTK_RGBA)output[3]=static_cast<unsigned char>(color[3]*alpha+.5);
            }else {
                output[0]=static_cast<unsigned char>(color[0]*.30+color[1]*.59+color[2]*.11+.5);
                if(format==VTK_LUMINANCE_ALPHA)output[1]=static_cast<unsigned char>(color[3]*alpha+.5);
            }
        }
    }
    std::shared_ptr<const GapRegionVolumes> m_regions;
    std::shared_ptr<const DataArray<std::int32_t>> m_boundaryRegions;
    std::array<double,2> m_range{0,0};
    AnalysisColorStyle::RampParams m_ramp;
    vtkSmartPointer<vtkLookupTable> m_initialColors;
    std::optional<std::int32_t> m_onlyRegion;
};

inline vtkStandardNewMacro(GapLabelLookupTable);
