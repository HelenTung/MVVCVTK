#pragma once
#include "AnalysisColorStyle.h"
#include <vtkActor2D.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkPropCollection.h>
#include <vtkTextMapper.h>
#include <vtkUnsignedCharArray.h>
#include <vtkViewport.h>
#include <vtkWindow.h>
#include <cstdio>

// 有限数值轴与无穷端段共用条带；尺寸随视窗缩放，不伪造无穷区间的数值跨度。
class AnalysisLegend final : public vtkScalarBarActor {
public:
    static AnalysisLegend* New() { return new AnalysisLegend; }
    vtkTypeMacro(AnalysisLegend,vtkScalarBarActor);
    void SetMapping(const std::array<double,2>& range,AnalysisColorStyle::RampParams params,bool hasValues=true) {
        AnalysisColorStyle::SetTailColors(params);m_range=range;m_params=std::move(params);m_hasValues=hasValues;Modified();
    }
    const std::array<double,2>& GetMappingRange() const { return m_range; }
    const AnalysisColorStyle::RampParams& GetMapping() const { return m_params; }
    void GetActors2D(vtkPropCollection* props) override {
        if(!m_hasValues)return;
        props->AddItem(m_bar);for(auto& t:m_text)props->AddItem(t);
    }
    int RenderOpaqueGeometry(vtkViewport* v) override {
        if(!m_hasValues)return 0;
        const auto* size=v->GetSize();
        if(size[0]<=0||size[1]<=0)return 0;
        if(NeedsBuild(v))Build(v);
        int rendered=m_bar->RenderOpaqueGeometry(v);
        for(auto& t:m_text)rendered+=t->RenderOpaqueGeometry(v);
        return rendered;
    }
    int RenderTranslucentPolygonalGeometry(vtkViewport*) override { return 0; }
    int RenderOverlay(vtkViewport* v) override {
        if(!m_hasValues)return 0;
        const auto* size=v->GetSize();
        if(size[0]<=0||size[1]<=0)return 0;
        if(NeedsBuild(v))Build(v);
        int rendered=m_bar->RenderOverlay(v);
        for(auto& t:m_text)rendered+=t->RenderOverlay(v);
        return rendered;
    }
    void ReleaseGraphicsResources(vtkWindow* w) override {
        Superclass::ReleaseGraphicsResources(w);m_bar->ReleaseGraphicsResources(w);
        for(auto& t:m_text)t->ReleaseGraphicsResources(w);
    }
private:
    AnalysisLegend():m_bar(vtkSmartPointer<vtkActor2D>::New()) { m_bar->PickableOff();m_bar->UseBoundsOff();UseBoundsOff(); }
    bool NeedsBuild(vtkViewport* v) {
        const auto* size=v->GetSize();
        return m_stamp<GetMTime()||m_size!=std::array<int,2>{size[0],size[1]}||m_dpi!=v->GetVTKWindow()->GetDPI();
    }
    void Build(vtkViewport* v) {
        const auto* size=v->GetSize();m_size={size[0],size[1]};m_text.clear();
        m_dpi=v->GetVTKWindow()->GetDPI();const double dpiScale=std::max(1,m_dpi)/72.;
        const bool compact=size[1]/dpiScale<150;
        const double scale=std::max(.01,std::min(size[0]/200.,size[1]/(compact?96.:300.)));
        const int font=std::max(1,int(std::lround((compact?9:11)*scale/dpiScale)));
        const double x=GetPosition()[0]*size[0],y=compact?16*scale:std::max(18*scale,GetPosition()[1]*size[1]);
        const double height=compact?std::max(24*scale,std::min(220*scale,size[0]-x-100*scale))
            :std::max(12*scale,std::min({GetHeight()*size[1],GetMaximumHeightInPixels()*scale,size[1]-y-29*scale}));
        const double origin=compact?x:y;
        const double tail=std::min(8*scale,height*.12),finiteHeight=height-2*tail,barWidth=(compact?8:12)*scale;
        auto points=vtkSmartPointer<vtkPoints>::New();auto cells=vtkSmartPointer<vtkCellArray>::New();
        auto colors=vtkSmartPointer<vtkUnsignedCharArray>::New();colors->SetNumberOfComponents(4);
        const auto text=[&](const std::string& value,double tx,double ty,int alignment=VTK_TEXT_LEFT) {
            auto t=vtkSmartPointer<vtkActor2D>::New();auto mapper=vtkSmartPointer<vtkTextMapper>::New();
            mapper->SetInput(value.c_str());mapper->GetTextProperty()->ShallowCopy(GetLabelTextProperty());
            mapper->GetTextProperty()->SetFontSize(font);mapper->GetTextProperty()->SetJustification(alignment);
            mapper->GetTextProperty()->SetVerticalJustificationToBottom();t->SetMapper(mapper);t->SetPosition(tx,ty);
            t->PickableOff();t->UseBoundsOff();m_text.push_back(t);
        };
        const auto quad=[&](double low,double high,double acrossLow,double acrossHigh,const std::array<double,4>& color) {
            const auto point=[&](double across,double along){return compact?points->InsertNextPoint(along,y+across,0):points->InsertNextPoint(x+across,along,0);};
            vtkIdType ids[4]{point(acrossLow,low),point(acrossHigh,low),point(acrossHigh,high),point(acrossLow,high)};
            cells->InsertNextCell(4,ids);unsigned char rgba[4];
            for(int k=0;k<4;++k)rgba[k]=static_cast<unsigned char>(std::lround(255*std::clamp(color[k],0.,1.)));
            colors->InsertNextTypedTuple(rgba);
        };
        const auto box=[&](double low,double high,const std::array<double,4>& color){quad(low,high,0,barWidth,color);};
        const auto low=AnalysisColorStyle::GetMappedColor(m_range,m_params,m_range[0]);
        const auto high=AnalysisColorStyle::GetMappedColor(m_range,m_params,m_range[1]);
        const long double span=std::max(static_cast<long double>(m_range[1])-m_range[0],
            std::max(1.L,std::max(std::abs(static_cast<long double>(m_range[0])),std::abs(static_cast<long double>(m_range[1]))))*16*std::numeric_limits<double>::epsilon());
        for(int i=0;i<8;++i) {
            const long double f=(i+.5L)/8;
            // 两个短端段压缩无穷区间，仍逐点调用同一映射；不另造颜色或强制平滑。
            const double below=static_cast<double>(m_range[0]-span*(1-f)/f),above=static_cast<double>(m_range[1]+span*f/(1-f));
            const auto a=std::isfinite(below)?AnalysisColorStyle::GetMappedColor(m_range,m_params,below)
                :std::array<double,4>{m_params.belowColor[0],m_params.belowColor[1],m_params.belowColor[2],low[3]};
            const auto b=std::isfinite(above)?AnalysisColorStyle::GetMappedColor(m_range,m_params,above)
                :std::array<double,4>{m_params.aboveColor[0],m_params.aboveColor[1],m_params.aboveColor[2],high[3]};
            box(origin+tail*i/8,origin+tail*(i+1)/8,a);
            box(origin+height-tail+tail*i/8,origin+height-tail+tail*(i+1)/8,b);
        }
        std::vector<double> bounds{m_range[0],m_range[1]};
        for(const auto& segment:m_params.segments)for(auto b:{segment.lower,segment.upper})
            if(b&&*b>m_range[0]&&*b<m_range[1])bounds.push_back(*b);
        if(m_params.tolerance)for(double b:*m_params.tolerance)if(b>m_range[0]&&b<m_range[1])bounds.push_back(b);
        std::sort(bounds.begin(),bounds.end());bounds.erase(std::unique(bounds.begin(),bounds.end()),bounds.end());
        if(bounds.size()==1)box(origin+tail,origin+height-tail,low);
        else for(std::size_t i=1;i<bounds.size();++i)for(int j=0;j<64;++j) {
            const double a=(1-j/64.)*bounds[i-1]+(j/64.)*bounds[i],b=(1-(j+1)/64.)*bounds[i-1]+((j+1)/64.)*bounds[i];
            box(origin+tail+finiteHeight*AnalysisColorStyle::GetFraction(m_range[0],m_range[1],a),
                origin+tail+finiteHeight*AnalysisColorStyle::GetFraction(m_range[0],m_range[1],b),
                AnalysisColorStyle::GetMappedColor(m_range,m_params,.5*a+.5*b));
        }
        const auto labels=AnalysisColorStyle::BuildLabels(m_range);const auto format=AnalysisColorStyle::GetLabelFormat(*labels);
        std::vector<std::string> values;int textWidth=0,textHeight=0;
        for(vtkIdType i=0;i<labels->GetNumberOfValues();++i){
            char value[64];std::snprintf(value,sizeof(value),format.c_str(),labels->GetValue(i)==0?0:labels->GetValue(i));values.emplace_back(value);
            vtkNew<vtkTextMapper> measure;measure->SetInput(value);measure->GetTextProperty()->ShallowCopy(GetLabelTextProperty());
            measure->GetTextProperty()->SetFontSize(font);int bounds[2]{};measure->GetSize(v,bounds);
            textWidth=std::max(textWidth,bounds[0]);textHeight=std::max(textHeight,bounds[1]);
        }
        const int last=int(values.size())-1;int stride=1;
        const double spacing=compact?textWidth+6*scale:textHeight+2*scale;
        while(stride<last&&finiteHeight*stride/last<spacing)stride*=2;
        std::string previous;
        for(int i=0;i<=last;++i){
            const double along=last?origin+tail+finiteHeight*i/last:origin+height/2;
            const double length=(i==0||i==last||i==8?5:i%4==0?4:2)*scale;
            quad(along-.5*scale,along+.5*scale,compact?-length:barWidth+scale,compact?-scale:barWidth+scale+length,{1,1,1,1});
            if((i%stride==0||i==last)&&values[i]!=previous){
                if(compact)text(values[i],along,y+barWidth+3*scale,i==0?VTK_TEXT_LEFT:i==last?VTK_TEXT_RIGHT:VTK_TEXT_CENTERED);
                else text(values[i],x+barWidth+8*scale,along-textHeight/2.);
                previous=values[i];
            }
        }
        if(compact){
            // 矮切片视窗横排，在下沿容纳数值和单位，给左上场景坐标留空间。
            text(u8"−∞",x,scale);text(u8"+∞",x+height,scale,VTK_TEXT_RIGHT);
            text(GetTitle()?GetTitle():"",x+height/2,y+barWidth+textHeight+8*scale,VTK_TEXT_CENTERED);
        }else{
            text(GetTitle()?GetTitle():"",x,y+height+textHeight+5*scale);
            text(u8"−∞",x+barWidth+8*scale,y-textHeight-3*scale);
            text(u8"+∞",x+barWidth+8*scale,y+height+2*scale);
        }
        auto data=vtkSmartPointer<vtkPolyData>::New();data->SetPoints(points);data->SetPolys(cells);data->GetCellData()->SetScalars(colors);
        auto mapper=vtkSmartPointer<vtkPolyDataMapper2D>::New();mapper->SetInputData(data);mapper->SetColorModeToDirectScalars();m_bar->SetMapper(mapper);m_stamp=GetMTime();
    }
    std::array<double,2> m_range{};AnalysisColorStyle::RampParams m_params;bool m_hasValues=true;
    vtkMTimeType m_stamp=0;std::array<int,2> m_size{};int m_dpi=0;vtkSmartPointer<vtkActor2D> m_bar;
    std::vector<vtkSmartPointer<vtkActor2D>> m_text;
};
