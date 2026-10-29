#include "gameplay/stone_rules.hpp"
#include <algorithm>

namespace pixel {
CommandResult StoneRules::apply(WorldState& w, const WorldCommand& c) {
    if (c.kind == CommandKind::pick_up) {
        const auto amount = w.dropped_stone_;
        w.held_stone_ += amount; w.dropped_stone_ = 0; w.drops_.clear();
        return {c.kind, true, amount, "Picked up " + std::to_string(amount) + " stone units."};
    }
    if (!w.bounds_.contains(c.target)) return {c.kind, false, 0, "Target is outside the finite world."};
    if (c.kind == CommandKind::dig) {
        if (c.radius < 1 || c.radius > 8) return {c.kind, false, 0, "Dig radius must be between 1 and 8."};
        std::vector<CellPos> removed;
        removed.reserve(289);
        for (int y = -c.radius; y <= c.radius; ++y)
            for (int x = -c.radius; x <= c.radius; ++x) {
                const CellPos p{c.target.x + x, c.target.y + y};
                if (x * x + y * y <= c.radius * c.radius && w.material_at(p) == Material::stone) removed.push_back(p);
            }
        for (auto p : removed) {
            const auto chunk = cell_to_chunk(p);
            auto drop = std::find_if(w.drops_.begin(), w.drops_.end(), [chunk](const auto& d) { return cell_to_chunk(d.position) == chunk; });
            if (drop == w.drops_.end()) w.drops_.push_back({p, 1});
            else ++drop->amount;
            w.set_material(p, Material::air);
        }
        const auto amount = static_cast<std::uint64_t>(removed.size());
        w.dropped_stone_ += amount;
        return {c.kind, true, amount, "Dug " + std::to_string(amount) + " stone units."};
    }
    if (c.kind == CommandKind::repair) {
        const auto origin = building_cell_origin(c.target);
        std::array<CellPos, building_grid_size * building_grid_size> fill{};
        std::size_t count = 0;
        for (int y = 0; y < building_grid_size; ++y)
            for (int x = 0; x < building_grid_size; ++x) {
                const CellPos p{origin.x + x, origin.y + y};
                auto material = w.material_at(p);
                if (!material) return {c.kind, false, 0, "Repair grid crosses the finite world boundary."};
                if(w.body_pixel_at(p))return {c.kind,false,0,"Repair space is occupied by a rigid body. Nothing changed."};
                if (*material == Material::air) fill[count++] = p;
                else if (*material != Material::stone) return {c.kind, false, 0, "Repair space contains another material."};
            }
        if (w.held_stone_ < count) return {c.kind, false, 0, "Not enough stone: need " + std::to_string(count) + ", held " + std::to_string(w.held_stone_) + ". Nothing changed."};
        for (std::size_t i = 0; i < count; ++i) w.set_material(fill[i], Material::stone);
        w.held_stone_ -= count;
        return {c.kind, true, count, "Repaired " + std::to_string(count) + " material cells."};
    }
    return {c.kind, false, 0, "Unknown world command."};
}
} // namespace pixel
