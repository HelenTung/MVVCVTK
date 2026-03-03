#pragma once
#include <cstddef>
#include <string>

class MemMappedFile {
public:
    MemMappedFile() = default;
    ~MemMappedFile() { close(); }
    MemMappedFile(const MemMappedFile&) = delete;
    MemMappedFile& operator=(const MemMappedFile&) = delete;

    /// @param length 0 = 映射整个文件
    bool open(const std::string& path, size_t length = 0);
    void close();

    const void* data() const { return m_data; }
    size_t      size() const { return m_size; }
    bool     is_open() const { return m_data != nullptr; }

private:
    const void* m_data = nullptr;
    size_t      m_size = 0;

#ifdef _WIN32
    void* m_hFile = nullptr;
    void* m_hMap = nullptr;
#else
    int   m_fd = -1;
#endif
};