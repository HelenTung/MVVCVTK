#include "AlignmentReference.h"
#include "AlignmentMath.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <charconv>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace {
constexpr std::size_t readBytes = 65536;
constexpr auto emptySlot = std::numeric_limits<std::uint64_t>::max();

void CheckWork(const AlignmentWork &work) {
    if (AlignmentGeometryFit::GetWorkStatus(work) != AlignmentStatus::FullyDetermined)
        throw std::runtime_error("Reference import cancelled.");
}
void Require(bool valid, const char *message) {
    if (!valid) throw std::invalid_argument(message);
}
void CheckHash(NTSTATUS status) {
    if (status < 0) throw std::system_error(std::make_error_code(std::errc::io_error), "SHA256 failed");
}
class File final {
  public:
    explicit File(const std::string &path) {
        m_handle = CreateFileW(std::filesystem::u8path(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "STL open failed");
    }
    ~File() { CloseHandle(m_handle); }
    HANDLE Get() const { return m_handle; }
  private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};
class Hash final {
  public:
    Hash() {
        CheckHash(BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        const auto status = BCryptCreateHash(m_algorithm, &m_hash, nullptr, 0, nullptr, 0, 0);
        if (status < 0) { BCryptCloseAlgorithmProvider(m_algorithm, 0); m_algorithm = nullptr; CheckHash(status); }
    }
    ~Hash() { if (m_hash) BCryptDestroyHash(m_hash); if (m_algorithm) BCryptCloseAlgorithmProvider(m_algorithm, 0); }
    void Add(unsigned char *data, ULONG size) { CheckHash(BCryptHashData(m_hash, data, size, 0)); }
    std::string Finish() {
        std::array<unsigned char, 32> bytes{};
        CheckHash(BCryptFinishHash(m_hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0));
        constexpr char hex[] = "0123456789abcdef";
        std::string text;
        text.reserve(64);
        for (const auto value : bytes) { text.push_back(hex[value >> 4]); text.push_back(hex[value & 15]); }
        return text;
    }
  private:
    BCRYPT_ALG_HANDLE m_algorithm = nullptr;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
};
std::uint32_t UInt32(const unsigned char *p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
class Reader final {
  public:
    Reader(const std::string &path, const AlignmentWork &work) : m_file(path), m_work(work) {
        LARGE_INTEGER size{};
        if (GetFileType(m_file.Get()) != FILE_TYPE_DISK || !GetFileSizeEx(m_file.Get(), &size) || size.QuadPart <= 0)
            throw std::system_error(std::make_error_code(std::errc::io_error), "STL is not a nonempty regular file");
        m_size = static_cast<std::uint64_t>(size.QuadPart);
        std::array<unsigned char, 84> header{};
        DWORD count = 0;
        if (!ReadFile(m_file.Get(), header.data(), static_cast<DWORD>(header.size()), &count, nullptr))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "STL header read failed");
        // 长度与面数吻合时按二进制读取，允许二进制头以 solid 开始。
        m_binary = count == header.size() && 84ULL + 50ULL * UInt32(header.data() + 80) == m_size;
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(m_file.Get(), beginning, nullptr, FILE_BEGIN))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "STL seek failed");
    }
    int Byte() {
        if (m_position == m_available && !Fill()) return -1;
        return m_buffer[m_position++];
    }
    void Read(unsigned char *out, std::size_t size) {
        while (size) {
            if (m_position == m_available) Require(Fill(), "Truncated binary STL.");
            const auto count = std::min(size, m_available - m_position);
            std::memcpy(out, m_buffer.data() + m_position, count);
            m_position += count; out += count; size -= count;
        }
    }
    std::string Word() {
        std::string text;
        int value;
        do { value = Byte(); } while (value >= 0 && std::isspace(static_cast<unsigned char>(value)));
        while (value >= 0 && !std::isspace(static_cast<unsigned char>(value))) {
            Require(text.size() < 128, "ASCII STL token is too long.");
            text.push_back(static_cast<char>(value)); value = Byte();
        }
        m_lastSeparator = value;
        return text;
    }
    void LineEnd() {
        if (m_lastSeparator == '\n' || m_lastSeparator < 0) return;
        int value;
        do { value = Byte(); } while (value >= 0 && value != '\n');
    }
    void Expect(const char *word) { Require(Word() == word, "Invalid ASCII STL structure."); }
    double Number() {
        const auto text = Word();
        double value = 0;
        // from_chars 不读取全局 locale；显式接受 STL 常见的前导正号。
        const auto first = text.data() + (!text.empty() && text.front() == '+' ? 1 : 0);
        const auto result = std::from_chars(first, text.data() + text.size(), value);
        Require(result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value),
                "Invalid STL number.");
        return value;
    }
    bool GetBinary() const { return m_binary; }
    std::uint64_t GetSize() const { return m_size; }
    std::string Finish() { Require(m_read == m_size, "STL byte count changed."); return m_hash.Finish(); }
  private:
    bool Fill() {
        CheckWork(m_work);
        DWORD count = 0;
        const auto remaining = m_size - m_read;
        const auto wanted = static_cast<DWORD>(std::min<std::uint64_t>(remaining, m_buffer.size()));
        if (!wanted) {
            unsigned char extra = 0;
            if (!ReadFile(m_file.Get(), &extra, 1, &count, nullptr))
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "STL EOF check failed");
            Require(count == 0, "STL length changed during import.");
            return false;
        }
        if (!ReadFile(m_file.Get(), m_buffer.data(), wanted, &count, nullptr))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "STL read failed");
        if (!count) return false;
        m_hash.Add(m_buffer.data(), count);
        m_read += count;
        m_available = count;
        m_position = 0;
        return true;
    }
    File m_file;
    Hash m_hash;
    const AlignmentWork &m_work;
    std::array<unsigned char, readBytes> m_buffer{};
    std::size_t m_position = 0, m_available = 0;
    std::uint64_t m_size = 0, m_read = 0;
    int m_lastSeparator = -1;
    bool m_binary = false;
};

class MeshBuilder final {
  public:
    MeshBuilder(const AlignmentWork &work, double scale) : m_work(work), m_scale(scale) { CheckBytes(0); }
    void Add(std::array<AlignmentPoint, 3> points) {
        CheckWork(m_work);
        for (auto &point : points) for (auto &value : point) {
            value *= m_scale;
            Require(std::isfinite(value), "STL unit conversion overflows.");
            if (value == 0) value = 0; // 统一正负零，不改变几何坐标。
        }
        const auto cross = (AlignmentMath::Vector(points[1]) - AlignmentMath::Vector(points[0])).cross(
                            AlignmentMath::Vector(points[2]) - AlignmentMath::Vector(points[0]));
        const auto area2 = cross.dot(cross);
        Require(std::isfinite(area2) && area2 > 0, "STL contains a degenerate triangle.");
        Grow(m_triangles, m_triangles.size() + 3);
        for (const auto &point : points) m_triangles.push_back(Vertex(point));
    }
    std::shared_ptr<const SurfaceMeshPayload> Finish(const std::string &frame) {
        Require(!m_triangles.empty(), "STL contains no triangles.");
        std::vector<std::uint64_t>().swap(m_slots);
        return std::make_shared<const SurfaceMeshPayload>(std::move(m_vertices), std::move(m_triangles),
                                                         std::vector<MeshAttribute>{}, frame);
    }
  private:
    void CheckBytes(std::size_t extra) const {
        const auto limit = std::numeric_limits<std::size_t>::max();
        std::size_t used = readBytes;
        if (used > limit) throw std::length_error("STL input buffer size accounting overflows.");
        for (const auto count : {m_vertices.capacity(), m_triangles.capacity(), m_slots.capacity()}) {
            if (count > (limit - used) / sizeof(std::uint64_t))
                throw std::length_error("STL mesh size accounting overflows.");
            used += count * sizeof(std::uint64_t);
        }
        if (extra > limit - used) throw std::length_error("STL growth size accounting overflows.");
    }
    template<class T> void Grow(std::vector<T> &values, std::size_t count) {
        if (count <= values.capacity()) return;
        if (count > std::numeric_limits<std::size_t>::max() / (2 * sizeof(T)))
            throw std::length_error("STL storage size overflows.");
        const auto next = std::max(count, values.capacity() * 2);
        CheckBytes(next * sizeof(T)); // 扩容期间新旧数组同时存在。
        values.reserve(next);
        CheckBytes(0);
    }
    std::size_t HashPoint(const AlignmentPoint &point) const {
        std::size_t hash = 0;
        for (const auto value : point) hash ^= std::hash<double>{}(value) + std::size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
        return hash;
    }
    AlignmentPoint Point(std::uint64_t index) const {
        const auto offset = static_cast<std::size_t>(index) * 3;
        return {m_vertices[offset], m_vertices[offset + 1], m_vertices[offset + 2]};
    }
    std::size_t GetSlot(const AlignmentPoint &point, const std::vector<std::uint64_t> &slots) const {
        auto slot = HashPoint(point) & (slots.size() - 1);
        std::size_t probes = 0;
        while (slots[slot] != emptySlot && Point(slots[slot]) != point) {
            if (++probes % 256 == 0) CheckWork(m_work);
            slot = (slot + 1) & (slots.size() - 1);
        }
        return slot;
    }
    std::uint64_t Vertex(const AlignmentPoint &point) {
        if (!m_slots.empty()) {
            const auto slot = GetSlot(point, m_slots);
            if (m_slots[slot] != emptySlot) return m_slots[slot];
        }
        const auto count = m_vertices.size() / 3;
        if (m_slots.empty() || count >= m_slots.size() / 2) {
            if (m_slots.size() > std::numeric_limits<std::size_t>::max() / (2 * sizeof(std::uint64_t)))
                throw std::length_error("STL weld index overflows.");
            const auto next = m_slots.empty() ? std::size_t{16} : m_slots.size() * 2;
            CheckBytes(next * sizeof(std::uint64_t));
            std::vector<std::uint64_t> slots(next, emptySlot);
            for (std::size_t i = 0; i < count; ++i) {
                if (i % 256 == 0) CheckWork(m_work);
                const auto slot = GetSlot(Point(i), slots);
                slots[slot] = static_cast<std::uint64_t>(i);
            }
            m_slots.swap(slots);
        }
        Grow(m_vertices, m_vertices.size() + 3);
        m_vertices.insert(m_vertices.end(), point.begin(), point.end());
        const auto slot = GetSlot(point, m_slots);
        m_slots[slot] = static_cast<std::uint64_t>(count);
        return static_cast<std::uint64_t>(count);
    }
    const AlignmentWork &m_work;
    double m_scale;
    std::vector<double> m_vertices;
    std::vector<std::uint64_t> m_triangles, m_slots;
};
} // namespace

bool AlignmentReferenceLoader::GetRequestValid(const AlignmentReferenceRequest &r) {
    return !r.filePath.empty() && r.filePath.find('\0') == std::string::npos &&
           !r.frameId.empty() && r.frameId.size() <= 128 &&
           !r.coordinateFrame.empty() && r.coordinateFrame.size() <= 128 &&
           static_cast<unsigned>(r.fileUnit) <= 2 && static_cast<unsigned>(r.unit) <= 2 &&
           ((r.fileUnit == AlignmentUnit::ModelUnit) == (r.unit == AlignmentUnit::ModelUnit));
}

AlignmentReferenceData AlignmentReferenceLoader::Load(const AlignmentReferenceRequest &request,
                                                      const AlignmentWork &work) {
    AlignmentReferenceData result;
    try {
        Require(GetRequestValid(request), "Invalid STL reference request.");
        CheckWork(work);
        const double scale = request.fileUnit == request.unit ? 1.0
                             : request.fileUnit == AlignmentUnit::Meter ? 1000.0 : 0.001;
        MeshBuilder builder(work, scale);
        Reader reader(request.filePath, work);
        if (reader.GetBinary()) {
            std::array<unsigned char, 84> header{};
            reader.Read(header.data(), header.size());
            const auto count = UInt32(header.data() + 80);
            const auto limit = std::numeric_limits<std::size_t>::max();
            if (count > (limit - readBytes) / (3 * sizeof(std::uint64_t)))
                throw std::length_error("Reference triangle indices size accounting overflows.");
            for (std::uint32_t i = 0; i < count; ++i) {
                CheckWork(work);
                std::array<unsigned char, 50> facet{};
                reader.Read(facet.data(), facet.size());
                std::array<AlignmentPoint, 3> points{};
                for (std::size_t j = 0; j < 12; ++j) {
                    const auto bits = UInt32(facet.data() + 4 * j);
                    float value;
                    std::memcpy(&value, &bits, sizeof(value));
                    Require(std::isfinite(value), "Non-finite binary STL value.");
                    if (j >= 3) points[(j - 3) / 3][(j - 3) % 3] = value;
                }
                builder.Add(points);
            }
            Require(reader.Byte() < 0, "Unexpected binary STL tail.");
        } else {
            auto word = reader.Word();
            while (!word.empty()) {
                Require(word == "solid", "ASCII STL must start with solid."); reader.LineEnd();
                word = reader.Word();
                while (word == "facet") {
                    CheckWork(work);
                    reader.Expect("normal");
                    for (int i = 0; i < 3; ++i) (void)reader.Number();
                    reader.Expect("outer"); reader.Expect("loop");
                    std::array<AlignmentPoint, 3> points{};
                    for (auto &point : points) { reader.Expect("vertex"); for (auto &value : point) value = reader.Number(); }
                    reader.Expect("endloop"); reader.Expect("endfacet");
                    builder.Add(points); word = reader.Word();
                }
                Require(word == "endsolid", "ASCII STL is missing endsolid."); reader.LineEnd();
                word = reader.Word();
            }
        }
        result.mesh = builder.Finish(request.coordinateFrame);
        result.metadata.filePath = request.filePath;
        result.metadata.sha256 = reader.Finish();
        result.metadata.frameId = request.frameId;
        result.metadata.coordinateFrame = request.coordinateFrame;
        result.metadata.fileUnit = request.fileUnit;
        result.metadata.unit = request.unit;
        result.metadata.sourceBytes = reader.GetSize();
        result.metadata.pointCount = result.mesh->GetVertices().size() / 3;
        result.metadata.triangleCount = result.mesh->GetTriangles().size() / 3;
        CheckWork(work);
        result.status = AlignmentStatus::FullyDetermined;
    } catch (const std::length_error &error) { result.status = AlignmentStatus::ResourceExhausted; result.message = error.what(); }
      catch (const std::bad_alloc &) { result.status = AlignmentStatus::ResourceExhausted; }
      catch (const std::invalid_argument &error) { result.status = AlignmentStatus::InvalidInput; result.message = error.what(); }
      catch (const std::system_error &error) { result.status = AlignmentStatus::IoError; result.message = error.what(); }
      catch (const std::exception &error) {
        const auto status = AlignmentGeometryFit::GetWorkStatus(work);
        result.status = status == AlignmentStatus::FullyDetermined ? AlignmentStatus::InternalError : status;
        result.message = error.what();
    }
    if (result.status != AlignmentStatus::FullyDetermined) result.mesh.reset();
    return result;
}
