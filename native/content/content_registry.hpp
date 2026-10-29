#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pixel {

enum class Motion { fixed, powder, liquid, gas, fire, rigid };

struct ExplosionDefinition {
    bool source_rules = false;
    int create_probability = 5; // Source Random15 % 101 < gate.
    int radius = 0; // 0 disables material detonation.
    int ray_energy = 400000;
    int max_durability = 11;
    int heat = 120;
    int impulse_q8 = 1536;
    int delay_ticks = 4;
    int trigger_energy = 20000;
    bool hole_enabled = true;
    bool destroy_liquids = false;
    std::string output = "material.smoke";
    std::string fire_material = "material.fire";
};

struct MaterialDefinition {
    std::string id;
    std::string display_name;
    std::uint32_t hardness = 1;
    std::string source_id;
    std::string cell_type = "liquid";
    bool sand = true;
    bool is_static = true;
    float density = 10;
    float solid_friction = 0.6f; // Project fallback when the source omits it.
    std::uint32_t color = 0xff888888; // AARRGGBB, display only.
    std::string texture_path;
    std::vector<std::string> tags;
    std::uint16_t lifetime = 0; // Prototype fixed ticks; 0 = indefinite.
    std::uint32_t fuel = 0;
    bool initially_burning = false;
    int ignition = 85;
    int heat = 0;
    // Local Noita-style ordinary-fluid branch; legacy authored fluids retain pressure.
    bool source_liquid = false;
    float liquid_gravity = 0.5f;
    float liquid_damping = 0.8f;
    float liquid_flow_speed = 0.9f;
    int flow_interval = 1;
    bool collectible = false;
    int cooling = 0;
    int warmth_threshold = 0;
    std::string warmth_output;
    int gravity_q8 = 256; // Liquids: relative to shared world gravity; other CA types retain their tick acceleration.
    int damping_milli = 900;
    int lateral_speed_q8 = 256;
    int flow_search = 64; // Local probe budget; open longer routes use the finite-row cache.
    int terminal_speed_q8 = 2048;
    int powder_liquid_speed_q8 = 102; // Project tuning: downward liquid entry, 102/256 cells per tick.
    int gas_upward_speed_q8 = 256;
    int gas_horizontal_speed_q8 = 256;
    int gas_downward_speed_q8 = 2048; // Limit for downward impulses, not spontaneous sinking.
    bool requires_oxygen = false;
    std::string flame_output = "material.fire";
    std::string smoke_output = "material.smoke";
    std::string burn_output = "material.smoke";
    int burn_heat = 100;
    // Imported materials use the recovered branches. The existing explicit
    // fuel countdown remains supported for authored prototype definitions.
    bool source_burning = false;
    bool burnable = false;
    int fire_hp = 100; // -1 disables the recovered burnout check.
    int generates_flames = 100; // Project fallback: source default is unknown.
    int generates_smoke = 0;
    std::string ignition_output; // Empty = no conversion at ignition entry.
    bool source_phase = false;
    bool source_gas = false;
    bool source_electricity = false;
    int cold_chance_rev=100, warmth_chance_rev=100; // Recovered signed-int16 defaults.
    int cold_threshold = -100;
    std::string cold_output;
    bool can_loosen = false;
    int settle_ticks = 30;
    bool slime = false;
    bool source_adhesion = false;
    int liquid_sticks_to_ceiling = 0; // Raw source threshold, not force or fixed duration.
    int adhesion_strength_q8 = 0;
    int adhesion_span = 1;
    int adhesion_hold_ticks = 0; // 0 = hold until support loss/impulse; otherwise timed dripping.
    int durability = 0;
    int hit_points = 100;
    ExplosionDefinition explosion;
    bool electrical_conductivity = false;
    int electric_cost = 4; // Prototype pulse energy spent on entering one conducting cell.
    [[nodiscard]] Motion motion(bool mobilized = false) const noexcept;
    [[nodiscard]] bool has_tag(std::string_view tag) const;
};

struct ReactionDefinition {
    std::string input_a, input_b; // material ID or [tag].
    std::string output_a, output_b; // material ID, air or keep.
    std::uint32_t interval = 1; // Deterministic sampling denominator, project tuning.
    std::uint32_t minimum_age = 0;
    int minimum_temperature = -1000;
    int maximum_temperature = 10000;
    std::string input_c; // Optional third distinct contact cell; empty = two-input reaction.
    std::string output_c = "keep";
    // Legacy authored blobs use connected disks. Source-gated records use
    // restricted rays or unrestricted strict disks; commit ordering is project-owned.
    int blob_radius_a = 0;
    int blob_radius_b = 0;
    // -1 keeps authored project sampling. Nonnegative values use the recovered
    // 0..10000 gate and source dispatch/label/region branches.
    int probability_gate = -1;
    bool fast = false;
    bool required_neighbor = false;
    int direction = -1; // any=-1, top=0, bottom=1, left=2, right=3.
    bool destroy_lonely = false;
    bool restrict_a = true;
    bool restrict_b = true;
};

class ContentRegistry;

struct RegistryResult {
    std::shared_ptr<const ContentRegistry> registry;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return registry != nullptr; }
};

class ContentRegistry final {
public:
    [[nodiscard]] static RegistryResult build(std::vector<MaterialDefinition> definitions, std::vector<ReactionDefinition> reactions = {});
    [[nodiscard]] const MaterialDefinition* find_material(std::string_view id) const;
    [[nodiscard]] std::size_t material_count() const noexcept { return materials_.size(); }
    [[nodiscard]] const std::vector<MaterialDefinition>& materials() const noexcept { return materials_; }
    [[nodiscard]] const std::vector<ReactionDefinition>& reactions() const noexcept { return reactions_; }
    [[nodiscard]] std::span<const std::size_t> reaction_candidates(std::string_view material_id) const;

private:
    explicit ContentRegistry(std::vector<MaterialDefinition> definitions);
    std::vector<MaterialDefinition> materials_;
    std::unordered_map<std::string, std::size_t> material_indices_;
    std::vector<ReactionDefinition> reactions_;
    std::vector<std::vector<std::size_t>> reaction_candidates_;
};

} // namespace pixel
