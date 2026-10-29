#pragma once
#include "content/content_registry.hpp"
#include "gameplay/stone_rules.hpp"
#include <span>
#include "simulation/rigid_simulation.hpp"

namespace pixel {
struct ElectricAddress {CellPos position;std::uint32_t body=0;bool operator==(const ElectricAddress&)const=default;};
struct ElectricRelocation {ElectricAddress from,to;};
class ElectricalSimulation final {
public:
    static CommandResult inject(WorldState& world,const ContentRegistry& content,const WorldCommand& command);
    static void transfer(WorldState&,ElectricAddress from,ElectricAddress to);
    static void transfer_batch(WorldState&,std::span<const ElectricRelocation>);
    static void reconcile(WorldState&,std::span<const RigidSimulation::FragmentLink> fragments={});
};
}
