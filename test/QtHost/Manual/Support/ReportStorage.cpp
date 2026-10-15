#include "ReportStorage.h"
#include <QDataStream>
#include <QFile>
#include <algorithm>
#include <queue>
namespace Manual {
namespace {
struct IndexCursor {
    QFile file;
    ReportRowId count,start=-1;
    int length=0;
    std::array<ReportRowId,1024> page{};
    IndexCursor(const QString& path,ReportRowId rows):file(path),count(rows){if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("报告行号索引打开失败");}
    ReportRowId Read(ReportRowId row) {
        if(row<0 || row>=count)throw std::out_of_range("报告行号索引越界");
        if(row<start || row>=start+length){start=row/1024*1024;length=static_cast<int>(std::min<ReportRowId>(1024,count-start));const auto bytes=length*static_cast<qint64>(sizeof(ReportRowId));
            if(!file.seek(start*sizeof(ReportRowId)) || file.read(reinterpret_cast<char*>(page.data()),bytes)!=bytes)throw std::runtime_error("报告行号索引读取失败");}
        return page[static_cast<std::size_t>(row-start)];
    }
};
bool Before(const ReportSortKey& a,const ReportSortKey& b,Qt::SortOrder direction) {
    // 继承既有复合键：数值、原始文本、原行号；两个方向均稳定处理完全相等的键。
    int order;
    if(a.numeric!=b.numeric)order=a.numeric?-1:1;
    else if(a.numeric && a.number!=b.number)order=a.number<b.number?-1:1;
    else order=QString::compare(a.text,b.text,a.numeric?Qt::CaseSensitive:Qt::CaseInsensitive);
    if(!order)return a.row<b.row;
    return direction==Qt::AscendingOrder?order<0:order>0;
}
void WriteKey(QDataStream& stream,const ReportSortKey& key){stream<<key.row<<key.text<<key.number<<key.numeric;if(stream.status()!=QDataStream::Ok)throw std::runtime_error("报告排序段写入失败");}
struct RunCursor {
    QFile file;QDataStream stream;ReportRowId end;ReportSortKey key;bool active=false;
    RunCursor(const QString& path,ReportRowId start,ReportRowId stop):file(path),stream(&file),end(stop){if(!file.open(QIODevice::ReadOnly) || !file.seek(start))throw std::runtime_error("报告排序段打开失败");Next();}
    void Next(){active=file.pos()<end;if(active){stream>>key.row>>key.text>>key.number>>key.numeric;if(stream.status()!=QDataStream::Ok || file.pos()>end)throw std::runtime_error("报告排序段读取失败");}}
};
struct Runs {
    QTemporaryFile file;QDataStream stream{&file};ReportIndex boundaries;
    Runs(){if(!file.open())throw std::runtime_error("无法创建报告排序工作文件");}
    void Flush(){if(!file.flush())throw std::runtime_error("报告排序段刷新失败");boundaries.PrepareRead();}
};
}
class ReportIndex::Impl {
public:
    QTemporaryFile file;ReportRowId count=0;mutable std::shared_ptr<IndexCursor> cursor;
    Impl(){if(!file.open())throw std::runtime_error("无法创建报告行号索引");}
};
ReportIndex::ReportIndex():m_impl(std::make_unique<Impl>()){}
ReportIndex::~ReportIndex()=default;
void ReportIndex::Append(ReportRowId value){if(m_impl->count>=std::numeric_limits<qint64>::max()/sizeof(value))throw std::overflow_error("报告行号索引地址溢出");auto& file=m_impl->file;if(!file.seek(file.size()) || file.write(reinterpret_cast<const char*>(&value),sizeof(value))!=sizeof(value))throw std::runtime_error("报告行号索引写入失败");++m_impl->count;m_impl->cursor.reset();}
ReportRowId ReportIndex::GetCount() const{return m_impl->count;}
void ReportIndex::PrepareRead() const{if(!m_impl->file.flush())throw std::runtime_error("报告行号索引刷新失败");}
ReportRowId ReportIndex::Read(ReportRowId row) const{if(!m_impl->cursor){PrepareRead();m_impl->cursor=std::make_shared<IndexCursor>(m_impl->file.fileName(),m_impl->count);}return m_impl->cursor->Read(row);}
std::function<ReportRowId(ReportRowId)> ReportIndex::CreateReader() const{auto cursor=std::make_shared<IndexCursor>(m_impl->file.fileName(),m_impl->count);return [cursor](ReportRowId row){return cursor->Read(row);};}
class ReportSorter::Impl {
public:
    explicit Impl(Qt::SortOrder order):direction(order),runs(std::make_unique<Runs>()),rows(std::make_shared<ReportIndex>()),heap([this](int a,int b){return Before(merge[b]->key,merge[a]->key,direction);}){}
    Qt::SortOrder direction;
    std::vector<ReportSortKey> chunk;
    std::size_t flushNext=0;
    bool flushing=false,finished=false,done=false;
    ReportRowId run=0;
    std::unique_ptr<Runs> runs,output;
    std::unique_ptr<RunCursor> left;
    std::shared_ptr<ReportIndex> rows;
    std::vector<std::unique_ptr<RunCursor>> merge;
    std::priority_queue<int,std::vector<int>,std::function<bool(int,int)>> heap;
    void StartFlush(){if(chunk.empty())return;std::sort(chunk.begin(),chunk.end(),[&](const auto& a,const auto& b){return Before(a,b,direction);});flushing=true;flushNext=0;runs->boundaries.Append(runs->file.pos());}
};
ReportSorter::ReportSorter(Qt::SortOrder direction):m_impl(std::make_unique<Impl>(direction)){}
ReportSorter::~ReportSorter()=default;
bool ReportSorter::GetCanAdd() const{return !m_impl->flushing && !m_impl->finished;}
void ReportSorter::Add(ReportSortKey key){auto& s=*m_impl;if(!GetCanAdd())throw std::logic_error("报告排序输入状态错误");s.chunk.push_back(std::move(key));if(s.chunk.size()==16384)s.StartFlush();}
void ReportSorter::Finish(){auto& s=*m_impl;if(!s.finished){s.finished=true;if(!s.flushing)s.StartFlush();}}
bool ReportSorter::Continue(){
    auto& s=*m_impl;if(s.done)return true;
    if(s.flushing){WriteKey(s.runs->stream,s.chunk[s.flushNext++]);if(s.flushNext==s.chunk.size()){s.runs->boundaries.Append(s.runs->file.pos());s.chunk.clear();s.flushing=false;}return false;}
    if(!s.finished)return false;
    const auto count=s.runs->boundaries.GetCount()/2;
    if(!count){s.rows->PrepareRead();s.done=true;return true;}
    if(count==1){
        if(!s.left){s.runs->Flush();s.left=std::make_unique<RunCursor>(s.runs->file.fileName(),s.runs->boundaries.Read(0),s.runs->boundaries.Read(1));}
        if(s.left->active){s.rows->Append(s.left->key.row);s.left->Next();return false;}
        s.left.reset();s.runs.reset();s.rows->PrepareRead();s.done=true;return true;
    }
    if(!s.output){s.runs->Flush();s.output=std::make_unique<Runs>();s.run=0;}
    if(s.merge.empty()){
        s.output->boundaries.Append(s.output->file.pos());
        const auto end=s.run+std::min<ReportRowId>(32,count-s.run);
        for(ReportRowId i=s.run;i<end;++i){s.merge.push_back(std::make_unique<RunCursor>(s.runs->file.fileName(),s.runs->boundaries.Read(i*2),s.runs->boundaries.Read(i*2+1)));if(s.merge.back()->active)s.heap.push(static_cast<int>(s.merge.size()-1));}
    }
    if(!s.heap.empty()){
        const auto index=s.heap.top();s.heap.pop();auto& next=s.merge[index];
        WriteKey(s.output->stream,next->key);next->Next();if(next->active)s.heap.push(index);return false;
    }
    s.output->boundaries.Append(s.output->file.pos());s.run+=static_cast<ReportRowId>(s.merge.size());s.merge.clear();
    if(s.run>=count){s.runs=std::move(s.output);s.run=0;}
    return false;
}
std::shared_ptr<ReportIndex> ReportSorter::GetRows() const{if(!m_impl->done)throw std::logic_error("报告排序尚未完成");return m_impl->rows;}
}
