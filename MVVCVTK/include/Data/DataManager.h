#pragma once
#include "Data/DataService.h"
#include <array>
#include <memory>
#include <string>

class BaseDataManager : public AbstractDataManager
{
protected:
    class Impl;
    // 随 BaseDataManager 生命周期独占 current image、range、spacing、version 真源及其事务锁。
    std::unique_ptr<Impl> m_impl;

    // 提交由派生类独占构造的 image，避免 TIFF 读取完成后再次复制整卷体素。
    bool SetOwnedImage(vtkSmartPointer<vtkImageData> image);
    bool SetPendingImage(TrustedImageState image);
public:
    BaseDataManager();
    ~BaseDataManager() override;

    TrustedImageSnapshot GetImageSnapshot() const override;
    // 仅当 current 仍是 expectedSnapshot 时原子发布 image+mask 新批次；
    // publishedSnapshot 在同一锁内返回实际发布的 owner，禁止提交后再读 current 猜测结果。
    bool SetCurrentData(
        TrustedImageState state,
        const TrustedImageSnapshot& expectedSnapshot,
        TrustedImageSnapshot& publishedSnapshot) override;
    vtkSmartPointer<vtkImageData> GetVtkImage() const override;

    std::array<double, 2> GetScalarRange() const override;
    std::array<double, 3> GetSpacing() const override;
    // 原子更新 current image 与 TrustedImageState 的 RAS spacing；值未变化时不递增 version。
    bool SetSpacing(const std::array<double, 3>& spacing) override;
    DataVersion GetDataVersion() const override;
    TrustedImageState GetImageState() const override;
    std::optional<ImageReadState> GetImageReadState() const override;
    ImageReadResult GetImageReadResult(
        std::size_t maxReadBytes = imageReadLimit) const override;
    ImageReadResult GetImageReadResult(
        const ImageReadRequest& request,
        const TaskStopToken& stopToken) const override;
    ImageReadChunkResult GetImageReadChunk(
        const ImageReadRequest& request,
        std::size_t voxelOffset,
        const TaskStopToken& stopToken) const override;

    // 基础/TIFF 数据源不支持 buffer pending 事务；RawVolumeDataManager 显式覆盖这两个入口。
    bool SetFromBuffer(const VolumeBuffer& buffer) override;
    bool SetCurrentFromPending(bool& hasPending) override;
    TrustedImageSnapshot GetPendingSnapshot() const override;
    bool SetCurrentFromPending(
        const TrustedImageSnapshot& expectedPending,
        TrustedImageSnapshot& publishedSnapshot) override;
    bool ClearPending() override;

    bool ExportData(
        const TrustedImageSnapshot& imageSnapshot,
        const std::string& outputDir,
        const DataExportParams& params) override;
    bool ExportData(
        const TrustedImageSnapshot& imageSnapshot,
        const std::string& outputDir,
        const DataExportParams& params,
        const TaskStopToken& stopToken) override;
    // dirPath 为 UTF-8 路径。
    bool ExportSlices(const std::string& dirPath, Orientation orientation, const WindowLevelParams& windowLevel, const std::array<double, 16>& modelToWorldMatrix) override;
    bool ExportSlices(
        const std::string& dirPath,
        Orientation orientation,
        const WindowLevelParams& windowLevel,
        const std::array<double, 16>& modelToWorldMatrix,
        const TaskStopToken& stopToken) override;
};

class RawVolumeDataManager : public BaseDataManager {
public:
    RawVolumeDataManager();
    ~RawVolumeDataManager() override;
    // filePath 为 UTF-8 路径。
    bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout) override;
    bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout,
        const TaskStopToken& stopToken) override;
    // 后台把调用期间有效的连续 float 缓冲复制为 pending image；输入 spacing/origin 属于 LPS 物理空间。
    bool SetFromBuffer(const VolumeBuffer& buffer) override;
    bool SetFromBuffer(
        const VolumeBuffer& buffer,
        const TaskStopToken& stopToken) override;
    // 深拷贝调用方 image 为 pending 隔离批次，不直接提交 current。
    bool SetImageSnapshot(vtkSmartPointer<vtkImageData> image);
    // owner 线程在所有 View 候选提交后，将同一个 pending owner 原子发布为 current。
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
    bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout,
        const TaskStopToken& stopToken) override;

private:
    class Impl;
    // 随 TiffVolumeDataManager 生命周期独占 TIFF 读取与 LPS/RAS 转换实现，不持有 Base current 真源。
    std::unique_ptr<Impl> m_impl;
};
