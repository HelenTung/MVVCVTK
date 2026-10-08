#include "Host/MetrologyReport.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <bcrypt.h>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <ostream>
#include <streambuf>
#include <string_view>

namespace {
using Status = MetrologyReportWriteStatus;
struct Failure final {
  Status status;
  const char *message;
  DWORD error = 0;
};
void Require(bool value, const char *message) {
  if (!value)
    throw Failure{Status::InvalidInput, message};
}
void Cancel(const MetrologyReportWriteOptions &options) {
  if (options.isCancelled && options.isCancelled())
    throw Failure{Status::Cancelled, "Report file write cancelled."};
}
std::wstring Native(const std::filesystem::path &path) {
  const auto value = path.native();
  return value.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + value.substr(2)
                                      : L"\\\\?\\" + value;
}
std::filesystem::path Destination(const std::string &utf8) {
  Require(!utf8.empty() && utf8.find('\0') == std::string::npos &&
              utf8.size() <= 131068,
          "Invalid report file path.");
  const auto length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                          static_cast<int>(utf8.size()), nullptr, 0);
  Require(length > 0 && length < 32700,
          "Report file path is not valid bounded UTF-8.");
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                              static_cast<int>(utf8.size()), wide.data(),
                              length) == length,
          "Report path conversion failed.");
  std::replace(wide.begin(), wide.end(), L'/', L'\\');
  Require(wide.rfind(L"\\\\?\\", 0) != 0 && wide.rfind(L"\\\\.\\", 0) != 0,
          "Device namespace paths are not report files.");
  const std::filesystem::path supplied(wide);
  Require(!supplied.filename().empty() && supplied.filename() != L"." &&
              supplied.filename() != L".." &&
              (!supplied.has_root_name() || supplied.has_root_directory()),
          "Report destination needs an ordinary filename.");
  // 必须在 Win32 绝对路径归一化前拒绝末尾点/空格，避免被静默映射到另一文件名。
  for (const auto &part : supplied.relative_path()) {
    const auto component = part.native();
    if (component == L"." || component == L"..")
      continue;
    Require(!component.empty() && component.back() != L'.' &&
                component.back() != L' ',
            "Ambiguous trailing dot or space in report path.");
    Require(std::none_of(component.begin(), component.end(),
                         [](wchar_t c) {
                           return c < 32 ||
                                  std::wstring_view(L"<>:\"|?*").find(c) !=
                                      std::wstring_view::npos;
                         }),
            "Unsupported report path character or alternate stream.");
    auto stem = component.substr(0, component.find(L'.'));
    for (auto &c : stem)
      if (c >= L'a' && c <= L'z')
        c -= L'a' - L'A';
    const bool port =
        stem.size() == 4 &&
        (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
        ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' ||
         stem[3] == L'\u00b2' || stem[3] == L'\u00b3');
    Require(stem != L"CON" && stem != L"PRN" && stem != L"AUX" &&
                stem != L"NUL" && stem != L"CONIN$" && stem != L"CONOUT$" &&
                !port,
            "Reserved device name is not a report file.");
  }
  const auto path =
      std::filesystem::absolute(supplied).lexically_normal().make_preferred();
  Require(path.native().size() < 32640,
          "Report path leaves no room for a temporary name.");
  return path;
}
class Temporary final {
public:
  Temporary() = default;
  Temporary(const Temporary &) = delete;
  Temporary &operator=(const Temporary &) = delete;
  ~Temporary() {
    if (handle != INVALID_HANDLE_VALUE) {
      if (!committed && !hasDiscardAttempted)
        Discard();
      CloseHandle(handle);
    }
  }
  void Open(const std::filesystem::path &parent) {
    for (int attempt = 0; attempt < 16; ++attempt) {
      std::array<unsigned char, 16> random{};
      if (BCryptGenRandom(nullptr, random.data(),
                          static_cast<ULONG>(random.size()),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw Failure{Status::IoError,
                      "Report temporary name generation failed."};
      static constexpr wchar_t hex[] = L"0123456789abcdef";
      std::wstring name = L".mvvcvtk-report-";
      for (auto byte : random) {
        name += hex[byte >> 4];
        name += hex[byte & 15];
      }
      name += L".tmp";
      path = parent / name;
      utf8Path = path.u8string();
      const auto native = Native(path);
      handle = CreateFileW(native.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle != INVALID_HANDLE_VALUE) {
        if (GetFileType(handle) != FILE_TYPE_DISK)
          throw Failure{Status::InvalidInput,
                        "Report destination is not disk storage."};
        return;
      }
      const auto error = GetLastError();
      if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
        throw Failure{Status::IoError, "Report temporary file creation failed.",
                      error};
    }
    throw Failure{Status::IoError,
                  "Report temporary name collision limit reached."};
  }
  bool Discard() noexcept {
    if (handle == INVALID_HANDLE_VALUE || committed || marked)
      return true;
    if (hasDiscardAttempted)
      return false;
    hasDiscardAttempted = true;
    FILE_DISPOSITION_INFO info{};
    info.DeleteFile = TRUE;
    marked = SetFileInformationByHandle(handle, FileDispositionInfo, &info,
                                        sizeof(info)) != FALSE;
    return marked;
  }
  HANDLE handle = INVALID_HANDLE_VALUE;
  std::filesystem::path path;
  std::string utf8Path;
  bool committed = false;

private:
  bool marked = false;
  bool hasDiscardAttempted = false;
};
class FileBuffer final : public std::streambuf {
public:
  explicit FileBuffer(HANDLE handle) : handle(handle) {}

protected:
  std::streamsize xsputn(const char *data, std::streamsize count) override {
    std::streamsize done = 0;
    while (done < count) {
      const auto next =
          static_cast<DWORD>(std::min<std::streamsize>(count - done, 65536));
      DWORD written = 0;
      if (!WriteFile(handle, data + done, next, &written, nullptr) ||
          written == 0)
        break;
      done += written;
    }
    return done;
  }

private:
  HANDLE handle;
};
void Commit(Temporary &file, const std::filesystem::path &destination) {
  const auto target = Native(destination);
  const auto nameBytes = target.size() * sizeof(wchar_t);
  const auto size =
      std::max(sizeof(FILE_RENAME_INFO), offsetof(FILE_RENAME_INFO, FileName) +
                                             nameBytes + sizeof(wchar_t));
  auto bytes = std::make_unique<unsigned char[]>(size);
  auto *info = ::new (bytes.get()) FILE_RENAME_INFO{};
  info->ReplaceIfExists = FALSE;
  info->RootDirectory = nullptr;
  info->FileNameLength = static_cast<DWORD>(nameBytes);
  std::memcpy(info->FileName, target.data(), nameBytes);
  if (!SetFileInformationByHandle(file.handle, FileRenameInfo, info,
                                  static_cast<DWORD>(size))) {
    const auto error = GetLastError();
    const bool exists =
        GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
    throw Failure{exists || error == ERROR_ALREADY_EXISTS ||
                          error == ERROR_FILE_EXISTS
                      ? Status::AlreadyExists
                      : Status::IoError,
                  "Report destination commit failed; existing targets are "
                  "never replaced.",
                  error};
  }
  file.committed = true;
}
} // namespace

MetrologyReportFileResult
WriteMetrologyReportFile(const MetrologyReport &report,
                         const std::string &utf8Path,
                         const MetrologyReportWriteOptions &options) {
  MetrologyReportFileResult result;
  Temporary file;
  try {
    const auto destination = Destination(utf8Path);
    Cancel(options);
    if (GetFileAttributesW(Native(destination).c_str()) !=
        INVALID_FILE_ATTRIBUTES)
      throw Failure{Status::AlreadyExists,
                    "Report destination already exists."};
    file.Open(destination.parent_path());
    FileBuffer buffer(file.handle);
    std::ostream stream(&buffer);
    result.write = WriteMetrologyReport(report, stream, options);
    if (result.write.status == Status::Succeeded) {
      Cancel(options);
      if (!FlushFileBuffers(file.handle))
        throw Failure{Status::IoError, "Report file flush failed.",
                      GetLastError()};
      Cancel(options);
      Commit(file, destination);
      result.isCommitted = true;
    }
  } catch (const Failure &e) {
    result.write.status = e.status;
    result.write.message = e.message;
    if (e.error)
      result.write.message += " Win32=" + std::to_string(e.error);
  } catch (const std::filesystem::filesystem_error &) {
    result.write.status = Status::IoError;
    result.write.message = "Report path resolution failed.";
  } catch (const std::bad_alloc &) {
    result.write.status = Status::BudgetExceeded;
  } catch (const std::exception &e) {
    result.write.status = Status::InternalError;
    result.write.message = e.what();
  } catch (...) {
    result.write.status = Status::InternalError;
    result.write.message = "Report file writer failed.";
  }
  if (!file.committed && !file.Discard()) {
    result.remainingTemporaryPath = std::move(file.utf8Path);
    result.write.message += " Temporary file cleanup failed.";
  }
  return result;
}
