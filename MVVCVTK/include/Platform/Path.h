#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace PlatformPath {

// 对外 std::string 路径统一为 UTF-8；filesystem 内部使用平台 native 编码。
inline std::filesystem::path GetNativePath(std::string_view utf8Path)
{
    return std::filesystem::u8path(utf8Path.begin(), utf8Path.end());
}

// VTK 9.4 文件接口接收 UTF-8 窄字符串，禁止在 Windows 上使用 path::string()。
inline std::string GetUtf8Path(const std::filesystem::path& nativePath)
{
    return nativePath.u8string();
}

}
