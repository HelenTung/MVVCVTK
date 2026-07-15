#pragma once
#include "AppInterfaces.h"
#include <vector>
#include <array>
#include <memory>
#include <string>

struct ReconBuffer {
    std::vector<float>    data; // LPS 连续体素，布局为 [z][y][x]、X 最快，期望元素数为 X*Y*Z
    std::array<int, 3>    dims = { 0, 0, 0 }; // LPS [X, Y, Z] 体素数；任一维为 0 表示空 buffer
    std::array<float, 3>  spacing = { 1.f, 1.f, 1.f }; // LPS [sx, sy, sz] 物理间距，单位沿用上游
    std::array<float, 3>  origin = { 0.f, 0.f, 0.f }; // LPS [x, y, z] 物理原点，提交 VTK 前转为 RAS
};

class BaseDataManager : public AbstractDataManager
{
protected:
    friend class VizService;
    friend class HostFeatureBindings;
    class Impl;
    // 随 BaseDataManager 生命周期独占 current image、range、spacing、version 真源及其事务锁。
    std::unique_ptr<Impl> m_impl;

    // 提交由派生类独占构造的 image，避免 TIFF 读取完成后再次复制整卷体素。
    bool SetOwnedImage(vtkSmartPointer<vtkImageData> image);
    ImageSnapshot GetImageSnapshot() const override;

public:
    BaseDataManager();
    ~BaseDataManager() override;

    vtkSmartPointer<vtkImageData> GetVtkImage() const override;

    std::array<double, 2> GetScalarRange() const override;
    std::array<double, 3> GetSpacing() const override;
    // 原子更新 current image 与 ImageState 的 RAS spacing；值未变化时不递增 version。
    bool SetSpacing(const std::array<double, 3>& spacing) override;
    DataVersion GetDataVersion() const override;
    ImageState GetImageState() const override;

    // 基础/TIFF 数据源不支持 buffer pending 事务；RawVolumeDataManager 显式覆盖这两个入口。
    bool SetFromBuffer(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) override;
    bool SetCurrentFromPending() override;

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
    // 深拷贝调用方 image 为 pending 隔离批次，不直接提交 current。
    bool SetImageSnapshot(vtkSmartPointer<vtkImageData> image);
    // 具备 VTK pipeline 写权限的消费线程领取 pending payload，触发 Modified() 后提交完整 current 图像状态。
    bool SetCurrentFromPending() override;

private:
    class Impl;
    // 随 RawVolumeDataManager 生命周期独占 pending image、派生元数据、门铃及其事务锁；
    // 后台生产完整批次，消费线程接管后清门铃，不复制 Base Impl 的 current 状态。
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
    // 随 TiffVolumeDataManager 生命周期独占 TIFF 读取与 LPS/RAS 转换实现，不持有 Base current 真源。
    std::unique_ptr<Impl> m_impl;
};
