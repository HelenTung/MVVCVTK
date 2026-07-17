#pragma once
#include <cstddef>
#include <string>

class MemMappedFile {
public:
    MemMappedFile() = default;
    ~MemMappedFile() { Clear(); }
    MemMappedFile(const MemMappedFile&) = delete;
    MemMappedFile& operator=(const MemMappedFile&) = delete;

    /// @param length 0 = 映射整个文件
    bool Load(const std::string& path, size_t length = 0);
    void Clear();

    const void* GetData() const { return m_data; }
    size_t      GetSize() const { return m_size; }
    bool        GetOpen() const { return m_data != nullptr; }

private:
    // GetData 是与本对象绑定的借用 view，Clear/析构/下次 Load 后失效。
    const void* m_data = nullptr;
    size_t      m_size = 0;

#ifdef _WIN32
    // Win32 文件句柄与 file-mapping 句柄；成功映射后由本对象成组持有，Clear() 先 unmap 再依次关闭。
    void* m_hFile = nullptr;
    void* m_hMap = nullptr;
#else
    // POSIX 映射对应的打开文件描述符；Clear() 在 munmap(m_data, m_size) 后关闭并恢复为 -1。
    int   m_fd = -1;
#endif
};
