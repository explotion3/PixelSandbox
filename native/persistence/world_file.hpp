#pragma once
#include "core/world_snapshot.hpp"
#include <filesystem>
#include <optional>

namespace pixel::persistence {
struct FileResult {
    bool ok = false;
    std::string error;
    double elapsed_ms = 0;
};
struct LoadResult {
    std::optional<WorldSnapshot> snapshot;
    std::string error;
    double elapsed_ms = 0;
};
[[nodiscard]] FileResult save_world_file(const std::filesystem::path& path, const WorldSnapshot& snapshot);
[[nodiscard]] LoadResult load_world_file(const std::filesystem::path& path);
} // namespace pixel::persistence
