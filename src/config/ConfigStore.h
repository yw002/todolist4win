#pragma once

#include <filesystem>

#include "common/Result.h"
#include "config/AppConfig.h"

namespace todo {

// Reads and writes config.ini. The file holds window geometry and appearance only; task data is
// owned by the local SQLite file and is never described here.
class ConfigStore {
public:
    ConfigStore();
    explicit ConfigStore(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;
    [[nodiscard]] Result<AppConfig> Load() const;
    // Whole-file rewrite through a sibling .tmp + MoveFileExW(MOVEFILE_REPLACE_EXISTING).
    [[nodiscard]] Result<void> Save(const AppConfig& config) const;
    // Kept as the UI flush entry point (window position, topmost, theme, completion display). Now
    // that the file no longer contains any other section, this is the same operation as Save();
    // the distinct name documents intent at the call sites.
    [[nodiscard]] Result<void> SaveUiState(const AppConfig& config) const { return Save(config); }
    [[nodiscard]] static std::filesystem::path DefaultPath();

private:
    std::filesystem::path path_;
};

}  // namespace todo
