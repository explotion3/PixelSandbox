#pragma once
#include <cstdint>

namespace pixel {
inline constexpr int chunk_size = 64;
inline constexpr int building_grid_size = 8;
struct CellPos { int x = 0; int y = 0; bool operator==(const CellPos&) const = default; };
struct ChunkPos { int x = 0; int y = 0; bool operator==(const ChunkPos&) const = default; };
constexpr int floor_div(int value, int divisor) noexcept {
    return value / divisor - (value % divisor < 0 ? 1 : 0);
}
constexpr ChunkPos cell_to_chunk(CellPos p) noexcept { return {floor_div(p.x, chunk_size), floor_div(p.y, chunk_size)}; }
constexpr CellPos cell_in_chunk(CellPos p) noexcept {
    const auto c = cell_to_chunk(p);
    return {p.x - c.x * chunk_size, p.y - c.y * chunk_size};
}
constexpr CellPos building_cell_origin(CellPos p) noexcept {
    return {floor_div(p.x, building_grid_size) * building_grid_size, floor_div(p.y, building_grid_size) * building_grid_size};
}
struct WorldBounds {
    CellPos origin{-64, 0};
    int width = 192;
    int height = 96;
    bool operator==(const WorldBounds&) const = default;
    [[nodiscard]] bool contains(CellPos p) const noexcept {
        return p.x >= origin.x && p.y >= origin.y &&
            static_cast<std::int64_t>(p.x) < static_cast<std::int64_t>(origin.x) + width &&
            static_cast<std::int64_t>(p.y) < static_cast<std::int64_t>(origin.y) + height;
    }
};
[[nodiscard]] bool valid_bounds(const WorldBounds& bounds) noexcept;
} // namespace pixel
