#pragma once
#include "AppInterfaces.h"
#include <vector>
#include <array>
#include <memory>
#include <string>

struct ReconBuffer {
    std::vector<float>    data; // 拷贝自重建输出的连续 float 体素
    std::array<int, 3>    dims = { 0, 0, 0 }; // LPS 输入的 [X, Y, Z] 体素数
    std::array<float, 3>  spacing = { 1.f, 1.f, 1.f }; // LPS 物理轴体素间距，单位沿用上游
    std::array<float, 3>  origin = { 0.f, 0.f, 0.f }; // LPS 物理原点，提交到 VTK 前统一转 RAS
};

// image 与 version 由 current 事务同锁快照；返回的智能指针不冻结后续 VTK 对象内容。
struct ImageState {
    vtkSmartPointer<vtkImageData> image;
    DataVersion version = 0;
};

class BaseDataManager : public AbstractDataManager
{
protected:
    class Impl;
    // Base Impl 独占 current image、range、spacing、version 真源及其事务锁。
    std::unique_ptr<Impl> m_impl;

    // 文件/TIFF 同步加载路径直接提交 current 四元组；空 image 不改变现有真源。
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
    // 后台把调用期间有效的连续 float 缓冲复制为 pending image；输入 spacing/origin 属于 LPS 物理空间。
    bool SetFromBuffer(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) override;
    // 把现成 VTK image 智能指针移入 pending 事务，不做深拷贝，也不提交 current。
    bool SetImageSnapshot(vtkSmartPointer<vtkImageData> image);
    // 主线程领取一批 pending payload，触发 Modified() 后在同一临界区提交 current 四元组。
    bool SetCurrentFromPending() override;

private:
    class Impl;
    // Raw Impl 只拥有 pending image、派生元数据、门铃及其事务锁，不复制 Base Impl 的 current 状态。
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
