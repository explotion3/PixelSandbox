#pragma once
#include "content/content_registry.hpp"
#include "core/world_state.hpp"
#include "gameplay/stone_rules.hpp"
#include "simulation/rigid_simulation.hpp"
#include <span>

namespace pixel {
struct BodyConversionResult {bool ok;std::string error;std::vector<RigidSimulation::FragmentLink> fragments;};
// One contact/thermal implementation for authoritative grid and body-local pixels.
class MaterialInteractions final {
public:
    static CommandResult temperature(WorldState&,const ContentRegistry&,CellPos,int radius,int signed_strength);
    static void step(WorldState&,const ContentRegistry&,std::span<const CellPos> active_grid);
    static BodyConversionResult consume_body_fuel(WorldState&,const ContentRegistry&,std::uint32_t body,std::span<const CellPos> local_pixels,const std::string& output);
};
}
