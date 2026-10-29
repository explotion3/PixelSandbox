#include "session/game_session.hpp"
#include <chrono>

#include <limits>
#include <stdexcept>
#include <utility>
#include <algorithm>
#include <cassert>
#include "simulation/material_simulation.hpp"

namespace pixel {

const char* state_name(SessionState state) noexcept {
    switch (state) {
        case SessionState::idle: return "Idle";
        case SessionState::running: return "Running";
        case SessionState::paused: return "Paused";
        case SessionState::ended: return "Ended";
    }
    return "Unknown";
}

GameSession::GameSession(std::shared_ptr<const ContentRegistry> content,std::unique_ptr<RigidSolver> solver) : content_(std::move(content)),rigid_solver_(std::move(solver)) {
    if (!content_) {
        throw std::invalid_argument("GameSession requires a validated, immutable ContentRegistry.");
    }
    commands_.reserve(256);
    results_.reserve(256);
}

SessionResult GameSession::create_empty_world(std::uint64_t seed) {
    if (world_) {
        return {false, "End the current world before creating another one."};
    }
    world_.emplace(seed);
    state_ = SessionState::running;
    publish_full_world();
    return {true, {}};
}

void GameSession::publish_full_world() {
    if(rigid_solver_)rigid_solver_->reset();
    changes_ = {++publication_, true, true, {}};
    changes_.dirty_chunks.reserve(world_->chunks_.size());
    world_->dirty_chunks_.clear();
    for (auto& chunk : world_->chunks_) chunk.dirty = false;
}

SessionResult GameSession::create_material_lab(std::uint64_t seed, bool liquids_only) {
    if (world_) return {false,"End the current world before creating another one."};
    for (const auto id : {"material.stone","material.sand","material.water","material.oil","material.wood","material.fire","material.smoke","material.ice","material.water_static"})
        if (!content_->find_material(id)) return {false,std::string("Laboratory requires material: ")+id};
    world_.emplace(MaterialSimulation::make_lab(seed,*content_,liquids_only));
    state_=SessionState::running;publish_full_world();return {true,{}};
}

SessionResult GameSession::create_stone_wall_world(std::uint64_t seed) {
    if (world_) return {false, "End the current world before creating another one."};
    if (!content_->find_material("material.stone")) return {false, "Missing material definition: material.stone"};
    world_.emplace(make_stone_wall_scenario(seed));
    state_ = SessionState::running;
    publish_full_world();
    return {true, {}};
}

SessionResult GameSession::submit(WorldCommand command) {
    if (!world_ || (state_ != SessionState::running && !(state_ == SessionState::paused && world_->material_lab_))) return {false, "World commands require a running session (resume first)."};
    if (commands_.size() >= 256) return {false, "World command queue is full (256); try again next tick."};
    commands_.push_back(command);
    return {true, {}};
}

SessionResult GameSession::restore_world(const WorldSnapshot& snapshot) {
    const auto error = validate_snapshot(snapshot);
    if (!error.empty()) return {false, error};
    if(!snapshot.bodies.empty() && !rigid_solver_)return {false,"This session has no rigid physics solver; current world was preserved."};
    for (const auto& id : snapshot.material_ids)
        if (!content_->find_material(id)) return {false, "Missing material definition: " + id};
    if (snapshot.material_ids.front() != "material.stone") return {false, "Material mapping slot 1 must be material.stone."};
    for (std::size_t i=0;i<snapshot.cells.size();++i) {
        if (!snapshot.cells[i]) continue;
        const auto* d=content_->find_material(snapshot.material_ids[snapshot.cells[i]-1]);
        if(d->cell_type=="solid")return {false,"Rigid material cannot be stored as fixed grid terrain: "+d->id};
        const auto& s=snapshot.states[i];
        if (s.damage_taken>static_cast<std::uint32_t>(d->hit_points) || (!d->source_gas && s.gas_counter) || (d->lifetime && (!s.life || s.life>d->lifetime)) || (!d->lifetime && s.life) || s.fuel>d->fuel || (!d->source_burning && s.burning && !s.fuel) ||
            (!d->source_burning && (s.fire_intensity || s.burning_ticks || s.fire_color || s.ignition_pending)))
            return {false,"Saved lifetime/fuel does not match material definition: "+d->id};
        if ((!d->source_liquid && (s.liquid_fall || s.liquid_lateral || s.liquid_direction)) || (s.mobilized && (!d->can_loosen || s.quiet_ticks>=d->settle_ticks)) ||
            (s.source_attachment && (!d->source_adhesion || d->is_static || d->liquid_sticks_to_ceiling<=0)) ||
            (s.detonation_ticks && (!d->explosion.radius || !s.fuel || s.detonation_ticks>d->explosion.delay_ticks)) ||
            ((!d->adhesion_strength_q8 || d->is_static) && (s.adhesion_ticks || s.adhesion_cooldown)) ||
            (s.adhesion_ticks && (!d->adhesion_hold_ticks || s.adhesion_ticks>=d->adhesion_hold_ticks)))
            return {false,"Saved mechanical state does not match material definition: "+d->id};
    }
    for(const auto& b:snapshot.bodies)for(const auto& p:b.pixels){
        const auto* d=content_->find_material(snapshot.material_ids[p.material-1]);
        if(p.state.damage_taken>static_cast<std::uint32_t>(d->hit_points) || p.state.gas_counter || (d->motion()!=Motion::fixed && d->motion()!=Motion::rigid) || (d->cell_type!="solid" && !d->sand) || (d->lifetime && (!p.state.life || p.state.life>d->lifetime)) || (!d->lifetime && p.state.life) || p.state.fuel>d->fuel ||
            (!d->source_burning && ((p.state.burning && !p.state.fuel) || p.state.fire_intensity || p.state.burning_ticks || p.state.fire_color || p.state.ignition_pending)) ||
            (p.state.detonation_ticks && (!d->explosion.radius || !p.state.fuel || p.state.detonation_ticks>d->explosion.delay_ticks)))
            return {false,"Rigid pixel does not match a fixed solid definition: "+d->id};
    }
    auto candidate = WorldState::from_valid_snapshot(snapshot);
    if(snapshot.material_lab){
        // Preserve saved indices, append newly available definitions for reaction products.
        for(const auto& definition:content_->materials())
            if(candidate.material_index(definition.id)==Material::air)candidate.material_ids_.push_back(definition.id);
        candidate.counts_.resize(candidate.material_ids_.size()+1,0);
    }
    world_.emplace(std::move(candidate)); // Commit only after complete validation and allocation.
    commands_.clear(); results_.clear();
    state_ = SessionState::paused;
    publish_full_world();
    return {true, {}};
}

std::optional<WorldSnapshot> GameSession::capture_snapshot() const {
    return world_ ? std::optional{world_->snapshot()} : std::nullopt;
}

ChangeBatch GameSession::take_changes() {
    if (!changes_.full_refresh && !changes_.drops_changed && changes_.dirty_chunks.empty() && !changes_.bodies_changed) return {};
    auto batch = std::move(changes_);
    changes_ = {};
    if (world_) changes_.dirty_chunks.reserve(world_->chunks_.size());
    return batch;
}

SessionResult GameSession::pause() {
    if (state_ != SessionState::running) {
        return {false, "Pause requires a running session."};
    }
    state_ = SessionState::paused;
    return {true, {}};
}

SessionResult GameSession::resume() {
    if (state_ != SessionState::paused) {
        return {false, "Resume requires a paused session."};
    }
    state_ = SessionState::running;
    return {true, {}};
}

void GameSession::end() noexcept {
    if(rigid_solver_)rigid_solver_->reset();
    world_.reset();
    commands_.clear(); results_.clear();
    changes_ = {};
    state_ = SessionState::ended;
}

bool GameSession::advance_fixed_step() {
    if (state_ != SessionState::running || !world_ || world_->tick_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const auto step_start=std::chrono::steady_clock::now();
    // Single-threaded phases: consume FIFO commands -> commit rules -> tick -> publish.
    results_.clear();
    const auto body_before=world_->body_revision_;
    world_->simulation_stats_={}; // Includes command-phase explosions in this fixed-step publication.
    for (const auto& command : commands_) {
        const auto* definition=content_->find_material(command.material_id);
        const bool body_create=command.kind==CommandKind::spawn_body || command.kind==CommandKind::detach_body ||
            (command.kind==CommandKind::paint && definition && definition->cell_type=="solid");
        CommandResult result{command.kind,false,0,"No command handler."};
        if(body_create){
            result=rigid_solver_?RigidSimulation::create(*world_,*content_,command,command.kind==CommandKind::detach_body):CommandResult{command.kind,false,0,"Rigid tools require a configured physics solver."};
        }else if(world_->material_lab_ && !world_->bodies_.empty() && (command.kind==CommandKind::erase || command.kind==CommandKind::push || command.kind==CommandKind::heat || command.kind==CommandKind::cool)){
            result=RigidSimulation::edit(*world_,*content_,command);
            if(result.ok){auto grid=MaterialSimulation::command(*world_,*content_,command);result.amount+=grid.amount;result.message+=" "+grid.message;result.ok=grid.ok;}
        }else{
            bool body_hit=false;
            if(command.radius>=1 && command.radius<=8 && world_->bounds_.contains(command.target) &&
               command.kind==CommandKind::loosen){
                const int radius=command.radius;
                for(int y=-radius;y<=radius && !body_hit;++y)for(int x=-radius;x<=radius;++x)if(x*x+y*y<=radius*radius && world_->body_pixel_at({command.target.x+x,command.target.y+y})){body_hit=true;break;}
            }
            result=body_hit?CommandResult{command.kind,false,0,"Rigid loosen coupling is not implemented in this bridge revision."}:
                command.kind<=CommandKind::repair?StoneRules::apply(*world_,command):MaterialSimulation::command(*world_,*content_,command);
        }
        if (result.ok && result.amount > 0) changes_.drops_changed = true;
        results_.push_back(std::move(result));
        assert(world_->world_stone_ + world_->dropped_stone_ + world_->held_stone_ == world_->initial_stone_);
    }
    commands_.clear();
    MaterialSimulation::step(*world_,*content_);
    if(rigid_solver_)RigidSimulation::advance(*world_,*content_,*rigid_solver_,fixed_delta_seconds);
    world_->simulation_stats_.simulation_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-step_start).count();
    assert(world_->world_stone_ + world_->dropped_stone_ + world_->held_stone_ == world_->initial_stone_);
    ++world_->tick_;
    for (auto chunk : world_->dirty_chunks_)
        if (std::find(changes_.dirty_chunks.begin(), changes_.dirty_chunks.end(), chunk) == changes_.dirty_chunks.end()) changes_.dirty_chunks.push_back(chunk);
    if(body_before!=world_->body_revision_)changes_.bodies_changed=true;
    if (!world_->dirty_chunks_.empty() || changes_.drops_changed || changes_.bodies_changed) changes_.sequence = ++publication_;
    world_->dirty_chunks_.clear();
    for (auto& chunk : world_->chunks_) chunk.dirty = false;
    return true;
}

std::optional<WorldView> GameSession::published_world() const noexcept {
    return world_ ? std::optional{world_->view()} : std::nullopt;
}

} // namespace pixel
