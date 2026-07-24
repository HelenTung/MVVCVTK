#pragma once
// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/include/Algorithms/VolumeBuffer.h
// VolumeBuffer.h - 纯体素快照访问层（无文件加载，无 I/O）
//
// =====================================================================

#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

// Gap 算法使用的只读体素快照：既支持自有 vector，也支持由 shared owner 锚定的零拷贝 float 别名。
// copy/move 会重新绑定 owned 模式的 data 指针，避免 vector 搬迁后 voxelsPtr 指向旧地址。
class GapVolumeBuffer {
public:
    GapVolumeBuffer() = default;

    GapVolumeBuffer(const GapVolumeBuffer& other)
        : voxels(other.voxels),
          voxelsPtr(nullptr),
          validMask(other.validMask),
          validMaskPtr(nullptr),
          dims(other.dims),
          spacing(other.spacing),
          origin(other.origin),
          minVal(other.minVal),
          maxVal(other.maxVal),
          m_voxelOwner(other.m_voxelOwner),
          m_maskOwner(other.m_maskOwner) {
        voxelsPtr = m_voxelOwner ? other.voxelsPtr : GetOwnedPointer();
        validMaskPtr = m_maskOwner
            ? other.validMaskPtr
            : GetOwnedMaskPointer();
    }

    GapVolumeBuffer& operator=(const GapVolumeBuffer& other) {
        if (this == &other) {
            return *this;
        }

        GapVolumeBuffer copy(other);
        *this = std::move(copy);
        return *this;
    }

    GapVolumeBuffer(GapVolumeBuffer&& other) noexcept
        : voxels(std::move(other.voxels)),
          voxelsPtr(other.voxelsPtr),
          validMask(std::move(other.validMask)),
          validMaskPtr(other.validMaskPtr),
          dims(other.dims),
          spacing(other.spacing),
          origin(other.origin),
          minVal(other.minVal),
          maxVal(other.maxVal),
          m_voxelOwner(std::move(other.m_voxelOwner)),
          m_maskOwner(std::move(other.m_maskOwner)) {
        if (!m_voxelOwner) {
            voxelsPtr = GetOwnedPointer();
        }
        if (!m_maskOwner) {
            validMaskPtr = GetOwnedMaskPointer();
        }
        other.voxelsPtr = nullptr;
        other.validMaskPtr = nullptr;
    }

    GapVolumeBuffer& operator=(GapVolumeBuffer&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        voxels = std::move(other.voxels);
        voxelsPtr = other.voxelsPtr;
        validMask = std::move(other.validMask);
        validMaskPtr = other.validMaskPtr;
        dims = other.dims;
        spacing = other.spacing;
        origin = other.origin;
        minVal = other.minVal;
        maxVal = other.maxVal;
        m_voxelOwner = std::move(other.m_voxelOwner);
        m_maskOwner = std::move(other.m_maskOwner);
        if (!m_voxelOwner) {
            voxelsPtr = GetOwnedPointer();
        }
        if (!m_maskOwner) {
            validMaskPtr = GetOwnedMaskPointer();
        }
        other.voxelsPtr = nullptr;
        other.validMaskPtr = nullptr;
        return *this;
    }

    // 转换路径独占 float vector；切换所有权模式时必须先释放旧的共享 owner。
    void SetOwnedVoxels(std::vector<float> ownedVoxels) noexcept {
        m_voxelOwner.reset();
        voxels = std::move(ownedVoxels);
        voxelsPtr = GetOwnedPointer();
    }

    // 已经是 float 的输入只保存只读别名；调用方必须保证 sharedVoxels 指向 voxelOwner 管理的存储，
    // 且该存储至少覆盖 dims 乘积个 float。本函数只检查两个参数非空，无法验证归属或容量。
    bool SetSharedVoxels(
        std::shared_ptr<const void> voxelOwner,
        const float* sharedVoxels) noexcept {
        if (!voxelOwner || !sharedVoxels) {
            return false;
        }

        std::vector<float>().swap(voxels);
        m_voxelOwner = std::move(voxelOwner);
        voxelsPtr = sharedVoxels;
        return true;
    }

    // 非空有效域与体素共享 x-fast 布局；0 表示分析域外，非 0 表示有效。
    void SetOwnedMask(std::vector<std::uint8_t> ownedMask) noexcept {
        m_maskOwner.reset();
        validMask = std::move(ownedMask);
        validMaskPtr = GetOwnedMaskPointer();
    }

    bool SetSharedMask(
        std::shared_ptr<const void> maskOwner,
        const std::uint8_t* sharedMask) noexcept {
        if (!maskOwner || !sharedMask) {
            return false;
        }

        std::vector<std::uint8_t>().swap(validMask);
        m_maskOwner = std::move(maskOwner);
        validMaskPtr = sharedMask;
        return true;
    }

    // 空 mask 表示整卷有效；清理时必须同时释放别名 owner。
    void ClearMask() noexcept {
        std::vector<std::uint8_t>().swap(validMask);
        m_maskOwner.reset();
        validMaskPtr = nullptr;
    }

    bool GetVoxelReady() const noexcept {
        // 只检查别名是否非空；dims 与 vector size 的一致性由快照生产方保证。
        return voxelsPtr != nullptr;
    }

    bool GetVoxelValid(std::size_t index) const noexcept {
        return !validMaskPtr || validMaskPtr[index] != 0;
    }

    // owned 模式的 x-fast 连续体素 owner，offset = x + y*dims[0] + z*dims[0]*dims[1]。
    // 直接改变该 public vector 的 size/capacity 会绕过别名重绑；生产方应通过 SetOwnedVoxels 或赋值运算更新。
    std::vector<float> voxels;
    // 非拥有只读别名；owned 模式指向 voxels，shared 模式由 m_voxelOwner 保证生命周期。
    const float* voxelsPtr = nullptr;
    // 空 vector/空别名表示整卷有效；非空时与 voxels 使用同一 x-fast offset。
    std::vector<std::uint8_t> validMask;
    const std::uint8_t* validMaskPtr = nullptr;

    // voxel 数量 [dimX, dimY, dimZ]；有效快照要求三项为正且 voxelsPtr 至少覆盖三者乘积。
    std::array<int, 3>     dims = { 0, 0, 0 };
    // 相邻 voxel 的轴向 physical 间距 [x, y, z]；Gap 统计按 mm 解释。
    std::array<double, 3>  spacing = { 1.0, 1.0, 1.0 };
    // index [0,0,0] 的输入 physical 原点 [x, y, z]，按 mm 解释；快照不保存 image direction。
    std::array<double, 3>  origin = { 0.0, 0.0, 0.0 };
    // 输入标量域闭区间；显示请求用它解析 DataRangeRatio ISO。
    float                 minVal = 0.f;
    float                 maxVal = 0.f;


    // ── 安全取值（含边界检查）───────────────────────────────────────
    inline float GetVoxelValue(int x, int y, int z) const noexcept {
        if (!voxelsPtr || x < 0 || x >= dims[0] ||
            y < 0 || y >= dims[1] ||
            z < 0 || z >= dims[2]) return 0.f;
        return voxelsPtr[(size_t)x + (size_t)y * dims[0] + (size_t)z * (size_t)dims[0] * dims[1]];
    }

    // ── 三线性插值：体素索引坐标 ─────────────────────────────────────
    // 需要完整 2x2x2 邻域；采样点落在体外或正好位于最外层上边界时返回 0，不做边缘钳制。
    inline float GetTrilinearValueByIndex(float fx, float fy, float fz) const noexcept {
        int x0 = static_cast<int>(std::floor(fx));
        int y0 = static_cast<int>(std::floor(fy));
        int z0 = static_cast<int>(std::floor(fz));
        int x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
        if (x0 < 0 || y0 < 0 || z0 < 0 ||
            x1 >= dims[0] || y1 >= dims[1] || z1 >= dims[2]) return 0.f;

        float tx = fx - x0, ty = fy - y0, tz = fz - z0;
        float c00 = GetVoxelValue(x0, y0, z0) * (1 - tx) + GetVoxelValue(x1, y0, z0) * tx;
        float c01 = GetVoxelValue(x0, y0, z1) * (1 - tx) + GetVoxelValue(x1, y0, z1) * tx;
        float c10 = GetVoxelValue(x0, y1, z0) * (1 - tx) + GetVoxelValue(x1, y1, z0) * tx;
        float c11 = GetVoxelValue(x0, y1, z1) * (1 - tx) + GetVoxelValue(x1, y1, z1) * tx;
        float c0 = c00 * (1 - ty) + c10 * ty;
        float c1 = c01 * (1 - ty) + c11 * ty;
        return c0 * (1 - tz) + c1 * tz;
    }

    // ── 三线性插值：轴对齐 input physical 坐标 (mm)；不应用 VTK direction matrix ──
    // 有效快照要求 spacing 三分量非零；本访问层信任生产方，不在每次采样时重复校验。
    inline float GetTrilinearValueByWorld(double x, double y, double z) const noexcept {
        return GetTrilinearValueByIndex(
            static_cast<float>((x - origin[0]) / spacing[0]),
            static_cast<float>((y - origin[1]) / spacing[1]),
            static_cast<float>((z - origin[2]) / spacing[2]));
    }

private:
    const float* GetOwnedPointer() const noexcept {
        return voxels.empty() ? nullptr : voxels.data();
    }

    const std::uint8_t* GetOwnedMaskPointer() const noexcept {
        return validMask.empty() ? nullptr : validMask.data();
    }

    // shared 模式的类型擦除生命周期锚点；owned 模式为空并由 voxels 唯一持有数据。
    std::shared_ptr<const void> m_voxelOwner;
    std::shared_ptr<const void> m_maskOwner;
};
