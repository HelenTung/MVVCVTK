#pragma once
#include <cstddef>
#include <string>

class MemMappedFile {
public:
    MemMappedFile() = default;
    ~MemMappedFile() { Close(); }
    MemMappedFile(const MemMappedFile&) = delete;
    MemMappedFile& operator=(const MemMappedFile&) = delete;

    /// @param length 0 = 映射整个文件
    bool Open(const std::string& path, size_t length = 0);
    void Close();

    const void* GetData() const { return m_data; }
    size_t      GetSize() const { return m_size; }
    bool        GetOpen() const { return m_data != nullptr; }

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
