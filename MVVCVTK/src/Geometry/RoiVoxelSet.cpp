#include "Data/RoiTypes.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include <utility>

class RoiVoxelBuilder::Impl final {
public:
    enum class Input { Empty,Runs,Voxels,Failed,Finished };
    Input input=Input::Empty;
    std::vector<RoiVoxelRun> runs;
    std::map<std::pair<int,int>,std::map<int,RoiVoxelRun>> rows;
    void Select(Input type){
        if(input!=Input::Empty&&input!=type)throw std::logic_error("ROI builder input mode or lifetime mismatch.");
        input=type;
    }
};
RoiVoxelBuilder::RoiVoxelBuilder():m_impl(std::make_unique<Impl>()){}
RoiVoxelBuilder::~RoiVoxelBuilder()=default;
void RoiVoxelBuilder::AppendRuns(const std::vector<RoiVoxelRun>& values){
    try{
        m_impl->Select(Impl::Input::Runs);
        for(const auto& run:values){
            if(!run.label||run.first[0]>run.last)throw std::invalid_argument("Invalid ROI run.");
            if(!m_impl->runs.empty()){
                auto& previous=m_impl->runs.back();
                if(std::tie(run.first[1],run.first[2],run.first[0])<std::tie(previous.first[1],previous.first[2],previous.first[0]))throw std::invalid_argument("Unordered ROI runs.");
                if(previous.first[1]==run.first[1]&&previous.first[2]==run.first[2]){
                    if(previous.last>=run.first[0])throw std::invalid_argument("Overlapping ROI runs.");
                    if(previous.label==run.label&&std::int64_t(previous.last)+1==run.first[0]){previous.last=run.last;continue;}
                }
            }
            m_impl->runs.push_back(run);
        }
    }catch(...){m_impl->input=Impl::Input::Failed;throw;}
}
void RoiVoxelBuilder::AppendVoxels(const std::vector<RoiVoxel>& values){
    try{
        m_impl->Select(Impl::Input::Voxels);
        for(const auto& voxel:values){
            if(!voxel.label)throw std::invalid_argument("Zero ROI voxel label.");
            auto& row=m_impl->rows[{voxel.index[1],voxel.index[2]}];auto after=row.upper_bound(voxel.index[0]);
            auto current=row.end();
            if(after!=row.begin()){
                auto before=std::prev(after);
                if(before->second.last>=voxel.index[0])throw std::invalid_argument("Duplicate ROI voxel.");
                if(before->second.label==voxel.label&&std::int64_t(before->second.last)+1==voxel.index[0]){before->second.last=voxel.index[0];current=before;}
            }
            if(current==row.end())current=row.emplace(voxel.index[0],RoiVoxelRun{voxel.index,voxel.index[0],voxel.label}).first;
            if(after!=row.end()&&after->second.label==voxel.label&&std::int64_t(current->second.last)+1==after->first){current->second.last=after->second.last;row.erase(after);}
        }
    }catch(...){m_impl->input=Impl::Input::Failed;throw;}
}
std::shared_ptr<const RoiVoxelSet> RoiVoxelBuilder::Finish(){
    if(m_impl->input==Impl::Input::Failed||m_impl->input==Impl::Input::Finished)throw std::logic_error("ROI builder cannot publish.");
    try{
        // 无序输入的索引逐行释放，最终区间数组只构建一次。
        while(!m_impl->rows.empty()){
            auto row=m_impl->rows.begin();for(const auto& item:row->second)m_impl->runs.push_back(item.second);m_impl->rows.erase(row);
        }
        auto result=std::make_shared<const RoiVoxelSet>(std::move(m_impl->runs));m_impl->input=Impl::Input::Finished;return result;
    }catch(...){m_impl->input=Impl::Input::Failed;throw;}
}

class RoiVoxelSet::Impl final {
public:
    std::vector<RoiVoxelRun> runs;
    std::vector<std::size_t> offsets{0};
    std::map<std::uint32_t,std::array<double,6>> bounds;
};
RoiVoxelSet::RoiVoxelSet(std::vector<RoiVoxelRun> runs):m_impl(std::make_shared<Impl>()) {
    const auto ordered=[](const auto& a,const auto& b){return std::tie(a.first[1],a.first[2],a.first[0])<std::tie(b.first[1],b.first[2],b.first[0]);};
    if(!std::is_sorted(runs.begin(),runs.end(),ordered))std::sort(runs.begin(),runs.end(),ordered);
    std::size_t written=0;
    for(const auto run:runs) {
        if(!run.label||run.first[0]>run.last)throw std::invalid_argument("Invalid ROI voxel run.");
        if(written) {
            auto& previous=runs[written-1];
            if(previous.first[1]==run.first[1]&&previous.first[2]==run.first[2]) {
                if(previous.last>=run.first[0])throw std::invalid_argument("Overlapping ROI voxel runs.");
                if(previous.label==run.label&&std::int64_t(previous.last)+1==run.first[0]){previous.last=run.last;continue;}
            }
        }
        runs[written++]=run;
    }
    runs.resize(written);m_impl->runs=std::move(runs);m_impl->offsets.reserve(written+1);
    for(const auto& run:m_impl->runs) {
        const auto count=static_cast<std::size_t>(std::int64_t(run.last)-run.first[0]+1);
        if(count>std::numeric_limits<std::size_t>::max()-m_impl->offsets.back())throw std::length_error("ROI voxel count overflows.");
        m_impl->offsets.push_back(m_impl->offsets.back()+count);
        auto& bounds=m_impl->bounds.emplace(run.label,std::array<double,6>{INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY}).first->second;
        for(int a=0;a<3;++a){bounds[a*2]=std::min(bounds[a*2],run.first[a]-.5);bounds[a*2+1]=std::max(bounds[a*2+1],(a==0?run.last:run.first[a])+.5);}
    }
}
const std::vector<RoiVoxelRun>& RoiVoxelSet::GetRuns()const noexcept{return m_impl->runs;}
std::size_t RoiVoxelSet::GetVoxelCount()const noexcept{return m_impl->offsets.back();}
std::vector<RoiVoxel> RoiVoxelSet::GetVoxelChunk(std::size_t offset,std::size_t count)const {
    if(offset>GetVoxelCount())throw std::out_of_range("ROI voxel chunk offset.");
    count=std::min(count,GetVoxelCount()-offset);std::vector<RoiVoxel> result;result.reserve(count);
    auto run=static_cast<std::size_t>(std::upper_bound(m_impl->offsets.begin(),m_impl->offsets.end(),offset)-m_impl->offsets.begin()-1);
    while(result.size()<count) {
        const auto& span=m_impl->runs[run];const auto first=std::int64_t(span.first[0])+static_cast<std::int64_t>(offset-m_impl->offsets[run]);
        const auto take=std::min(count-result.size(),m_impl->offsets[run+1]-offset);
        for(std::size_t i=0;i<take;++i)result.push_back({{static_cast<int>(first+static_cast<std::int64_t>(i)),span.first[1],span.first[2]},span.label});
        offset+=take;++run;
    }
    return result;
}
std::uint32_t RoiVoxelSet::GetLabel(const std::array<int,3>& index) const noexcept
{
    const auto found=std::upper_bound(m_impl->runs.begin(),m_impl->runs.end(),index,
        [](const auto& key,const auto& run){return std::tie(key[1],key[2],key[0])<std::tie(run.first[1],run.first[2],run.first[0]);});
    if(found==m_impl->runs.begin())return 0;
    const auto& run=*std::prev(found);
    return run.first[1]==index[1]&&run.first[2]==index[2]&&index[0]<=run.last?run.label:0;
}

std::array<double,6> RoiVoxelSet::GetIndexBounds(const std::vector<std::uint64_t>& labels) const noexcept
{
    std::array<double,6> result{INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
    const auto add=[&](const auto& bounds){for(int a=0;a<3;++a){result[a*2]=std::min(result[a*2],bounds[a*2]);result[a*2+1]=std::max(result[a*2+1],bounds[a*2+1]);}};
    if(labels.empty())for(const auto& entry:m_impl->bounds)add(entry.second);
    else for(auto label:labels){if(label>UINT32_MAX)continue;const auto found=m_impl->bounds.find(static_cast<std::uint32_t>(label));if(found!=m_impl->bounds.end())add(found->second);}
    return result;
}
