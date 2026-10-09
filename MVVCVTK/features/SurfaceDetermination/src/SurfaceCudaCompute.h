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
    void Reset() noexcept;
    std::size_t GetDeviceBytes() const noexcept { return m_deviceBytes; }

  private:
    void *m_library = nullptr;
    void *m_session = nullptr;
    SurfaceCudaRead m_read = nullptr;
    SurfaceCudaClose m_close = nullptr;
    std::size_t m_deviceBytes = 0;
};
