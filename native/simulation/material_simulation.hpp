#pragma once
#include "content/content_registry.hpp"
#include "gameplay/stone_rules.hpp"

namespace pixel {
// Pure C++ grid simulation. Only GameSession invokes step at a completed command phase.
class MaterialSimulation final {
public:
    static WorldState make_lab(std::uint64_t seed, const ContentRegistry& content, bool liquids_only=false);
    static CommandResult command(WorldState& world, const ContentRegistry& content, const WorldCommand& command);
    static void step(WorldState& world, const ContentRegistry& content);
    static CellState initial_state(const MaterialDefinition& definition, std::uint32_t random_seed=0);
private:
    static void transform(WorldState& world, CellPos position, Material material, CellState state);
};
}
