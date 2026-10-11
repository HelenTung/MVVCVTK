// 测试用途：验证导出取消、提交失败和同名并发文件的完整性。
#include "Data/Internal/AtomicFileOutput.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <fstream>
#include <array>
#include <iostream>
#include <thread>
#include <string>
#include <iterator>
#include <vector>

int main()
{
    const auto directory = std::filesystem::current_path() / ("atomic-export-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directory(directory);
    const auto target = directory / "output.raw";
    const std::string old = "previous-complete-result";
    const auto reset = [&] { std::ofstream file(target,std::ios::binary); file << old; };
    const auto read = [&] { std::ifstream file(target,std::ios::binary); return std::string(std::istreambuf_iterator<char>(file),{}); };
    int failures = 0;
    const auto check = [&](bool ok, const char* message) { std::cout << (ok?"PASS ":"FAIL ") << message << '\n'; failures += !ok; };
    reset();
    {
        AtomicFileOutput file(target);
        check(file.GetIsOpen() && file.Write("partial",7),"temporary write succeeds");
        check(read()==old,"writing leaves previous final untouched");
        TaskStopSource stop; stop.Stop();
        check(!file.Commit(stop.GetToken()),"cancelled task cannot commit");
    }
    check(read()==old,"cancel preserves previous bytes");
    {
        const auto locked = CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        AtomicFileOutput file(target); file.Write("replacement",11);
        check(locked!=INVALID_HANDLE_VALUE && !file.Commit({}),"locked target rejects commit");
        if (locked!=INVALID_HANDLE_VALUE) CloseHandle(locked);
    }
    check(read()==old,"failed replacement preserves previous bytes");
    {
        AtomicFileOutput file(directory/"missing"/"output.raw");
        check(!file.GetIsOpen() && !file.Write("x",1) && !file.Commit({}),"open failure cannot publish");
    }
    std::array<bool,2> completed{};
    const auto write = [&](unsigned i) {
        AtomicFileOutput file(target); const std::vector<char> bytes(1024*1024, i?'B':'A');
        completed[i] = file.Write(bytes.data(),bytes.size()) && file.Commit({});
    };
    std::thread a(write,0), b(write,1); a.join(); b.join();
    const auto bytes=read();
    check(completed[0] && completed[1] && bytes.size()==1024*1024
        && (bytes==std::string(bytes.size(),'A') || bytes==std::string(bytes.size(),'B')),
        "same-path concurrent commits retain one complete result");
    check(std::distance(std::filesystem::directory_iterator(directory),std::filesystem::directory_iterator{})==1,"task temporary files are cleaned");
    std::filesystem::remove(target); std::filesystem::remove(directory);
    return failures;
}
