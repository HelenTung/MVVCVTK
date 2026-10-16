#pragma once
#include "Data/DataPayloads.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#include <list>
#include <unordered_map>
#include <atomic>
#include <cstring>
#include <initializer_list>

namespace MeshStorage {
// 仅仓内：可写阶段由算法拥有。文件跟随最后一个只读快照关闭，不暴露临时路径。
class File final {
public:
    File() {
        wchar_t directory[MAX_PATH+1]{},name[MAX_PATH+1]{};
        const auto count=GetTempPathW(MAX_PATH,directory);
        if(!count || count>MAX_PATH || !GetTempFileNameW(directory,L"msh",0,name))throw std::runtime_error("Cannot create mesh storage.");
        m_file=CreateFileW(name,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(m_file==INVALID_HANDLE_VALUE){DeleteFileW(name);throw std::runtime_error("Cannot open mesh storage.");}
    }
    ~File(){CloseMap();if(m_file!=INVALID_HANDLE_VALUE)CloseHandle(m_file);}
    File(const File&)=delete;File& operator=(const File&)=delete;
    std::size_t GetCapacity()const noexcept{return m_capacity;}
    void* GetData()const noexcept{return m_data;}
    void Reserve(std::size_t bytes) {
        if(m_frozen)throw std::logic_error("Published mesh storage is immutable.");
        if(bytes<=m_capacity)return;
        if(bytes>std::size_t(INT64_MAX))throw std::length_error("Mesh file size overflows.");
        const auto growth=std::max<std::size_t>(65536,m_capacity<=std::size_t(INT64_MAX)/2?m_capacity*2:bytes);
        const auto capacity=std::max(bytes,growth);
        if(capacity>std::size_t(INT64_MAX))throw std::length_error("Mesh file size overflows.");
        CloseMap();LARGE_INTEGER end{};end.QuadPart=static_cast<LONGLONG>(capacity);
        FILE_ALLOCATION_INFO allocation{};allocation.AllocationSize=end;
        if(!SetFileInformationByHandle(m_file,FileAllocationInfo,&allocation,sizeof(allocation)))throw std::runtime_error("Mesh storage space reservation failed.");
        if(!SetFilePointerEx(m_file,end,nullptr,FILE_BEGIN)||!SetEndOfFile(m_file))throw std::runtime_error("Mesh storage allocation failed.");
        m_mapping=CreateFileMappingW(m_file,nullptr,PAGE_READWRITE,0,0,nullptr);
        if(!m_mapping)throw std::runtime_error("Mesh storage mapping failed.");
        m_data=MapViewOfFile(m_mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,capacity);
        if(!m_data){CloseHandle(m_mapping);m_mapping=nullptr;throw std::runtime_error("Mesh storage view failed.");}
        m_capacity=capacity;
    }
    void Touch(std::size_t byte)const {
        const auto block=byte/pageBytes;
        if(m_last.load(std::memory_order_relaxed)==block)return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto found=m_recent.find(block);
        if(found!=m_recent.end())m_order.splice(m_order.begin(),m_order,found->second);
        else {
            m_order.push_front(block);
            try {m_recent.emplace(block,m_order.begin());}
            catch(...) {m_order.pop_front();throw;}
        }
        m_last.store(block,std::memory_order_relaxed);
        if(m_order.size()>residentPages){const auto old=m_order.back();m_order.pop_back();m_recent.erase(old);Trim(old*pageBytes,std::min(pageBytes,m_capacity-old*pageBytes));}
    }
    void Release()const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if(m_data && !m_frozen && !FlushViewOfFile(m_data,m_capacity))throw std::runtime_error("Mesh storage flush failed.");
        if(m_data)VirtualUnlock(m_data,m_capacity);
        m_order.clear();m_recent.clear();m_last.store(SIZE_MAX,std::memory_order_relaxed);
    }
    // 写入者必须先结束全部算法任务。冻结后旧的私有写指针也不能修改已发布数据。
    void Freeze() {
        if(m_frozen)return;
        Release();DWORD previous=0;
        if(m_data && !VirtualProtect(m_data,m_capacity,PAGE_READONLY,&previous))throw std::runtime_error("Mesh storage freeze failed.");
        m_frozen=true;
    }
    void Copy(std::size_t offset,std::size_t bytes,void* destination)const {
        if(offset>m_capacity || bytes>m_capacity-offset)throw std::out_of_range("Mesh storage read is outside the file.");
        if(!bytes)return;
        if(!CopyBytes(static_cast<const unsigned char*>(m_data)+offset,destination,bytes))throw std::runtime_error("Mesh storage read failed.");
        VirtualUnlock(static_cast<unsigned char*>(m_data)+offset,bytes);
    }
    void CopyWindow(std::size_t offset,std::size_t bytes,void* destination)const {
        if(offset>m_capacity || bytes>m_capacity-offset)throw std::out_of_range("Mesh storage read is outside the file.");
        if(!bytes)return;
        for(auto byte=offset;byte<offset+bytes;byte=(byte/pageBytes+1)*pageBytes)Touch(byte);
        if(!CopyBytes(static_cast<const unsigned char*>(m_data)+offset,destination,bytes))throw std::runtime_error("Mesh storage read failed.");
    }
    std::size_t GetResidentBound()const noexcept{return std::min(m_capacity,residentPages*pageBytes);}
private:
    static bool CopyBytes(const void* source,void* target,std::size_t bytes)noexcept {
        __try{std::memcpy(target,source,bytes);return true;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    void Trim(std::size_t offset,std::size_t bytes)const {
        if(!bytes)return;
        auto* address=static_cast<unsigned char*>(m_data)+offset;
        if(!m_frozen && !FlushViewOfFile(address,bytes))throw std::runtime_error("Mesh storage page flush failed.");
        // 未锁定文件页的 ERROR_NOT_LOCKED 是预期结果；只移出本文件的工作集，不清空进程工作集。
        VirtualUnlock(address,bytes);
    }
    void CloseMap()noexcept {
        if(m_data){UnmapViewOfFile(m_data);m_data=nullptr;}
        if(m_mapping){CloseHandle(m_mapping);m_mapping=nullptr;}
        m_capacity=0;m_order.clear();m_recent.clear();m_last.store(SIZE_MAX,std::memory_order_relaxed);
    }
    static constexpr std::size_t pageBytes=65536,residentPages=128;
    HANDLE m_file=INVALID_HANDLE_VALUE,m_mapping=nullptr;
    void* m_data=nullptr;std::size_t m_capacity=0;bool m_frozen=false;
    mutable std::mutex m_mutex;
    mutable std::list<std::size_t> m_order;
    mutable std::unordered_map<std::size_t,std::list<std::size_t>::iterator> m_recent;
    mutable std::atomic<std::size_t> m_last{SIZE_MAX};
};

template<class Value> class Source final : public MeshValueSource<Value> {
public:
    Source(std::shared_ptr<const File> file,std::size_t count):m_file(std::move(file)),m_count(count){
        if(!m_file || count>m_file->GetCapacity()/sizeof(Value))throw std::out_of_range("Mesh source count is outside the file.");
    }
    std::size_t GetCount()const noexcept override{return m_count;}
    const std::shared_ptr<const File>& GetFile()const noexcept{return m_file;}
    std::size_t GetResidentBytes()const noexcept override{
        return m_file->GetResidentBound()+std::min(m_count,cacheValues*cacheWindows)*sizeof(Value);
    }
    void ReadValues(std::size_t offset,std::size_t count,Value* target)const override {
        if(offset>m_count || count>m_count-offset)throw std::out_of_range("Mesh block is outside the array.");
        if(!count)return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        if(count>cacheValues){m_file->Copy(offset*sizeof(Value),count*sizeof(Value),target);return;}
        while(count) {
            const auto start=offset/cacheValues*cacheValues;
            auto index=cacheWindows;
            for(std::size_t i=0;i<cacheWindows;++i)if(m_cache[i].start==start){index=i;break;}
            if(index==cacheWindows) {
                index=m_next++%cacheWindows;
                auto& window=m_cache[index];window.start=SIZE_MAX;
                try {window.values.resize(std::min(cacheValues,m_count-start));}
                catch(const std::bad_alloc&) {
                    // 缓存是加速选项；目标缓冲已存在时，不因额外窗口分配失败拒绝读取。
                    m_file->Copy(offset*sizeof(Value),count*sizeof(Value),target);return;
                }
                // 短读取复用已有的有界映射页；立即移出工作集会让 BVH 随机访问反复缺页。
                try {m_file->CopyWindow(start*sizeof(Value),window.values.size()*sizeof(Value),window.values.data());}
                catch(const std::bad_alloc&) {
                    // 映射页缓存的记账分配同样可选；已有目标缓冲仍能直接读取。
                    m_file->Copy(start*sizeof(Value),window.values.size()*sizeof(Value),window.values.data());
                }
                window.start=start;
            }
            const auto& window=m_cache[index];
            const auto read=std::min(count,window.values.size()-(offset-start));
            std::copy_n(window.values.begin()+(offset-start),read,target);
            offset+=read;count-=read;target+=read;
        }
    }
private:
    static constexpr std::size_t cacheValues=std::max<std::size_t>(1,65536/sizeof(Value));
    // 对齐的多个窗口保留三角形的远端顶点，避免单窗口在顶点之间反复读放大。
    static constexpr std::size_t cacheWindows=8;
    struct Window final {std::size_t start=SIZE_MAX;std::vector<Value> values;};
    std::shared_ptr<const File> m_file;std::size_t m_count=0;
    mutable std::mutex m_mutex;mutable std::size_t m_next=0;mutable std::array<Window,cacheWindows> m_cache;
};
}

// 私有算法容器：原位访问仍保持地址语义，容量在临时文件上增长；完成后只交出块读取快照。
template<class Value> class PagedArray final {
    static_assert(std::is_trivially_copyable<Value>::value,"Paged mesh records must be value records.");
public:
    using value_type=Value;using size_type=std::size_t;
    PagedArray()=default;
    static PagedArray ShareFile(std::shared_ptr<const MeshStorage::File> file,std::size_t count){
        PagedArray value;value.m_file=std::const_pointer_cast<MeshStorage::File>(std::move(file));value.m_size=count;return value;
    }
    explicit PagedArray(std::size_t count){resize(count);}
    PagedArray(std::size_t count,const Value& value){resize(count,value);}
    PagedArray(std::initializer_list<Value> values){assign(values.begin(),values.end());}
    template<class Iterator> PagedArray(Iterator first,Iterator last){assign(first,last);}
    explicit PagedArray(std::vector<Value> values){assign(values.begin(),values.end());}
    PagedArray(const PagedArray& other){assign(other.begin(),other.end());}
    PagedArray& operator=(const PagedArray& other){if(this!=&other){PagedArray copy(other);swap(copy);}return *this;}
    PagedArray(PagedArray&& other)noexcept:m_file(std::move(other.m_file)),m_small(std::move(other.m_small)),m_size(other.m_size){other.m_size=0;}
    PagedArray& operator=(PagedArray&& other)noexcept {m_file=std::move(other.m_file);m_small=std::move(other.m_small);m_size=other.m_size;other.m_size=0;return *this;}
    std::size_t size()const noexcept{return m_size;}
    bool empty()const noexcept{return !m_size;}
    bool operator==(const PagedArray& other)const{return size()==other.size() && std::equal(begin(),end(),other.begin());}
    bool operator!=(const PagedArray& other)const{return !(*this==other);}
    std::size_t capacity()const noexcept{return m_file?m_file->GetCapacity()/sizeof(Value):m_small.capacity();}
    std::size_t GetResidentBytes()const noexcept{return m_file?m_file->GetResidentBound():m_small.capacity()*sizeof(Value);}
    void reserve(std::size_t count){
        if(count>max_size())throw std::length_error("Mesh array size overflows.");
        if(!m_file && count<=smallCount){
            if(count>m_small.capacity())m_small.reserve(std::max(count,std::min(smallCount,std::max<std::size_t>(1,m_small.capacity()*2))));
            return;
        }
        if(!m_file){
            auto file=std::make_shared<MeshStorage::File>();file->Reserve(count*sizeof(Value));
            auto* destination=static_cast<Value*>(file->GetData());
            for(std::size_t i=0;i<m_size;++i){file->Touch(i*sizeof(Value));::new(destination+i)Value(m_small[i]);}
            m_file=std::move(file);std::vector<Value>().swap(m_small);
        }else if(count>capacity()){
            m_file->Reserve(count*sizeof(Value));
            // C++17 中重新建立映射对象的生命周期；逐个值保留原对象表示，不建立全量副本。
            auto* destination=data();
            for(std::size_t i=0;i<m_size;++i){m_file->Touch(i*sizeof(Value));Value saved{};std::memcpy(&saved,destination+i,sizeof(Value));::new(destination+i)Value(saved);}
        }
    }
    void resize(std::size_t count,const Value& value=Value{}){
        const Value saved=value;
        if(count>m_size)reserve(count);
        if(!m_file){m_small.resize(count,saved);m_size=count;return;}
        const auto old=m_size;m_size=count;for(auto i=old;i<count;++i){m_file->Touch(i*sizeof(Value));::new(data()+i)Value(saved);}
    }
    void ResizeUninitialized(std::size_t count){reserve(count);if(!m_file)m_small.resize(count);else for(auto i=m_size;i<count;++i){m_file->Touch(i*sizeof(Value));::new(data()+i)Value;}m_size=count;}
    std::size_t max_size()const noexcept{return std::size_t(INT64_MAX)/sizeof(Value);}
    void shrink_to_fit()const{ReleasePages();}
    void clear()noexcept{m_size=0;m_small.clear();}
    void push_back(const Value& value){if(m_size==SIZE_MAX)throw std::length_error("Mesh array count overflows.");const Value saved=value;reserve(m_size+1);if(!m_file)m_small.push_back(saved);else {m_file->Touch(m_size*sizeof(Value));::new(data()+m_size)Value(saved);}++m_size;}
    template<class... Args> Value& emplace_back(Args&&... args){push_back(Value{std::forward<Args>(args)...});return back();}
    Value& operator[](std::size_t index){if(m_file)m_file->Touch(index*sizeof(Value));return data()[index];}
    const Value& operator[](std::size_t index)const{if(m_file)m_file->Touch(index*sizeof(Value));return data()[index];}
    Value& at(std::size_t index){if(index>=m_size)throw std::out_of_range("Mesh array index is invalid.");return (*this)[index];}
    const Value& at(std::size_t index)const{if(index>=m_size)throw std::out_of_range("Mesh array index is invalid.");return (*this)[index];}
    Value& front(){return (*this)[0];}const Value& front()const{return (*this)[0];}
    Value& back(){return (*this)[m_size-1];}const Value& back()const{return (*this)[m_size-1];}
    Value* data()noexcept{return m_file?static_cast<Value*>(m_file->GetData()):m_small.data();}
    const Value* data()const noexcept{return m_file?static_cast<const Value*>(m_file->GetData()):m_small.data();}
    void swap(PagedArray& other)noexcept{m_file.swap(other.m_file);m_small.swap(other.m_small);std::swap(m_size,other.m_size);}
    void ReleasePages()const{if(m_file)m_file->Release();}
    // 随机短记录不预取整页缓存；调用方提供有界工作区，文件读取保留 I/O 错误转换。
    void CopyValues(std::size_t offset,std::size_t count,Value* target)const {
        if(offset>m_size || count>m_size-offset)throw std::out_of_range("Paged value range is invalid.");
        if(!count)return;
        if(m_file)m_file->CopyWindow(offset*sizeof(Value),count*sizeof(Value),target);
        else std::copy_n(m_small.begin()+offset,count,target);
    }
    MeshReadArray<Value> GetReadSnapshot()const {
        if(!m_size)return {};
        if(!m_file)return MeshReadArray<Value>(m_small);
        m_file->Freeze();return MeshReadArray<Value>(std::make_shared<const MeshStorage::Source<Value>>(m_file,m_size));
    }
    MeshReadArray<Value> Finish() {
        if(!m_file){auto result=MeshReadArray<Value>(std::move(m_small));m_size=0;return result;}
        auto result=GetReadSnapshot();
        m_file.reset();m_size=0;return result;
    }
    template<bool Const> class Iterator final {
        using Owner=std::conditional_t<Const,const PagedArray,PagedArray>;
    public:
        using iterator_category=std::random_access_iterator_tag;using value_type=Value;using difference_type=std::ptrdiff_t;
        using reference=std::conditional_t<Const,const Value&,Value&>;using pointer=std::conditional_t<Const,const Value*,Value*>;
        Iterator()=default;Iterator(Owner* owner,std::size_t index):m_owner(owner),m_index(index){}
        reference operator*()const{return (*m_owner)[m_index];}pointer operator->()const{return &**this;}
        reference operator[](difference_type n)const{return *(*this+n);}
        Iterator& operator++(){++m_index;return *this;}Iterator operator++(int){auto copy=*this;++*this;return copy;}
        Iterator& operator--(){--m_index;return *this;}Iterator operator--(int){auto copy=*this;--*this;return copy;}
        Iterator& operator+=(difference_type n){m_index=std::size_t(difference_type(m_index)+n);return *this;}
        Iterator& operator-=(difference_type n){return *this+=-n;}
        Iterator operator+(difference_type n)const{auto copy=*this;return copy+=n;}
        friend Iterator operator+(difference_type n,Iterator value){return value+=n;}
        Iterator operator-(difference_type n)const{auto copy=*this;return copy-=n;}
        difference_type operator-(const Iterator& other)const{return difference_type(m_index)-difference_type(other.m_index);}
        bool operator==(const Iterator& other)const{return m_owner==other.m_owner && m_index==other.m_index;}
        bool operator!=(const Iterator& other)const{return !(*this==other);}
        bool operator<(const Iterator& other)const{return m_index<other.m_index;}
        bool operator>(const Iterator& other)const{return other<*this;}
        bool operator<=(const Iterator& other)const{return !(other<*this);}
        bool operator>=(const Iterator& other)const{return !(*this<other);}
    private:Owner* m_owner=nullptr;std::size_t m_index=0;
    };
    using iterator=Iterator<false>;using const_iterator=Iterator<true>;
    iterator begin(){return {this,0};}iterator end(){return {this,m_size};}
    const_iterator begin()const{return {this,0};}const_iterator end()const{return {this,m_size};}
    const_iterator cbegin()const{return begin();}const_iterator cend()const{return end();}
    template<class Input> void assign(Input first,Input last){clear();for(;first!=last;++first)push_back(*first);}
    void assign(std::size_t count,const Value& value){clear();resize(count,value);}
    template<class Input> iterator insert(iterator at,Input first,Input last){if(at!=end())throw std::logic_error("Mesh block appends must be ordered.");const auto start=m_size;for(;first!=last;++first)push_back(*first);return {this,start};}
private:
    // 小组件也使用同一容器；最多 64 KiB 的本地缓冲避免为每个小对象建立临时文件。
    static constexpr std::size_t smallCount=std::max<std::size_t>(1,65536/sizeof(Value));
    std::shared_ptr<MeshStorage::File> m_file;std::vector<Value> m_small;std::size_t m_size=0;
};

// 只在拥有者仍存活、底层数组已定长时使用。区间自身不另建文件或常驻索引。
template<class Value> struct PagedRange final {
    PagedArray<Value>* owner=nullptr;std::size_t offset=0,count=0;
    Value& operator[](std::size_t index)const{return (*owner)[offset+index];}
    Value& front()const{return (*this)[0];}
    std::size_t size()const noexcept{return count;}
    bool empty()const noexcept{return !count;}
    auto begin()const{return owner->begin()+static_cast<std::ptrdiff_t>(offset);}
    auto end()const{return begin()+static_cast<std::ptrdiff_t>(count);}
};

template<class Value,std::size_t Count> struct PagedArrayHash final {
    std::size_t operator()(const std::array<Value,Count>& key)const noexcept {
        std::size_t hash=0;
        for(const auto& value:key)hash^=std::hash<Value>{}(value)+std::size_t(0x9e3779b9U)+(hash<<6U)+(hash>>2U);
        return hash;
    }
};

// 私有查找表的槽也写入文件；只保存固定的页窗口，不为全部边/点建立内存 map。
template<class Key,class Value,class Hash=std::hash<Key>> class PagedMap final {
    struct Slot final {Key key{};Value value{};bool occupied=false;};
public:
    explicit PagedMap(std::function<void()> checkpoint={}):m_checkpoint(std::move(checkpoint)){}
    std::optional<Value> Find(const Key& key)const {
        if(m_slots.empty()){for(std::size_t i=0;i<m_smallCount;++i)if(m_small[i].key==key)return m_small[i].value;return {};}
        auto index=Hash{}(key)&(m_slots.size()-1);
        while(m_slots[index].occupied){if(m_slots[index].key==key)return m_slots[index].value;index=(index+1)&(m_slots.size()-1);}
        return {};
    }
    Value Insert(const Key& key,const Value& value) {
        if(const auto found=Find(key))return *found;
        if(m_slots.empty() && m_smallCount<m_small.size()){m_small[m_smallCount++]={key,value,true};++m_count;return value;}
        if(m_slots.empty() || m_count>=m_slots.size()/2)Grow();
        SetSlot(m_slots,key,value);++m_count;return value;
    }
    void clear(){m_slots=PagedArray<Slot>{};m_count=0;m_smallCount=0;}
private:
    static void SetSlot(PagedArray<Slot>& slots,const Key& key,const Value& value){
        auto index=Hash{}(key)&(slots.size()-1);
        while(slots[index].occupied)index=(index+1)&(slots.size()-1);
        slots[index]={key,value,true};
    }
    void Grow(){
        if(m_slots.size()>m_slots.max_size()/2)throw std::length_error("Mesh lookup size overflows.");
        PagedArray<Slot> next;next.resize(m_slots.empty()?1024:m_slots.size()*2);
        for(std::size_t i=0;i<m_smallCount;++i)SetSlot(next,m_small[i].key,m_small[i].value);
        for(std::size_t i=0;i<m_slots.size();++i){if((i&1023U)==0 && m_checkpoint)m_checkpoint();const auto slot=m_slots[i];if(slot.occupied)SetSlot(next,slot.key,slot.value);}
        m_slots.swap(next);m_smallCount=0;m_slots.ReleasePages();
    }
    PagedArray<Slot> m_slots;std::array<Slot,32> m_small{};std::size_t m_count=0,m_smallCount=0;std::function<void()> m_checkpoint;
};

// 固定大小排序工作区，随后以两个游标逐段归并；文件大小不计为驻留内存。
template<class Value,class Less> void SortPagedArray(PagedArray<Value>& values,Less less,
    const std::function<void()>& checkpoint={},std::size_t workspaceBytes=2*1024*1024) {
    const auto run=std::max<std::size_t>(1,workspaceBytes/sizeof(Value));
    std::vector<Value> buffer;buffer.reserve(std::min(run,values.size()));
    std::size_t work=0;
    const auto check=[&]{if((work++ & 1023U)==0 && checkpoint)checkpoint();};
    for(std::size_t first=0;first<values.size();) {
        const auto count=std::min(run,values.size()-first);buffer.resize(count);
        for(std::size_t i=0;i<count;++i){check();buffer[i]=values[first+i];}
        std::sort(buffer.begin(),buffer.end(),[&](const Value& a,const Value& b){check();return less(a,b);});
        for(std::size_t i=0;i<count;++i){check();values[first+i]=buffer[i];}
        first+=count;
    }
    std::vector<Value>().swap(buffer);values.ReleasePages();
    for(std::size_t width=run;width<values.size();) {
        PagedArray<Value> output;output.ResizeUninitialized(values.size());
        for(std::size_t first=0;first<values.size();) {
            const auto middle=first+std::min(width,values.size()-first);
            const auto last=middle+std::min(width,values.size()-middle);
            auto left=first,right=middle,target=first;
            while(left<middle || right<last){
                check();
                if(right==last || (left<middle && !less(values[right],values[left])))output[target++]=values[left++];
                else output[target++]=values[right++];
            }
            first=last;
        }
        values.swap(output);values.ReleasePages();
        if(width>values.size()/2)break;width*=2;
    }
    if(checkpoint)checkpoint();
}

class MeshPayloadBuilder final {
    template<class Value> class ArraySource final:public MeshValueSource<Value> {
    public:
        explicit ArraySource(std::shared_ptr<const DataArray<Value>> source):m_source(std::move(source)){}
        std::size_t GetCount()const noexcept override{return m_source->size();}
        std::size_t GetResidentBytes()const noexcept override{return m_source->get_allocator().storage?0:m_source->capacity()*sizeof(Value);}
        void ReadValues(std::size_t offset,std::size_t count,Value* target)const override {
            if(offset>m_source->size() || count>m_source->size()-offset)throw std::out_of_range("Mesh shared array range is invalid.");
            std::copy_n(m_source->begin()+offset,count,target);
        }
    private:std::shared_ptr<const DataArray<Value>> m_source;
    };
public:
    static std::shared_ptr<const SurfaceMeshPayload> Build(
        std::shared_ptr<const DataArray<double>> points,std::shared_ptr<const DataArray<std::uint64_t>> faces,
        std::vector<MeshAttribute> pointData={},std::string frame="RAS",std::vector<MeshAttribute> cellData={},
        const std::function<bool()>& cancelled={}) {
        if(!points||!faces)return {};
        return Build(MeshReadArray<double>(std::make_shared<const ArraySource<double>>(std::move(points))),
            MeshReadArray<std::uint64_t>(std::make_shared<const ArraySource<std::uint64_t>>(std::move(faces))),
            SurfaceMeshPayload::GetFields(std::move(pointData)),std::move(frame),SurfaceMeshPayload::GetFields(std::move(cellData)),cancelled);
    }
    template<class Value> static PagedArray<Value> ShareFrozen(const MeshReadArray<Value>& input,const std::function<bool()>& cancelled={}){
        if(auto file=std::dynamic_pointer_cast<const MeshStorage::Source<Value>>(input.m_source))return PagedArray<Value>::ShareFile(file->GetFile(),input.size());
        PagedArray<Value> values;values.reserve(input.size());
        for(std::size_t offset=0;offset<input.size();){auto chunk=input.GetChunk(offset,65536/sizeof(Value)+1,cancelled);
            if(chunk.error!=MeshReadError::None)throw std::runtime_error("Parent mesh source read failed.");values.insert(values.end(),chunk.values.begin(),chunk.values.end());offset=chunk.nextOffset;}
        values.GetReadSnapshot();return values;
    }
    static std::shared_ptr<const SurfaceMeshPayload> Build(MeshReadArray<double> points,MeshReadArray<std::uint64_t> triangles,
        std::vector<MeshField> pointFields={},std::string frame="RAS",std::vector<MeshField> cellFields={},const std::function<bool()>& cancelled={}) {
        auto value=std::shared_ptr<const SurfaceMeshPayload>(new SurfaceMeshPayload(std::move(points),std::move(triangles),
            std::move(pointFields),std::move(frame),std::move(cellFields),SurfaceMeshPayload::ReadUse{cancelled}));
        return value->GetValid()?value:nullptr;
    }
};

template<class Value> class MeshRangeSource final:public MeshValueSource<Value> {
public:
    MeshRangeSource(MeshReadArray<Value> source,std::size_t offset,std::size_t count)
        :m_source(std::move(source)),m_offset(offset),m_count(count){
        if(offset>m_source.size() || count>m_source.size()-offset)throw std::out_of_range("Mesh range is invalid.");
    }
    std::size_t GetCount()const noexcept override{return m_count;}
    std::size_t GetResidentBytes()const noexcept override{return m_source.GetResidentBytes();}
    void ReadValues(std::size_t offset,std::size_t count,Value* target)const override {
        if(offset>m_count || count>m_count-offset)throw std::out_of_range("Mesh range block is invalid.");
        while(count){const auto chunk=m_source.GetChunk(m_offset+offset,count);
            if(chunk.error!=MeshReadError::None || chunk.values.empty())throw std::runtime_error("Mesh range read failed.");
            std::copy(chunk.values.begin(),chunk.values.end(),target);target+=chunk.values.size();offset+=chunk.values.size();count-=chunk.values.size();}
    }
private:MeshReadArray<Value> m_source;std::size_t m_offset,m_count;
};
template<class Value> MeshReadArray<Value> GetMeshRange(MeshReadArray<Value> source,std::size_t offset,std::size_t count){
    return MeshReadArray<Value>(std::make_shared<const MeshRangeSource<Value>>(std::move(source),offset,count));
}

struct MeshFieldWriter final {
    std::string name;std::size_t componentCount=0;PagedArray<double> values;std::uint32_t activeRoles=0;
    MeshField Finish(){return {std::move(name),componentCount,values.Finish(),activeRoles};}
};
inline std::vector<MeshField> FinishMeshFields(std::vector<MeshFieldWriter> writers){
    std::vector<MeshField> fields;fields.reserve(writers.size());for(auto& writer:writers)fields.push_back(writer.Finish());return fields;
}

template<class Input,class Value,class Convert> class MeshConvertedSource final:public MeshValueSource<Value> {
public:
    MeshConvertedSource(MeshReadArray<Input> source,std::size_t components,Convert convert)
        :m_source(std::move(source)),m_components(components),m_convert(std::move(convert)) {
        if(!components || m_source.size()>SIZE_MAX/components)throw std::length_error("Mesh field count overflows.");
    }
    std::size_t GetCount()const noexcept override{return m_source.size()*m_components;}
    std::size_t GetResidentBytes()const noexcept override{return m_source.GetResidentBytes();}
    void ReadValues(std::size_t offset,std::size_t count,Value* target)const override {
        if(offset>GetCount() || count>GetCount()-offset)throw std::out_of_range("Converted mesh block is invalid.");
        if(count==1){const auto value=m_source[offset/m_components];*target=m_convert(value,offset%m_components);return;}
        auto record=offset/m_components;auto component=offset%m_components;
        while(count){
            const auto required=1+(count-1+component)/m_components;
            auto chunk=m_source.GetChunk(record,required);
            if(chunk.error!=MeshReadError::None)throw std::runtime_error("Converted mesh source read failed.");
            for(const auto& value:chunk.values){
                while(component<m_components && count){*target++=m_convert(value,component++);--count;}
                component=0;++record;if(!count)break;
            }
        }
    }
private:MeshReadArray<Input> m_source;std::size_t m_components;Convert m_convert;
};
template<class Value,class Input,class Convert> MeshReadArray<Value> ConvertMeshArray(MeshReadArray<Input> source,std::size_t components,Convert convert){
    return MeshReadArray<Value>(std::make_shared<const MeshConvertedSource<Input,Value,Convert>>(std::move(source),components,std::move(convert)));
}
