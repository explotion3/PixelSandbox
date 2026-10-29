#pragma once
#include "gameplay/stone_rules.hpp"
#include "content/content_registry.hpp"

namespace pixel {
struct BodyMotion {
    std::uint32_t id;
    double x,y,angle,vx,vy,angular_velocity;
    bool sleeping;
};
// An implemented solver is injected by the native physics library. No engine handles cross this boundary.
class RigidSolver {
public:
    virtual ~RigidSolver()=default;
    virtual void reset() noexcept=0;
    [[nodiscard]] virtual std::vector<BodyMotion> step(const WorldState&,const ContentRegistry&,double seconds)=0;
};
class RigidSimulation final {
    static bool displace_liquids(WorldState&,const ContentRegistry&);
public:
    struct ShapeBefore {double x,y;std::uint64_t revision;};
    struct FragmentLink {std::uint32_t child,parent;};
    [[nodiscard]] static std::vector<std::vector<RigidPixel>> connected_parts(const std::vector<RigidPixel>&);
    [[nodiscard]] static std::vector<ShapeBefore> shape_centers(const WorldState&,const ContentRegistry&);
    static std::vector<FragmentLink> finish_material_changes(WorldState&,const ContentRegistry&,const std::vector<ShapeBefore>&);
    [[nodiscard]] static CommandResult create(WorldState&,const ContentRegistry&,const WorldCommand&,bool detach);
    [[nodiscard]] static CommandResult edit(WorldState&,const ContentRegistry&,const WorldCommand&);
    static void advance(WorldState&,const ContentRegistry&,RigidSolver&,double seconds);
};
}
