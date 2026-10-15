#pragma once
#include "Data/DataPayloads.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <algorithm>
#include <mutex>
#include <stdexcept>

// 任务内一个临时文件，多次分配共用映射页，不按对象建文件。
// 文件只属于内存后备，不是工程格式；最后一个数组 owner 消失后自动删除。
class MappedArrayStorage final : public DataArrayStorage {
    struct Page {
        void* data = nullptr;
        std::size_t size = 0, used = 0, live = 0;
        ~Page() { if (data) UnmapViewOfFile(data); }
    };
    struct Header { Page* page; void* allocation; std::size_t bytes; };
public:
    explicit MappedArrayStorage(std::size_t heapLimit = 8 * 1024 * 1024) : m_heapLimit(heapLimit) {
        SYSTEM_INFO system{}; GetSystemInfo(&system); m_granularity = system.dwAllocationGranularity;
    }
    ~MappedArrayStorage() noexcept override {
        m_pages.clear();
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
    }
    void* Allocate(std::size_t bytes, std::size_t alignment) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        alignment = (std::max)(alignment, alignof(Header));
        if (bytes > SIZE_MAX - sizeof(Header) - alignment) throw std::bad_alloc();
        const auto required = bytes + sizeof(Header) + alignment;
        // 小数组复用有限堆工作区；超过驻留目标则使用同一文件中的映射页。
        // 这是存储选择，不拒绝剩余对象，也不影响正式结果精度。
        if (bytes < 1024 * 1024 && bytes <= m_heapLimit - m_heapBytes) {
            auto* allocation = ::operator new(required);
            const auto address = (reinterpret_cast<std::uintptr_t>(allocation) + sizeof(Header) + alignment - 1) / alignment * alignment;
            auto* result = reinterpret_cast<void*>(address);
            reinterpret_cast<Header*>(result)[-1] = {nullptr, allocation, bytes};
            m_heapBytes += bytes;
            return result;
        }
        Page* page = m_pages.empty() ? nullptr : m_pages.back().get();
        if (!page || !page->data || required > page->size - page->used) {
            OpenFile();
            constexpr std::size_t pageBytes = 4 * 1024 * 1024;
            if (required > SIZE_MAX - m_granularity) throw std::bad_alloc();
            const auto size = ((std::max)(required, pageBytes) + m_granularity - 1) / m_granularity * m_granularity;
            if (size > static_cast<std::uint64_t>(INT64_MAX) - m_length) throw std::bad_alloc();
            const auto end = m_length + size;
            auto next = std::make_unique<Page>();
            HANDLE mapping = CreateFileMappingW(m_file, nullptr, PAGE_READWRITE,
                static_cast<DWORD>(end >> 32), static_cast<DWORD>(end), nullptr);
            if (!mapping) throw std::runtime_error("Cannot extend result backing storage.");
            next->data = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                static_cast<DWORD>(m_length >> 32), static_cast<DWORD>(m_length), size);
            CloseHandle(mapping);
            if (!next->data) throw std::runtime_error("Cannot map result backing storage.");
            next->size = size; m_length = end; page = next.get(); m_pages.push_back(std::move(next));
        }
        const auto base = reinterpret_cast<std::uintptr_t>(page->data);
        const auto offset = (base + page->used + sizeof(Header) + alignment - 1) / alignment * alignment - base;
        auto* result = static_cast<unsigned char*>(page->data) + offset;
        reinterpret_cast<Header*>(result)[-1] = {page, nullptr, bytes};
        page->used = offset + bytes; ++page->live;
        return result;
    }
    void Deallocate(void* data, std::size_t, std::size_t) noexcept override {
        if (!data) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto header = reinterpret_cast<Header*>(data)[-1];
        if (!header.page) { m_heapBytes -= header.bytes; ::operator delete(header.allocation); return; }
        if (--header.page->live == 0) {
            UnmapViewOfFile(header.page->data); header.page->data = nullptr;
        }
    }
    void Finish() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& page : m_pages)
            if (page->data && !FlushViewOfFile(page->data, page->used))
                throw std::runtime_error("Cannot flush completed result blocks.");
        if (m_file != INVALID_HANDLE_VALUE && !FlushFileBuffers(m_file))
            throw std::runtime_error("Cannot finish result backing storage.");
    }
private:
    void OpenFile() {
        if (m_file != INVALID_HANDLE_VALUE) return;
        wchar_t directory[MAX_PATH + 1]{}, path[MAX_PATH + 1]{};
        const auto count = GetTempPathW(MAX_PATH, directory);
        if (!count || count > MAX_PATH || !GetTempFileNameW(directory, L"mvc", 0, path))
            throw std::runtime_error("Cannot create result backing storage.");
        m_file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) {
            DeleteFileW(path);
            throw std::runtime_error("Cannot open result backing storage.");
        }
    }
    HANDLE m_file = INVALID_HANDLE_VALUE;
    std::size_t m_granularity = 65536;
    std::uint64_t m_length = 0;
    std::size_t m_heapBytes = 0;
    std::size_t m_heapLimit;
    std::mutex m_mutex;
    std::vector<std::unique_ptr<Page>> m_pages;
};

// 仅当前计算作用域选择 allocator；每个数组保存自己的 owner，离开作用域不影响读者。
class ResultArrayScope final {
public:
    explicit ResultArrayScope(std::size_t heapLimit = 8 * 1024 * 1024)
        : m_previous(DataArrayStorage::GetCurrent()), m_storage(std::make_shared<MappedArrayStorage>(heapLimit)) {
        DataArrayStorage::GetCurrent() = m_storage;
    }
    ~ResultArrayScope() { DataArrayStorage::GetCurrent() = std::move(m_previous); }
    ResultArrayScope(const ResultArrayScope&) = delete;
    ResultArrayScope& operator=(const ResultArrayScope&) = delete;
    void Finish() { m_storage->Finish(); }
private:
    std::shared_ptr<DataArrayStorage> m_previous;
    std::shared_ptr<MappedArrayStorage> m_storage;
};
