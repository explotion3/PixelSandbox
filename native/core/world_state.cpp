#include "core/world_state.hpp"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <unordered_set>
#include <cmath>

namespace pixel {
bool valid_bounds(const WorldBounds& b) noexcept {
    return b.width > 0 && b.height > 0 && b.width <= 512 && b.height <= 512 &&
        b.origin.x >= -1000000 && b.origin.x <= 1000000 && b.origin.y >= -1000000 && b.origin.y <= 1000000;
}
namespace {
bool valid_interaction_state(const CellState& s,std::uint64_t tick){return std::isfinite(s.liquid_fall) && s.liquid_fall>=0 && s.liquid_fall<=65536 && std::isfinite(s.liquid_lateral) && std::abs(s.liquid_lateral)<=65536 && s.liquid_direction<=3 && s.damage_taken<=1000000000 && s.gas_counter>=-18000 && s.gas_counter<=60001 && (s.electric_stamp==UINT64_MAX || s.electric_stamp<=tick) && (s.source_attachment<=1 || (s.source_attachment>=5 && s.source_attachment<=7));}
}
std::string validate_snapshot(const WorldSnapshot& s) {
    if (s.version != save_format_version) return "Unknown save format version: " + std::to_string(s.version);
    if (!valid_bounds(s.bounds)) return "Invalid world bounds (maximum 512 x 512).";
    if (s.chunk_edge != chunk_size || s.building_edge != building_grid_size) return "Unsupported chunk/building grid ratio.";
    if (s.material_ids.empty() || s.material_ids.size() > 65534) return "Invalid material content mapping.";
    std::unordered_set<std::string> ids;
    for (const auto& id : s.material_ids)
        if (id.empty() || id.size() > 128 || !ids.insert(id).second) return "Invalid or duplicate saved material ID.";
    const auto count = static_cast<std::size_t>(s.bounds.width) * s.bounds.height;
    if (s.cells.size() != count || s.states.size() != count) return "Material/state shape size does not match world dimensions.";
    if(s.bodies.size()>64 || !s.next_body_id)return "Invalid rigid body count/next ID.";
    std::size_t body_pixels=0;std::unordered_set<std::uint32_t> body_ids;
    for(const auto& b:s.bodies){
        if(!b.id || b.id>=s.next_body_id || !body_ids.insert(b.id).second || !b.shape_revision ||
           !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.angle) || !std::isfinite(b.vx) || !std::isfinite(b.vy) || !std::isfinite(b.angular_velocity) ||
           b.x<s.bounds.origin.x-64 || b.x>s.bounds.origin.x+s.bounds.width+64 || b.y<s.bounds.origin.y-64 || b.y>s.bounds.origin.y+s.bounds.height+64 ||
           std::abs(b.angle)>4 || std::abs(b.vx)>512 || std::abs(b.vy)>512 || std::abs(b.angular_velocity)>20 || b.pixels.empty() || b.pixels.size()>1024)return "Invalid rigid body identity, pose or shape size.";
        std::unordered_set<int> occupied;
        const auto cosine=std::cos(b.angle),sine=std::sin(b.angle);
        for(const auto& p:b.pixels){
            const auto x=b.x+cosine*p.local.x-sine*p.local.y,y=b.y+sine*p.local.x+cosine*p.local.y;
            if(x<s.bounds.origin.x-0.25 || y<s.bounds.origin.y-0.25 || x>s.bounds.origin.x+s.bounds.width+0.25 || y>s.bounds.origin.y+s.bounds.height+0.25)
                return "Rigid pixel is outside the finite world.";
            if(!valid_interaction_state(p.state,s.tick) || p.local.x<-32 || p.local.x>32 || p.local.y<-32 || p.local.y>32 || !p.material || p.material>s.material_ids.size() ||
               !occupied.insert((p.local.y+32)*65+p.local.x+32).second || p.state.created_tick>s.tick || p.state.temperature<-1000 || p.state.temperature>10000 ||
               p.state.life>60000 || p.state.liquid_fall || p.state.liquid_lateral || p.state.liquid_direction || p.state.mobilized || p.state.quiet_ticks || p.state.adhesion_ticks || p.state.adhesion_cooldown || p.state.source_attachment || p.state.detonation_ticks>600 || p.state.electrified_ticks>60 || p.state.liquid_fraction_x>=16777216 || p.state.liquid_fraction_y>=16777216 ||
               p.state.momentum_remainder_x<-255 || p.state.momentum_remainder_x>255 || p.state.momentum_remainder_y<-255 || p.state.momentum_remainder_y>255 ||
               p.state.fuel>1000000000 || (p.state.ignition_pending && !p.state.burning) || p.state.velocity_x || p.state.velocity_y || p.state.vertical_fraction || p.state.horizontal_fraction)return "Invalid rigid pixel material/state.";
        }
        std::vector<int> connected{*occupied.begin()};std::unordered_set<int> seen{connected.front()};
        for(std::size_t i=0;i<connected.size();++i){const int at=connected[i];for(const int delta:{-65,-1,1,65}){
            if((delta==-1 && at%65==0) || (delta==1 && at%65==64))continue;
            const int next=at+delta;if(occupied.contains(next) && seen.insert(next).second)connected.push_back(next);
        }}
        if(connected.size()!=b.pixels.size())return "Rigid shape is disconnected; fragments must have separate IDs.";
        body_pixels+=b.pixels.size();
    }
    if (s.initial_stone > count+body_pixels || s.held_stone > s.initial_stone) return "Invalid initial/held stone quantity.";
    std::uint64_t total = s.held_stone;
    for(const auto& b:s.bodies)for(const auto& p:b.pixels)if(p.material==1)++total;
    for (std::size_t i = 0; i < count; ++i) {
        if (s.cells[i] > s.material_ids.size()) return "Cell refers to an invalid material index.";
        if (s.cells[i] == 1) ++total; // Mapping slot 1 is reserved for stone, checked against content on restore.
        const auto& state = s.states[i];
        if (!valid_interaction_state(state,s.tick) || state.temperature < -1000 || state.temperature > 10000 || state.life > 60000 || state.fuel > 1000000000 ||
            state.created_tick>s.tick || state.velocity_x<-8192 || state.velocity_x>8192 || state.velocity_y<-8192 || state.velocity_y>8192 ||
            state.vertical_fraction>255 || state.horizontal_fraction>255 ||
            state.quiet_ticks>600 || (!state.mobilized && state.quiet_ticks) || state.adhesion_ticks>60000 || state.adhesion_cooldown>16 ||
            state.detonation_ticks>600 || state.electrified_ticks>60 || state.liquid_fraction_x>=16777216 || state.liquid_fraction_y>=16777216 ||
            state.momentum_remainder_x<-255 || state.momentum_remainder_x>255 || state.momentum_remainder_y<-255 || state.momentum_remainder_y>255 ||
            (state.ignition_pending && !state.burning) || (s.cells[i] == 0 && state != CellState{})) return "Invalid per-cell simulation state.";
    }
    if(s.electric_pulses.size()>count+body_pixels)return "Too many electric pulse heads.";
    auto valid_address=[&](CellPos pos,std::uint32_t body){
        if(!body)return s.bounds.contains(pos);
        const auto found=std::find_if(s.bodies.begin(),s.bodies.end(),[&](const auto& b){return b.id==body;});
        return found!=s.bodies.end() && std::any_of(found->pixels.begin(),found->pixels.end(),[&](const auto& p){return p.local==pos;});
    };
    std::unordered_set<std::uint64_t> pulse_heads;
    for(const auto& p:s.electric_pulses){
        if(!valid_address(p.position,p.body) || !valid_address(p.previous,p.previous_body) ||
            (!p.source_rules && p.body==p.previous_body && std::abs(p.position.x-p.previous.x)+std::abs(p.position.y-p.previous.y)>1) ||
            !p.energy || p.energy>10000 || !p.speed || p.speed>64 || !p.heat_interval || p.heat_interval>10000 ||
            !std::isfinite(p.avg_x) || !std::isfinite(p.avg_y) || std::abs(p.avg_x)>65536 || std::abs(p.avg_y)>65536 || !std::isfinite(p.probability_to_heat) || p.probability_to_heat<0 || p.probability_to_heat>1 || p.splittings_left>16 || p.splitting_energy>10000 ||
            p.heat>1000 || !p.glow_ticks || p.glow_ticks>60)return "Invalid electric pulse parameters or path.";
        const auto index=p.body?static_cast<std::uint64_t>((p.position.y+32)*65+p.position.x+32):static_cast<std::uint64_t>(p.position.y-s.bounds.origin.y)*s.bounds.width+static_cast<std::uint64_t>(p.position.x-s.bounds.origin.x);
        if(!p.source_rules && !pulse_heads.insert((static_cast<std::uint64_t>(p.body)<<32)|index).second)return "Duplicate electric pulse position.";
    }
    if (s.drops.size() > 100) return "Too many aggregated stone drops.";
    std::vector<ChunkPos> occupied;
    for (const auto& drop : s.drops) {
        if (!s.bounds.contains(drop.position) || drop.amount == 0 || drop.amount > s.initial_stone) return "Invalid drop position or quantity.";
        const auto chunk = cell_to_chunk(drop.position);
        if (std::find(occupied.begin(), occupied.end(), chunk) != occupied.end()) return "Duplicate aggregated drop chunk.";
        occupied.push_back(chunk);
        total += drop.amount;
    }
    if (total != s.initial_stone) return "Stone conservation check failed: world + drops + held != initial.";
    return {};
}
WorldState::WorldState(std::uint64_t seed, WorldBounds bounds) : seed_(seed), bounds_(bounds) {
    if (!valid_bounds(bounds)) throw std::invalid_argument("Invalid finite world bounds.");
    const auto first = cell_to_chunk(bounds.origin);
    const auto last = cell_to_chunk({bounds.origin.x + bounds.width - 1, bounds.origin.y + bounds.height - 1});
    first_chunk_=first;chunk_columns_=last.x-first.x+1;chunk_rows_=last.y-first.y+1;
    for (int y = first.y; y <= last.y; ++y)
        for (int x = first.x; x <= last.x; ++x) chunks_.push_back({{x, y}, {}, {}, false, true});
    dirty_chunks_.reserve(chunks_.size());
    drops_.reserve(chunks_.size());
    counts_[0] = static_cast<std::uint64_t>(bounds.width) * bounds.height;
}
std::size_t WorldState::chunk_index(ChunkPos p) const noexcept {
    const int x=p.x-first_chunk_.x,y=p.y-first_chunk_.y;
    if(x<0 || x>=chunk_columns_ || y<0 || y>=chunk_rows_)return chunks_.size();
    return static_cast<std::size_t>(y*chunk_columns_+x);
}
const MaterialChunk* WorldState::chunk_at(ChunkPos p) const noexcept {
    const auto index = chunk_index(p);
    return index < chunks_.size() ? &chunks_[index] : nullptr;
}
std::optional<Material> WorldState::material_at(CellPos p) const noexcept {
    if (!bounds_.contains(p)) return std::nullopt;
    const auto local = cell_in_chunk(p);
    return chunks_[chunk_index(cell_to_chunk(p))].cells[static_cast<std::size_t>(local.y * chunk_size + local.x)];
}
const CellState* WorldState::cell_state(CellPos p) const noexcept {
    if (!bounds_.contains(p)) return nullptr;
    const auto local = cell_in_chunk(p);
    return &chunks_[chunk_index(cell_to_chunk(p))].states[static_cast<std::size_t>(local.y * chunk_size + local.x)];
}
std::optional<BodyPixelAddress> WorldState::body_pixel_at(CellPos p) const noexcept {
    if(!bounds_.contains(p))return {};
    for(std::size_t i=bodies_.size();i>0;--i){
        const auto& bounds=body_bounds_[i-1];
        if(p.x+0.5<bounds[0] || p.y+0.5<bounds[1] || p.x+0.5>bounds[2] || p.y+0.5>bounds[3])continue;
        const auto& b=bodies_[i-1];const auto dx=p.x+0.5-b.x,dy=p.y+0.5-b.y,c=std::cos(b.angle),s=std::sin(b.angle);
        const CellPos local{static_cast<int>(std::floor(c*dx+s*dy+0.5)),static_cast<int>(std::floor(-s*dx+c*dy+0.5))};
        for(std::size_t j=0;j<b.pixels.size();++j)if(b.pixels[j].material && b.pixels[j].local==local)return BodyPixelAddress{i-1,j};
    }
    return {};
}
void WorldState::refresh_body_bounds(){
    body_bounds_.clear();body_bounds_.reserve(bodies_.size());
    for(const auto& b:bodies_){
        std::array<double,4> box{b.x+100,b.y+100,b.x-100,b.y-100};
        const auto c=std::cos(b.angle),s=std::sin(b.angle),margin=0.5*(std::abs(c)+std::abs(s));
        for(const auto& p:b.pixels){const auto x=b.x+c*p.local.x-s*p.local.y,y=b.y+s*p.local.x+c*p.local.y;box[0]=std::min(box[0],x-margin);box[1]=std::min(box[1],y-margin);box[2]=std::max(box[2],x+margin);box[3]=std::max(box[3],y+margin);}
        body_bounds_.push_back(box);
    }
}
std::optional<Material> WorldState::surface_material_at(CellPos p) const noexcept {
    const auto body=body_pixel_at(p);return body?std::optional{static_cast<Material>(bodies_[body->body].pixels[body->pixel].material)}:material_at(p);
}
const CellState* WorldState::surface_cell_state(CellPos p) const noexcept {
    const auto body=body_pixel_at(p);return body?&bodies_[body->body].pixels[body->pixel].state:cell_state(p);
}
Material WorldState::material_index(std::string_view id) const noexcept {
    const auto it = std::find(material_ids_.begin(), material_ids_.end(), id);
    return it == material_ids_.end() ? Material::air : static_cast<Material>(1 + it - material_ids_.begin());
}
void WorldState::wake(CellPos p) {
    const auto c = cell_to_chunk(p);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        const auto i = chunk_index({c.x+x,c.y+y});
        if (i < chunks_.size()) chunks_[i].awake = true;
    }
}
void WorldState::set_material(CellPos p, Material material) { set_cell(p, material, {}); }
void WorldState::set_cell(CellPos p, Material material, CellState state) {
    if (!bounds_.contains(p)) throw std::out_of_range("Material write outside finite world.");
    const auto chunk = cell_to_chunk(p);
    const auto local = cell_in_chunk(p);
    auto& block = chunks_[chunk_index(chunk)];
    const auto index = static_cast<std::size_t>(local.y * chunk_size + local.x);
    auto& previous = block.cells[index];
    if (previous == material && block.states[index] == state) return;
    if (previous != material) {
        --counts_[static_cast<std::uint16_t>(previous)]; ++counts_[static_cast<std::uint16_t>(material)];
        if (previous == Material::stone) --world_stone_;
        if (material == Material::stone) ++world_stone_;
    }
    if(previous!=material || block.states[index].mobilized!=state.mobilized)++block.collision_revision;
    if(block.states[index].electrified_ticks && !state.electrified_ticks)--electrified_cells_;
    if(!block.states[index].electrified_ticks && state.electrified_ticks)++electrified_cells_;
    const bool visible=previous!=material || block.states[index].burning!=state.burning || block.states[index].life!=state.life || (block.states[index].electrified_ticks>0)!=(state.electrified_ticks>0);
    previous = material; block.states[index] = state;
    if (visible && !block.dirty) { block.dirty = true; dirty_chunks_.push_back(chunk); }
    wake(p);
}
WorldView WorldState::view() const noexcept {
    auto v = simulation_stats_;
    v.freeze_chance=freeze_chance_;v.unfreeze_chance=unfreeze_chance_;
    v.seed=seed_; v.tick=tick_; v.bounds=bounds_; v.world_stone=world_stone_; v.dropped_stone=dropped_stone_;
    v.held_stone=held_stone_; v.initial_stone=initial_stone_; v.material_lab=material_lab_;
    v.electric_pulses=static_cast<std::uint32_t>(electric_pulses_.size());v.electric_energy=electric_energy_;v.electrified_cells=electrified_cells_;
    v.rigid_bodies=static_cast<std::uint32_t>(bodies_.size());for(const auto& b:bodies_)v.rigid_pixels+=static_cast<std::uint32_t>(b.pixels.size());
    return v;
}
WorldSnapshot WorldState::snapshot() const {
    WorldSnapshot s;
    s.seed=seed_; s.tick=tick_; s.bounds=bounds_; s.material_ids=material_ids_; s.material_lab=material_lab_;
    s.initial_stone=initial_stone_; s.held_stone=held_stone_; s.drops=drops_;
    s.freeze_chance=freeze_chance_;s.unfreeze_chance=unfreeze_chance_;
    s.electric_pulses=electric_pulses_;
    s.bodies=bodies_;s.next_body_id=next_body_id_;
    s.cells.reserve(static_cast<std::size_t>(bounds_.width) * bounds_.height); s.states.reserve(s.cells.capacity());
    for (int y=bounds_.origin.y; y<bounds_.origin.y+bounds_.height; ++y)
        for (int x=bounds_.origin.x; x<bounds_.origin.x+bounds_.width; ++x) {
            s.cells.push_back(static_cast<std::uint16_t>(*material_at({x,y})));
            s.states.push_back(*cell_state({x,y}));
        }
    return s;
}
WorldState WorldState::from_valid_snapshot(const WorldSnapshot& s) {
    WorldState w(s.seed,s.bounds); w.material_ids_=s.material_ids; w.material_lab_=s.material_lab;
    w.counts_.resize(s.material_ids.size()+1,0);
    std::size_t i=0;
    for (int y=s.bounds.origin.y; y<s.bounds.origin.y+s.bounds.height; ++y)
        for (int x=s.bounds.origin.x; x<s.bounds.origin.x+s.bounds.width; ++x,++i)
            w.set_cell({x,y},static_cast<Material>(s.cells[i]),s.states[i]);
    w.freeze_chance_=s.freeze_chance;w.unfreeze_chance_=s.unfreeze_chance;
    w.tick_=s.tick; w.initial_stone_=s.initial_stone; w.held_stone_=s.held_stone; w.drops_=s.drops;
    w.electric_pulses_=s.electric_pulses;for(const auto& p:w.electric_pulses_)w.electric_energy_+=p.energy;
    w.bodies_=s.bodies;w.next_body_id_=s.next_body_id;
    w.refresh_body_bounds();
    for(const auto& b:w.bodies_)for(const auto& p:b.pixels){++w.counts_[p.material];if(p.material==1)++w.world_stone_;if(p.state.electrified_ticks)++w.electrified_cells_;}
    for (const auto& drop:w.drops_) w.dropped_stone_+=drop.amount;
    w.drops_.reserve(w.chunks_.size());
    return w;
}
} // namespace pixel
