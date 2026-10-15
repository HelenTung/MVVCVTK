#pragma once
#include "FeatureSupport/SpatialMeshSource.h"
#include "FeatureSupport/ReadTaskQueue.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace FeatureInternal {
// 一个正式结果共用有限解码缓存；每个视图只保留自己的活动批次。
// 队列和渲染器由 Feature 提供，本类不拥有线程、窗口或业务对象目录。
class SpatialMeshCache final : public std::enable_shared_from_this<SpatialMeshCache> {
public:
    struct Key final {
        std::uint64_t batch=0,firstFace=0,endFace=UINT64_MAX;
        bool operator<(const Key& other) const {return std::tie(batch,firstFace,endFace)<std::tie(other.batch,other.firstFace,other.endFace);}
    };
    struct Read final {
        vtkSmartPointer<vtkPolyData> mesh;std::string error;bool isPending=false,isThrottled=false;
        std::uint64_t styleVersion=0;
        // 等待接纳的视图保留 ticket；LRU 不撤销仍有接收方的读取或就绪页。
        std::shared_ptr<const void> ticket;
    };
    struct Counters final {std::uint64_t batchesRead=0,pointsRead=0,facesRead=0,indexRecordsRead=0,cacheHits=0;std::size_t entries=0;};
    using Decorate=std::function<void(vtkPolyData&,const SpatialMeshSource&,const ReadTaskQueue::Cancel&)>;
    SpatialMeshCache(std::shared_ptr<const SpatialMeshSource> source,std::shared_ptr<ReadTaskQueue> queue,
        std::size_t capacity=128,Decorate decorate={})
        :m_source(std::move(source)),m_queue(std::move(queue)),m_capacity(std::max(std::size_t{1},capacity)),m_decorate(std::move(decorate))
    {
        if(!m_source||!m_queue)throw std::invalid_argument("Display cache requires a source and an existing worker queue.");
    }
    const SpatialMeshSource& GetSource() const noexcept {return *m_source;}
    const std::shared_ptr<const SpatialMeshSource>& GetSourceOwner() const noexcept {return m_source;}
    const std::shared_ptr<ReadTaskQueue>& GetQueue() const noexcept {return m_queue;}
    std::uint64_t GetRevision() const noexcept {return m_revision.load(std::memory_order_acquire);}
    std::uint64_t GetStyleVersion() const {const std::lock_guard<std::mutex> lock(m_mutex);return m_styleVersion;}
    Counters GetCounters() const {const std::lock_guard<std::mutex> lock(m_mutex);auto result=m_counters;result.entries=m_entries.size();return result;}

    // owner 调用；返回已有批次或提交有限读取，绝不在本调用栈解码几何。
    Read Get(const Key& key,vtkSmartPointer<vtkPolyData> heldGeometry={})
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        auto found=m_entries.find(key);
        if(found!=m_entries.end()) {
            found->second->used=++m_clock;
            if(found->second->mesh){++m_counters.cacheHits;return {found->second->mesh,{},false,false,m_styleVersion,found->second};}
            if(!found->second->error.empty())return {{},found->second->error,false,false,m_styleVersion,found->second};
            if(found->second->isPending&&found->second->queueToken&&!found->second->queueToken->load(std::memory_order_acquire))return {{},{},true,false,m_styleVersion,found->second};
            found->second->isPending=false;
        }
        if(found==m_entries.end()) {
            while(m_entries.size()>=m_capacity) {
                auto oldest=m_entries.end();
                for(auto i=m_entries.begin();i!=m_entries.end();++i) {
                    const auto& token=i->second->queueToken;
                    const bool abandoned=i->second->isPending&&token&&token->load(std::memory_order_acquire);
                    if(i->second.use_count()==1&&(!i->second->isPending||abandoned)&&(oldest==m_entries.end()||i->second->used<oldest->second->used))oldest=i;
                }
                if(oldest==m_entries.end())break;
                m_entries.erase(oldest);
            }
            found=m_entries.emplace(key,std::make_shared<Entry>()).first;
        }
        const auto entry=found->second;entry->used=++m_clock;entry->isPending=true;
        if(!entry->previous&&m_reuseGeometry)entry->previous=std::move(heldGeometry);
        const auto weak=weak_from_this();const auto version=m_styleVersion;
        const std::weak_ptr<Entry> weakEntry=entry;
        std::shared_ptr<std::atomic<bool>> admitted;
        try {
            admitted=m_queue->Submit([weak,weakEntry,key,version](const auto& stopped){
                const auto self=weak.lock();const auto pending=weakEntry.lock();
                if(!self||!pending)return;
                const auto cancel=[&]{return stopped()||pending->cancelled.load(std::memory_order_acquire);};
                vtkSmartPointer<vtkPolyData> mesh;SpatialMeshSource::Batch decoded;std::string error;
                try {
                    if(cancel())return;
                    Decorate decorate;vtkSmartPointer<vtkPolyData> previous;
                    {const std::lock_guard<std::mutex> lock(self->m_mutex);decorate=self->m_decorate;previous=pending->previous;}
                    if(previous){mesh=vtkSmartPointer<vtkPolyData>::New();mesh->ShallowCopy(previous);}
                    else {decoded=self->m_source->Read(key.batch,cancel,key.firstFace,key.endFace);mesh=decoded.mesh;}
                    if(decorate)decorate(*mesh,*self->m_source,cancel);
                    if(cancel())return;
                }catch(const std::exception& failure){if(cancel())return;error=failure.what();mesh=nullptr;}
                catch(...){if(cancel())return;error="Display batch preparation failed.";mesh=nullptr;}
                const std::lock_guard<std::mutex> lock(self->m_mutex);
                const auto current=self->m_entries.find(key);
                if(version!=self->m_styleVersion||current==self->m_entries.end()||current->second!=pending||cancel())return;
                pending->mesh=std::move(mesh);pending->previous=nullptr;pending->error=std::move(error);pending->isPending=false;
                if(decoded.mesh){++self->m_counters.batchesRead;self->m_counters.pointsRead+=decoded.sourcePointsRead;self->m_counters.facesRead+=decoded.sourceFacesRead;self->m_counters.indexRecordsRead+=decoded.sourceIndexRecordsRead;}
                self->m_revision.fetch_add(1,std::memory_order_release);
            });
        }catch(...){entry->isPending=false;throw;}
        entry->queueToken=std::move(admitted);
        if(!entry->queueToken) {
            entry->isPending=false;
            m_entries.erase(found);
            return m_queue->GetIsStopping()?Read{{},"Display reader has stopped.",false,false}:Read{{},{},false,true};
        }
        return {{},{},true,false,m_styleVersion,entry};
    }

    // 色带变更只重建有界缓存的显示属性；正式几何及已解码 points/cells 继续共享。
    void SetDecoration(Decorate decorate,bool reuseGeometry=true)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::map<Key,std::shared_ptr<Entry>> replacement;
        for(auto& item:m_entries) {
            const auto old=item.second;
            auto next=std::make_shared<Entry>();if(reuseGeometry)next->previous=old->mesh?old->mesh:old->previous;next->used=old->used;replacement.emplace(item.first,std::move(next));
        }
        for(auto& item:m_entries)item.second->cancelled.store(true,std::memory_order_release);
        m_entries.swap(replacement);++m_styleVersion;m_reuseGeometry=reuseGeometry;m_decorate=std::move(decorate);
        m_revision.fetch_add(1,std::memory_order_release);
    }
    void Clear() noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);for(auto& item:m_entries)item.second->cancelled.store(true,std::memory_order_release);
        m_entries.clear();++m_styleVersion;m_reuseGeometry=false;m_revision.fetch_add(1,std::memory_order_release);
    }
private:
    struct Entry final {vtkSmartPointer<vtkPolyData> mesh,previous;std::string error;std::uint64_t used=0;bool isPending=false;std::atomic<bool> cancelled{false};std::shared_ptr<std::atomic<bool>> queueToken;};
    std::shared_ptr<const SpatialMeshSource> m_source;
    std::shared_ptr<ReadTaskQueue> m_queue;
    const std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::map<Key,std::shared_ptr<Entry>> m_entries;
    Decorate m_decorate;
    Counters m_counters;
    std::uint64_t m_clock=0,m_styleVersion=0;
    bool m_reuseGeometry=true;
    std::atomic<std::uint64_t> m_revision{1};
};
} // namespace FeatureInternal
