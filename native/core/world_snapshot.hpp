#pragma once
#include "core/coordinates.hpp"
#include <string>
#include <vector>

namespace pixel {
inline constexpr std::uint32_t save_format_version = 15;
struct CellState {
    std::int32_t temperature = 0; // Project heat units, not Celsius.
    std::uint16_t life = 0;
    std::uint32_t fuel = 0;
    bool burning = false;
    std::uint64_t created_tick = 0;
    std::int32_t velocity_x = 0; // Signed Q8 cells/tick, transported with the material.
    std::int32_t velocity_y = 0;
    std::uint16_t vertical_fraction = 0;
    std::uint16_t horizontal_fraction = 0;
    bool mobilized = false;
    std::uint16_t quiet_ticks = 0;
    std::uint16_t adhesion_ticks = 0;
    std::uint16_t adhesion_cooldown = 0;
    std::uint8_t source_attachment = 0; // 0=free, 1=released awaiting transport, 5/6/7=right/above/left support.
    std::uint16_t detonation_ticks = 0;
    std::uint16_t electrified_ticks = 0;
    std::uint32_t liquid_fraction_x = 0; // Q24 face-volume credit, below one whole cell.
    std::uint32_t liquid_fraction_y = 0;
    std::int16_t momentum_remainder_x = 0; // Signed Q16 refinement of the Q8 velocity.
    std::int16_t momentum_remainder_y = 0;
    std::uint8_t fire_intensity = 0; // Raw byte; recovered branches read it signed.
    std::uint16_t burning_ticks = 0; // Body burn counter, wraps as uint16.
    std::uint32_t fire_color = 0; // 0 = original material color, otherwise AARRGGBB.
    std::uint32_t damage_taken = 0; // Persistent HP loss; 0 means pristine.
    std::int32_t gas_counter = 0; // Recovered signed gas counter, not fixed-step age.
    std::uint64_t electric_stamp = UINT64_MAX; // No source electrical visit yet.
    bool ignition_pending = false; // Ignition entry conversion, consumed by thermal stage.
    float liquid_fall = 0; // Source q_y, not physical velocity.
    float liquid_lateral = 0; // Source q_x, damped after successful lateral swaps.
    std::uint8_t liquid_direction = 0; // 0=initial, 1=falling, 2=lateral, 3=blocked.
    bool operator==(const CellState&) const = default;
};
struct StoneDrop {
    CellPos position;
    std::uint64_t amount = 0;
    bool operator==(const StoneDrop&) const = default;
};
struct ElectricPulse {
    CellPos position;
    CellPos previous;
    std::uint32_t energy = 1000;
    std::uint16_t speed = 32;
    std::uint16_t heat_interval = 10;
    std::uint16_t heat = 120;
    std::uint16_t glow_ticks = 12;
    std::uint32_t body = 0; // 0: grid coordinates; otherwise position is body-local.
    std::uint32_t previous_body = 0;
    bool source_rules = false;
    float avg_x = 1, avg_y = 0;
    std::uint16_t splittings_left = 0;
    std::uint32_t splitting_energy = 0;
    float probability_to_heat = 0.1f;
    bool set_fire = false;
    bool operator==(const ElectricPulse&) const = default;
};
struct RigidPixel {
    CellPos local;
    std::uint16_t material = 0;
    CellState state;
    bool operator==(const RigidPixel&) const = default;
};
struct RigidBodyState {
    std::uint32_t id = 0;
    double x=0,y=0,angle=0,vx=0,vy=0,angular_velocity=0; // cells, seconds, radians.
    bool sleeping=false;
    std::uint64_t shape_revision=1; // Published shape cache key, preserved by explicit saves.
    std::vector<RigidPixel> pixels;
    bool operator==(const RigidBodyState&) const = default;
};
struct WorldSnapshot {
    std::uint32_t version = save_format_version;
    std::uint64_t seed = 0;
    std::uint64_t tick = 0;
    WorldBounds bounds;
    std::uint32_t chunk_edge = chunk_size;
    std::uint32_t building_edge = building_grid_size;
    std::vector<std::string> material_ids{"material.stone"}; // 0 = air, 1 = first ID.
    std::vector<std::uint16_t> cells; // Actual bounds only, row-major.
    std::vector<StoneDrop> drops;
    std::uint64_t held_stone = 0;
    std::uint64_t initial_stone = 0;
    bool material_lab = false;
    std::vector<CellState> states;
    std::vector<ElectricPulse> electric_pulses;
    std::uint32_t next_body_id=1;
    std::vector<RigidBodyState> bodies;
    std::uint8_t freeze_chance=0, unfreeze_chance=0;
    bool operator==(const WorldSnapshot&) const = default;
};
[[nodiscard]] std::string validate_snapshot(const WorldSnapshot& snapshot);
} // namespace pixel
