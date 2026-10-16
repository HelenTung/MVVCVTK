#include "Data/DataPayloads.h"
#include "MappedArrayStorage.h"
#include "MeshPayloadBuilder.h"
#include <Windows.h>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct alignas(4096) AlignedRecord { std::uint64_t value = 0; };
}
bool GetDataArrayTests()
{
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    try {
        std::weak_ptr<DataArrayStorage> storage;
        std::shared_ptr<const SurfaceMeshPayload> mesh;
        const void* original = nullptr;
        {
            ResultArrayScope scope(0);
            DataArray<std::uint64_t> values;
            constexpr std::size_t count = 1024 * 1024;
            for (std::size_t i = 0; i < count; ++i) values.push_back(i ^ 0x12345678U);
            for (std::size_t i = 0; i < count; ++i)
                Require(values[i] == (i ^ 0x12345678U), "backing growth changed logical array values");
            MEMORY_BASIC_INFORMATION memory{};
            Require(VirtualQuery(values.data(), &memory, sizeof(memory)) != 0 && memory.Type == MEM_MAPPED,
                "large result still requires a full heap allocation");
            auto copy = values;
            copy[0] = 7;
            Require(values[0] != 7 && copy.data() != values.data(), "mutable array copies alias their elements");
            DataArray<AlignedRecord> aligned(16);
            Require(reinterpret_cast<std::uintptr_t>(aligned.data()) % alignof(AlignedRecord) == 0,
                "mapped result does not satisfy element alignment");
            const auto allocator = aligned.get_allocator().storage;
            auto* overAligned = allocator->Allocate(131072,131072);
            const bool isAligned = reinterpret_cast<std::uintptr_t>(overAligned) % 131072 == 0;
            allocator->Deallocate(overAligned,131072,131072);
            Require(isAligned,"backing allocation alignment depends on the mapping base address");
            DataArray<double> points{0,0,0, 1,0,0, 0,1,0};
            DataArray<std::uint64_t> triangles{0,1,2};
            original = points.data(); storage = points.get_allocator().storage;
            mesh = MeshPayloadBuilder::Build(std::make_shared<const DataArray<double>>(std::move(points)),
                std::make_shared<const DataArray<std::uint64_t>>(std::move(triangles)));
            Require(mesh && mesh->GetVertices().data() == original, "mesh freezing copied the completed points");
            scope.Finish();
        }
        Require(!storage.expired() && mesh->GetVertices()[3] == 1, "reader lost backing storage after producer exit");
        auto snapshot = std::dynamic_pointer_cast<const SurfaceMeshPayload>(mesh->CreateSnapshot());
        mesh.reset();
        Require(snapshot && snapshot->GetVertices().data() == original && !storage.expired(),
            "graph snapshot copied or prematurely released frozen geometry");
        snapshot.reset();
        Require(storage.expired(), "backing store outlived its final result reader");
        {
            ResultArrayScope scope(0);std::size_t checks=0;
            auto points=std::make_shared<const DataArray<double>>(3*65536,0.0);
            auto faces=std::make_shared<const DataArray<std::uint64_t>>();
            const auto cancelled=MeshPayloadBuilder::Build(points,faces,{},"RAS",{},[&]{return ++checks>=2;});
            Require(!cancelled&&checks==2,"freezing a large candidate ignored cancellation during validation");
        }

        std::vector<RecordColumn> input{{"id", std::vector<std::uint64_t>{5,6}}};
        RecordTablePayload table(DataTypeId{"test.rows",1}, "test.rows", input);
        std::get<DataArray<std::uint64_t>>(input[0].values)[0] = 99;
        const auto tableSnapshot = std::dynamic_pointer_cast<const RecordTablePayload>(table.CreateSnapshot());
        Require(tableSnapshot && &tableSnapshot->GetColumns() == &table.GetColumns()
            && std::get<DataArray<std::uint64_t>>(table.GetColumns()[0].values)[0] == 5,
            "record snapshots copied frozen columns or accepted an external mutable alias");
        auto runs=MeshAttributeValues::FromRuns({{0,3,33554433},{3,2,INT32_MAX}});
        const auto& readonlyRuns=runs;
        Require(runs.size()==5 && readonlyRuns[0]==33554433 && readonlyRuns[4]==INT32_MAX
            && runs.GetValueType()==ImageValueType::Int32,"integer runs changed formal IDs");
        auto writable=runs;writable[0]=7;
        Require(readonlyRuns[0]==33554433 && writable.GetValueType()==ImageValueType::Float64,
            "writing an attribute changed a frozen integer owner");
        auto integers=MeshAttributeValues::FromIntegers({INT32_MIN,0,INT32_MAX});
        Require(integers.GetChunk(1,2)==std::vector<double>({0,INT32_MAX}),"integer attribute range read failed");
        bool rejected=false;
        try{MeshAttributeValues::FromRuns({{1,1,3}});}catch(const std::invalid_argument&){rejected=true;}
        Require(rejected,"noncontiguous attribute runs were accepted");
        std::weak_ptr<DataArrayStorage> transferred;
        {
        DataArray<std::uint64_t> shared;
        {ResultArrayScope scope(0);DataArray<std::uint64_t> candidate(4096,19);
            transferred=candidate.get_allocator().storage;shared=std::move(candidate);scope.Finish();}
        std::thread consumer([values=std::move(shared)]()mutable{
            if(values.at(4095)!=19)std::terminate();
            // 从消费线程分配/释放同一 owner，检查作用域不是线程生命周期。
            auto copy=values;copy.resize(8192,23);
        });consumer.join();
        } // MSVC 的 moved-from 容器仍保有使用原 allocator 的调试 proxy。
        Require(transferred.expired(),"backing owner leaked after cross-thread consumer release");
        {
            const auto count=GetEnvironmentVariableW(L"TMP",nullptr,0);
            std::wstring previous(count,L'\0');
            if(count){GetEnvironmentVariableW(L"TMP",previous.data(),count);previous.resize(count-1);}
            struct RestoreTemp {std::wstring value;bool existed;
                ~RestoreTemp(){SetEnvironmentVariableW(L"TMP",existed?value.c_str():nullptr);}
            } restore{previous,count!=0};
            wchar_t directory[MAX_PATH+1]{};Require(GetTempPathW(MAX_PATH,directory)!=0,"test temp directory unavailable");
            const auto missing=std::wstring(directory)+L"mvvcvtk-missing-"+std::to_wstring(GetCurrentProcessId());
            Require(GetFileAttributesW(missing.c_str())==INVALID_FILE_ATTRIBUTES,"test failure path unexpectedly exists");
            Require(SetEnvironmentVariableW(L"TMP",missing.c_str())!=0,"test cannot set a missing temporary storage location");
            bool failed=false;std::weak_ptr<DataArrayStorage> failedOwner;
            try{ResultArrayScope scope(0);failedOwner=DataArrayStorage::GetCurrent();DataArray<std::uint64_t> values(4096,7);scope.Finish();}
            catch(const std::runtime_error&){failed=true;}
            Require(failed&&failedOwner.expired(),"backing I/O failure fell back to a heap result or leaked the candidate owner");
        }
        return true;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return false; }
}
