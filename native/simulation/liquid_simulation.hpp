#pragma once
#include "content/content_registry.hpp"
#include "core/world_state.hpp"
#include "core/physical_units.hpp"
#include <span>

namespace pixel {
inline double liquid_gravity(const MaterialDefinition& d){return d.gravity_q8/256.0*physical::gravity_per_tick;}
// Finite-volume pressure projection and conservative, whole-cell transport.
// All fields are derived per step; persistent momentum belongs to WorldState.
class LiquidSimulation final {
public:
    static void step(WorldState&,const ContentRegistry&,std::span<const CellPos> active,
        std::span<const std::uint8_t> attached,std::span<std::uint8_t> updated,
        std::span<std::uint8_t> transported);
private:
    static void substep(WorldState&,std::span<const MaterialDefinition* const>,std::span<const Motion>,std::span<const CellPos>,
        std::span<const std::uint8_t>,std::span<std::uint8_t>,std::span<std::uint8_t>,double);
};
}
