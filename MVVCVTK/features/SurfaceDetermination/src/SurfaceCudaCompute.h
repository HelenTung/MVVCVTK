#pragma once
#include "SurfaceCudaTypes.h"

class SurfaceCudaCompute final {
  public:
    SurfaceCudaCompute() = default;
    ~SurfaceCudaCompute() noexcept;
    SurfaceCudaCompute(const SurfaceCudaCompute &) = delete;
    SurfaceCudaCompute &operator=(const SurfaceCudaCompute &) = delete;
    bool Start(const SurfaceCudaVolume &volume, std::size_t hostCredit, std::size_t deviceLimit);
    // 只读部署代际提示，不加载 DLL；不代表设备/驱动可用性或内容摘要。
    static std::uint64_t GetBackendStamp() noexcept;
    bool Read(const double *points, std::size_t count, SurfaceCudaSample *output);
    // 缓冲由 session 所有，只在创建线程访问；提交后索引不可再写，收集后样本可读到下次 Prepare。
    double *Prepare(unsigned slot);
    bool Submit(unsigned slot, std::size_t count, SurfaceCudaTicket &ticket);
    const SurfaceCudaSample *Collect(const SurfaceCudaTicket &ticket);
    // 仅仓内测试后端提供探针：1..3 为计算故障，4..6 为计时故障，7 为 slab 部分搬移中断。
    bool SetTestFault(unsigned fault);
    SurfaceCudaProfile GetProfile() const;
    bool GetActive() const noexcept { return m_session != nullptr; }
    void Reset() noexcept;
    std::size_t GetDeviceBytes() const noexcept { return m_deviceBytes; }
    std::size_t GetHostBytes() const noexcept {return m_hostBytes;}

  private:
    void *m_library = nullptr;
    void *m_session = nullptr;
    SurfaceCudaRead m_read = nullptr;
    SurfaceCudaPrepareCallback m_prepare = nullptr;
    SurfaceCudaSubmitCallback m_submit = nullptr;
    SurfaceCudaCollectCallback m_collect = nullptr;
    SurfaceCudaTestFaultCallback m_testFault = nullptr;
    SurfaceCudaProfileCallback m_profile = nullptr;
    SurfaceCudaClose m_close = nullptr;
    std::size_t m_deviceBytes = 0,m_hostBytes=0;
    std::uint64_t m_epoch = 0;
};
