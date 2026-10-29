#include "core/world_state.hpp"

namespace pixel {
WorldState make_stone_wall_scenario(std::uint64_t seed) {
    WorldState world(seed);
    // Authored fixture, not terrain generation. Crosses x=0, x=64 and y=64.
    for (int y = 16; y < 80; ++y)
        for (int x = -24; x < 88; ++x) world.set_material({x, y}, Material::stone);
    world.initial_stone_ = world.world_stone_;
    return world;
}
} // namespace pixel
