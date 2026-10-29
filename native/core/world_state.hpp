#pragma once

#include "core/world_snapshot.hpp"
#include <array>
#include <optional>

namespace pixel {

// Save-local runtime indices. Only air/stone reserve slots; all other values
// come from material_ids(), so adding definitions never needs new enum members.
enum class Material : std::uint16_t { air = 0, stone = 1 };
struct MaterialChunk {
    ChunkPos position;
    std::array<Material, chunk_size * chunk_size> cells{};
    std::array<CellState, chunk_size * chunk_size> states{};
    bool dirty = false;
    bool awake = true;
    std::uint64_t collision_revision = 1;
};
struct WorldView {
    std::uint64_t seed = 0;
    std::uint64_t tick = 0;
    WorldBounds bounds;
    std::uint64_t world_stone = 0;
    std::uint64_t dropped_stone = 0;
    std::uint64_t held_stone = 0;
    std::uint64_t initial_stone = 0;
    bool material_lab = false;
    std::uint32_t active_chunks = 0;
    std::uint32_t visited_cells = 0;
    std::uint32_t moved_cells = 0;
    std::uint32_t reactions = 0;
    std::uint32_t expired = 0;
    std::uint32_t deferred_reactions = 0;
    std::uint32_t explosions = 0;
    std::uint32_t blast_destroyed = 0;
    std::uint32_t blast_mobilized = 0;
    std::uint32_t electric_pulses = 0;
    std::uint32_t electrified_cells = 0;
    std::uint64_t electric_energy = 0;
    std::uint64_t electric_spent = 0;
    std::uint8_t freeze_chance=0,unfreeze_chance=0;
    double simulation_ms = 0;
    std::uint32_t rigid_bodies=0,rigid_pixels=0;
    std::uint32_t fluid_displaced=0,fluid_blocked=0;
    std::uint32_t liquid_pressure_iterations=0,liquid_transfers=0;
    std::uint32_t liquid_body_impulses=0;
    double liquid_divergence_before=0,liquid_divergence_after=0;
    std::uint32_t liquid_substeps=0;
    std::uint64_t liquid_route_visits=0;
    double liquid_setup_ms=0,liquid_projection_ms=0,liquid_transport_ms=0;
};

class GameSession;
class StoneRules;
class WorldState;
class MaterialSimulation;
class ExplosionSimulation;
class ElectricalSimulation;
class RigidSimulation;
struct BodyPixelAddress {std::size_t body,pixel;};
[[nodiscard]] WorldState make_stone_wall_scenario(std::uint64_t seed);

class WorldState final {
public:
    explicit WorldState(std::uint64_t seed, WorldBounds bounds = {});
    [[nodiscard]] WorldView view() const noexcept;
    [[nodiscard]] std::optional<Material> material_at(CellPos p) const noexcept;
    [[nodiscard]] const std::vector<MaterialChunk>& chunks() const noexcept { return chunks_; }
    [[nodiscard]] const MaterialChunk* chunk_at(ChunkPos p) const noexcept;
    [[nodiscard]] const std::vector<StoneDrop>& drops() const noexcept { return drops_; }
    [[nodiscard]] WorldSnapshot snapshot() const;
    [[nodiscard]] const CellState* cell_state(CellPos p) const noexcept;
    [[nodiscard]] std::optional<Material> surface_material_at(CellPos p) const noexcept;
    [[nodiscard]] const CellState* surface_cell_state(CellPos p) const noexcept;
    [[nodiscard]] std::optional<BodyPixelAddress> body_pixel_at(CellPos p) const noexcept;
    [[nodiscard]] const std::vector<RigidBodyState>& bodies() const noexcept {return bodies_;}
    [[nodiscard]] Material material_index(std::string_view id) const noexcept;
    [[nodiscard]] const std::vector<std::string>& material_ids() const noexcept { return material_ids_; }
    [[nodiscard]] std::uint64_t material_count(Material m) const noexcept {
        const auto i=static_cast<std::uint16_t>(m);return i<counts_.size()?counts_[i]:0;
    }

private:
    friend class GameSession;
    friend class StoneRules;
    friend class MaterialSimulation;
    friend class LiquidSimulation;
    friend class SourceLiquidSimulation;
    friend class ExplosionSimulation;
    friend class ElectricalSimulation;
    friend class RigidSimulation;
    friend class MaterialInteractionStage;
    friend WorldState make_stone_wall_scenario(std::uint64_t seed);
    std::uint64_t seed_;
    std::uint64_t tick_ = 0;
    WorldBounds bounds_;
    ChunkPos first_chunk_;
    int chunk_columns_ = 0;
    int chunk_rows_ = 0;
    std::vector<MaterialChunk> chunks_;
    std::vector<ChunkPos> dirty_chunks_;
    std::vector<StoneDrop> drops_;
    std::uint64_t world_stone_ = 0;
    std::uint64_t dropped_stone_ = 0;
    std::uint64_t held_stone_ = 0;
    std::uint64_t initial_stone_ = 0;
    bool material_lab_ = false;
    std::uint8_t freeze_chance_=0,unfreeze_chance_=0;
    std::vector<std::string> material_ids_{"material.stone"};
    std::vector<std::uint64_t> counts_{0,0};
    WorldView simulation_stats_;
    std::vector<ElectricPulse> electric_pulses_;
    std::uint64_t electric_energy_ = 0;
    std::uint32_t electrified_cells_ = 0;
    std::vector<RigidBodyState> bodies_;
    std::uint32_t next_body_id_=1;
    std::uint64_t body_revision_=0;
    std::vector<std::array<double,4>> body_bounds_;
    void refresh_body_bounds();
    [[nodiscard]] std::size_t chunk_index(ChunkPos p) const noexcept;
    void set_material(CellPos p, Material material);
    void set_cell(CellPos p, Material material, CellState state);
    void wake(CellPos p);
    static WorldState from_valid_snapshot(const WorldSnapshot& snapshot);
};

} // namespace pixel
