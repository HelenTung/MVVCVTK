#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#if defined(MVVCVTK_GAP_KERNEL_EXPORTS)
#define MVVCVTK_GAP_KERNEL_API __declspec(dllexport)
#else
#define MVVCVTK_GAP_KERNEL_API __declspec(dllimport)
#endif

#define MVVCVTK_GAP_KERNEL_CALL __cdecl

inline constexpr std::uint32_t GapKernelAbiVersion = 6;

// GapAnalysis 与私有算法 DLL 之间只交换固定宽度 POD 和调用方持有的原始缓冲区；
// 供应商 C++/STL/VTK 类型不得越过此边界；掩膜路径由 bridge 生成有效域派生结果。
using GapKernelCancel = std::int32_t (MVVCVTK_GAP_KERNEL_CALL*)(void*) noexcept;
struct GapKernelRequest final {
    std::uint32_t abiVersion;
    std::uint32_t structSize;
    const float* voxelData;
    std::uint64_t voxelCount;
    std::int32_t dims[3];
    std::int32_t extent[6];
    double spacing[3];
    double origin[3];
    double direction[9];
    float backgroundMean;
    float materialMean;
    float isoThreshold;
    std::int32_t numberRuns;
    const std::uint8_t* validityMask;
    std::uint64_t validityCount;
    // 同步调用期间可重入的只读取消查询；无锁、不修改调用方状态，context 活到返回。
    GapKernelCancel getCancelled;
    void* cancelContext;
    std::uint32_t reserved[8];
};

struct GapKernelHeader final {
    std::int32_t regionCount;
    std::int32_t dims[3];
    std::int64_t totalVoxelCount;
    float materialMean;
    float materialStd;
    float airMean;
    float airStd;
    float computedThreshold;
    std::int32_t algorithmMode;
    float materialVolumeMM3;
    float defectVolumeMM3;
    float defectVolumeRatio;
    float objectMean;
    float objectStd;
};

struct GapKernelRegion final {
    std::int32_t id;
    std::int64_t voxelCount;
    float volumeMM3;
    float equivalentDiameterMM;
    float radiusMM;
    float diameterMM;
    double centerMM[3];
    std::int32_t centroidMM[3];
    std::int32_t bbox[6];
    std::int32_t seedVoxel[3];
    float minGray;
    float maxGray;
    float meanGray;
    float stdDevGray;
    float grayDeviation;
    float gapMM;
    float compactness;
    float surfaceAreaMM2;
    float sphericity;
    float pcaDeviation1;
    float pcaDeviation2;
    float pcaDeviation3;
    float pcaMaxDeviationRatio;
    float pcaMinDeviationRatio;
    float projectedAreaXMM2;
    float projectedAreaYMM2;
    float projectedAreaZMM2;
    float projectedSizeX;
    float projectedSizeY;
    float projectedSizeZ;
    float defectProbability;
    std::uint32_t boundaryStatus;
    std::uint32_t hasWholeObjectGeometry;
};

using GapKernelLabelOwnerClone = void* (
    MVVCVTK_GAP_KERNEL_CALL*)(const void* owner) noexcept;
using GapKernelLabelOwnerRelease = void (
    MVVCVTK_GAP_KERNEL_CALL*)(void* owner) noexcept;

// header/regions 仅在同步 callback 返回前有效。labels 可在 callback 内克隆 opaque owner；
// 克隆成功后 labels 保持有效，直到调用方用配对 release 释放 owner。
struct GapKernelResultView final {
    std::uint32_t abiVersion;
    std::uint32_t structSize;
    std::uint32_t headerSize;
    std::uint32_t regionSize;
    const GapKernelHeader* header;
    const GapKernelRegion* regions;
    std::uint64_t regionCount;
    const std::int32_t* labels;
    std::uint64_t labelCount;
    const void* labelOwner;
    GapKernelLabelOwnerClone cloneLabelOwner;
    GapKernelLabelOwnerRelease releaseLabelOwner;
};

using GapKernelResultSink = std::int32_t (
    MVVCVTK_GAP_KERNEL_CALL*)(
        const GapKernelResultView* result,
        void* context) noexcept;

using GapKernelEntry = std::int32_t (
    MVVCVTK_GAP_KERNEL_CALL*)(
        const GapKernelRequest* request,
        GapKernelResultSink sink,
        void* context) noexcept;

extern "C" MVVCVTK_GAP_KERNEL_API std::int32_t
MVVCVTK_GAP_KERNEL_CALL BuildGapResult(
    const GapKernelRequest* request,
    GapKernelResultSink sink,
    void* context) noexcept;

static_assert(std::is_standard_layout_v<GapKernelRequest>);
static_assert(std::is_trivially_copyable_v<GapKernelRequest>);
static_assert(sizeof(GapKernelRequest) == 264);
static_assert(offsetof(GapKernelRequest, numberRuns) == 196);
static_assert(std::is_standard_layout_v<GapKernelHeader>);
static_assert(std::is_trivially_copyable_v<GapKernelHeader>);
static_assert(sizeof(GapKernelHeader) == 72);
static_assert(std::is_standard_layout_v<GapKernelRegion>);
static_assert(std::is_trivially_copyable_v<GapKernelRegion>);
static_assert(sizeof(GapKernelRegion) == 200);
static_assert(std::is_standard_layout_v<GapKernelResultView>);
static_assert(std::is_trivially_copyable_v<GapKernelResultView>);
static_assert(sizeof(GapKernelResultView) == 80);
static_assert(offsetof(GapKernelResultView, labelOwner) == 56);
