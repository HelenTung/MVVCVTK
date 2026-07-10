#pragma once
#include "AppInterfaces.h"
#include <vector>
#include <array>
#include <memory>
#include <string>

struct ReconBuffer {
    std::vector<float>    data;                // 拷贝自重建输出的体素数据
    std::array<int, 3>    dims = { 0, 0, 0 };
    std::array<float, 3>  spacing = { 1.f, 1.f, 1.f };
    std::array<float, 3>  origin = { 0.f, 0.f, 0.f }; // 上游 ITK/LPS 物理原点，提交到 VTK 前统一转 RAS
};

struct ImageState {
    vtkSmartPointer<vtkImageData> image;
    DataVersion version = 0;
};

class BaseDataManager : public AbstractDataManager
{
protected:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    bool SetCurrentImage(vtkSmartPointer<vtkImageData> image);

public:
    BaseDataManager();
    ~BaseDataManager() override;

    vtkSmartPointer<vtkImageData> GetVtkImage() const override;

    std::array<double, 2> GetScalarRange() const override;
    std::array<double, 3> GetSpacing() const override;
    DataVersion GetDataVersion() const override;
    ImageState GetImageState() const;

    bool ExportData(const std::string& filePath, const std::array<double, 16>& modelToWorldMatrix) override;
    bool ExportSlices(const std::string& dirPath, Orientation orientation, const WindowLevelParams& windowLevel, const std::array<double, 16>& modelToWorldMatrix) override;
};

class RawVolumeDataManager : public BaseDataManager {
public:
    RawVolumeDataManager();
    ~RawVolumeDataManager() override;
    bool SetDataLoaded(const std::string& filePath,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) override;
    bool SetFromBuffer(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) override;
    bool SetImageSnapshot(vtkSmartPointer<vtkImageData> image);
    bool SetCurrentFromPending() override;

private:
    class Impl;
    std::unique_ptr<Impl> m_rawImpl;
};

class TiffVolumeDataManager : public BaseDataManager {
public:
    TiffVolumeDataManager();
    ~TiffVolumeDataManager() override;
    // 实现加载接口
    bool SetDataLoaded(const std::string& filePath,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
