#pragma once
#include "AppInterfaces.h"
#include <array>
#include <memory>
#include <string>

class BaseDataManager : public AbstractDataManager
{
protected:
    friend class VizService;
    friend struct HostCoreServices;
    class Impl;
    // 随 BaseDataManager 生命周期独占 current image、range、spacing、version 真源及其事务锁。
    std::unique_ptr<Impl> m_impl;

    // 提交由派生类独占构造的 image，避免 TIFF 读取完成后再次复制整卷体素。
    bool SetOwnedImage(vtkSmartPointer<vtkImageData> image);
    bool SetPendingImage(ImageState image);
    // 仅当 current 仍是 expectedSnapshot 时原子发布 image+mask 新批次；
    // publishedSnapshot 在同一锁内返回实际发布的 owner，禁止提交后再读 current 猜测结果。
    bool SetCurrentData(
        ImageState state,
        const ImageSnapshot& expectedSnapshot,
        ImageSnapshot& publishedSnapshot);
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
    bool SetFromBuffer(const VolumeBuffer& buffer) override;
    bool SetCurrentFromPending(bool& hasPending) override;
    bool ClearPending() override;

    // filePath/dirPath 均为 UTF-8 路径。
    bool ExportData(const std::string& filePath, const std::array<double, 16>& modelToWorldMatrix) override;
    bool ExportSlices(const std::string& dirPath, Orientation orientation, const WindowLevelParams& windowLevel, const std::array<double, 16>& modelToWorldMatrix) override;
};

class RawVolumeDataManager : public BaseDataManager {
public:
    RawVolumeDataManager();
    ~RawVolumeDataManager() override;
    // filePath 为 UTF-8 路径。
    bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout) override;
    // 后台把调用期间有效的连续 float 缓冲复制为 pending image；输入 spacing/origin 属于 LPS 物理空间。
    bool SetFromBuffer(const VolumeBuffer& buffer) override;
    // 深拷贝调用方 image 为 pending 隔离批次，不直接提交 current。
    bool SetImageSnapshot(vtkSmartPointer<vtkImageData> image);
    // 具备 VTK pipeline 写权限的消费线程领取 pending payload，触发 Modified() 后提交完整 current 图像状态。
    bool SetCurrentFromPending(bool& hasPending) override;
    bool ClearPending() override;

};

class TiffVolumeDataManager : public BaseDataManager {
public:
    TiffVolumeDataManager();
    ~TiffVolumeDataManager() override;
    // filePath 为 UTF-8 路径。
    bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout) override;

private:
    class Impl;
    // 随 TiffVolumeDataManager 生命周期独占 TIFF 读取与 LPS/RAS 转换实现，不持有 Base current 真源。
    std::unique_ptr<Impl> m_impl;
};
