#pragma once
#include "content/content_registry.hpp"
#include "core/world_state.hpp"
#include "simulation/rigid_simulation.hpp"
#include <span>

namespace pixel {
struct ExplosionResult {bool ok;std::uint64_t affected;std::string error;std::vector<RigidSimulation::FragmentLink> fragments;};
class ExplosionSimulation final {
public:
    static ExplosionResult apply(WorldState& world,const ContentRegistry& content,CellPos center,const ExplosionDefinition& blast);
    static void advance(WorldState& world,const ContentRegistry& content);
private:
    static ExplosionResult commit(WorldState&,const ContentRegistry&,CellPos,const ExplosionDefinition&,std::span<const CellPos> fuel,Material residue);
};
}
