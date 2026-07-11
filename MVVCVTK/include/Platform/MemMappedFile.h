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
    // [风险] GetData 裸 view 在 Clear()/析构后失效；Load() 又不会先 Clear 或拒绝已打开对象，
    // 连续成功 Load 会覆盖地址/平台句柄并泄漏上一映射，因此调用方必须显式 Clear 后再复用。
    const void* m_data = nullptr;
    // [风险] 非零 length 未校验是否超过文件大小，POSIX 访问越过 EOF 的映射页可能故障；
    // 映射失败前若已写入该值，还可能出现 GetOpen()==false 但 GetSize()!=0 的中间状态。
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
