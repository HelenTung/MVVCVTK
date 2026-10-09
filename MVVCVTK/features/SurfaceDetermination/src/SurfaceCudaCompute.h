#pragma once
#include "SurfaceCudaTypes.h"

class SurfaceCudaCompute final {
  public:
    SurfaceCudaCompute() = default;
    ~SurfaceCudaCompute() noexcept;
    SurfaceCudaCompute(const SurfaceCudaCompute &) = delete;
    SurfaceCudaCompute &operator=(const SurfaceCudaCompute &) = delete;
    bool Start(const SurfaceCudaVolume &volume, std::size_t releasedBytes, std::size_t deviceLimit);
    bool Read(const double *points, std::size_t count, SurfaceCudaSample *output);
    // 缓冲由 session 所有，只在创建线程访问；提交后索引不可再写，收集后样本可读到下次 Prepare。
    double *Prepare(unsigned slot);
    bool Submit(unsigned slot, std::size_t count, SurfaceCudaTicket &ticket);
    const SurfaceCudaSample *Collect(const SurfaceCudaTicket &ticket);
    // 仅仓内测试后端提供该探针：1=提交中断，2=收集失败，3=收尾同步失败。
    bool SetTestFault(unsigned fault);
    bool GetActive() const noexcept { return m_session != nullptr; }
    void Reset() noexcept;
    std::size_t GetDeviceBytes() const noexcept { return m_deviceBytes; }

  private:
    void *m_library = nullptr;
    void *m_session = nullptr;
    SurfaceCudaRead m_read = nullptr;
    SurfaceCudaPrepareCallback m_prepare = nullptr;
    SurfaceCudaSubmitCallback m_submit = nullptr;
    SurfaceCudaCollectCallback m_collect = nullptr;
    SurfaceCudaTestFaultCallback m_testFault = nullptr;
    SurfaceCudaClose m_close = nullptr;
    std::size_t m_deviceBytes = 0;
    std::uint64_t m_epoch = 0;
};
