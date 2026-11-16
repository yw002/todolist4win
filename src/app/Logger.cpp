#include "app/Logger.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

namespace todo {

Logger::Logger() : Logger(DefaultPath()) {}
Logger::Logger(std::filesystem::path path) : path_(std::move(path)) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    stream_.open(path_, std::ios::app);
}
std::filesystem::path Logger::DefaultPath() {
    std::wstring base(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), static_cast<DWORD>(base.size()));
    if (length == 0 || length >= base.size()) return std::filesystem::current_path() / L"TodoListWin" / L"Logs" / L"todolist.log";
    base.resize(length);
    return std::filesystem::path(base) / L"TodoListWin" / L"Logs" / L"todolist.log";
}
void Logger::Info(const std::string& operation, std::uint64_t requestId) { Write("INFO", operation, requestId, nullptr); }
void Logger::Error(const std::string& operation, const AppError& error, std::uint64_t requestId) { Write("ERROR", operation, requestId, &error); }
void Logger::Write(const char* level, const std::string& operation, std::uint64_t requestId, const AppError* error) {
    std::lock_guard lock(mutex_);
    if (!stream_) return;
    RollIfNeeded();
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char timestamp[32]{};
    std::snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    stream_ << timestamp << ' ' << level << " operation=" << operation << " request=" << requestId;
    if (error) stream_ << " code=" << static_cast<int>(error->code) << " native=" << error->nativeCode;
    stream_ << '\n';
    stream_.flush();
}
void Logger::RollIfNeeded() {
    std::error_code ec;
    if (std::filesystem::file_size(path_, ec) < 1024 * 1024 || ec) return;
    stream_.close();
    std::filesystem::remove(path_.string() + ".3", ec);
    for (int i = 2; i >= 1; --i) std::filesystem::rename(path_.string() + "." + std::to_string(i), path_.string() + "." + std::to_string(i + 1), ec);
    std::filesystem::rename(path_, path_.string() + ".1", ec);
    stream_.open(path_, std::ios::trunc);
}

}  // namespace todo
