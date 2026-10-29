#pragma once
#include <cstdint>

namespace pixel::physical {
inline constexpr std::uint32_t ticks_per_second=60;
inline constexpr double cells_per_meter=16;
inline constexpr double gravity=10; // meters / second squared, shared with Box2D.
inline constexpr double gravity_per_tick=cells_per_meter*gravity/(ticks_per_second*ticks_per_second);
}
