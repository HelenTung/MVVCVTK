#pragma once
#include "AppInterfaces.h"
#include <vector>
#include <mutex>
#include <array>

struct ReconBuffer {
    std::vector<float>    data;                // 拷贝自重建输出的体素数据
    std::array<int, 3>    dims = { 0, 0, 0 };
    std::array<float, 3>  spacing = { 1.f, 1.f, 1.f };
    std::array<float, 3>  origin = { 0.f, 0.f, 0.f };
};


class BaseDataManager : public AbstractDataManager
{
protected:
	mutable std::mutex m_dataMutex;
	vtkSmartPointer<vtkImageData> m_vtkImage;
public:
    BaseDataManager() {
        m_vtkImage = vtkSmartPointer<vtkImageData>::New();
    }

    vtkSmartPointer<vtkImageData> GetVtkImage() const override {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        return m_vtkImage;
	}

    bool SetTransformedDataSaved(const std::string& filePath, const std::array<double, 16>& transformMatrix) override;
};

class RawVolumeDataManager : public BaseDataManager {
private:
    int m_dims[3] = { 0, 0, 0 };
    double m_spacing = 0.02125;

    // ── 重建注入路径（前后处理分离）──────────────────────────────────
    mutable std::mutex            m_reconMutex;
    vtkSmartPointer<vtkImageData> m_pendingImage;   // 后台线程写，主线程读
    std::atomic<bool>             m_hasPendingImage{ false };

public:
    RawVolumeDataManager() = default;
    bool SetDataLoaded(const std::string& filePath) override;
    bool SetFromBuffer(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) override;
    bool ConsumeReconImage();
};


class TiffVolumeDataManager : public BaseDataManager {
public:
    TiffVolumeDataManager() = default;
    // 实现加载接口
    bool SetDataLoaded(const std::string& filePath) override;
};
