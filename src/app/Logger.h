#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "common/Error.h"

namespace todo {

class Logger {
public:
    Logger();
    explicit Logger(std::filesystem::path path);
    void Info(const std::string& operation, std::uint64_t requestId = 0);
    void Error(const std::string& operation, const AppError& error, std::uint64_t requestId = 0);
    [[nodiscard]] static std::filesystem::path DefaultPath();
private:
    void Write(const char* level, const std::string& operation, std::uint64_t requestId, const AppError* error);
    void RollIfNeeded();
    std::filesystem::path path_;
    std::ofstream stream_;
    std::mutex mutex_;
};

}  // namespace todo
