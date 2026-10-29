#include "godot_host/session_node.hpp"
#include "godot_host/material_definition_resource.hpp"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/os.hpp>
#include "persistence/world_file.hpp"
#include "physics/box2d_solver.hpp"
#include <utility>
#include <exception>

namespace pixel::host {
using namespace godot;

void SandboxSessionNode::_bind_methods() {
    ClassDB::bind_method(D_METHOD("start_world"), &SandboxSessionNode::start_world);
    ClassDB::bind_method(D_METHOD("start_wall_world"), &SandboxSessionNode::start_wall_world);
    ClassDB::bind_method(D_METHOD("start_material_lab"), &SandboxSessionNode::start_material_lab);
    ClassDB::bind_method(D_METHOD("request_single_step"), &SandboxSessionNode::request_single_step);
    ClassDB::bind_method(D_METHOD("request_reset_lab"), &SandboxSessionNode::request_reset_lab);
    ClassDB::bind_method(D_METHOD("request_save"), &SandboxSessionNode::request_save);
    ClassDB::bind_method(D_METHOD("request_load"), &SandboxSessionNode::request_load);
    ClassDB::bind_method(D_METHOD("toggle_pause"), &SandboxSessionNode::toggle_pause);
    ClassDB::bind_method(D_METHOD("end_session"), &SandboxSessionNode::end_session);
    ClassDB::bind_method(D_METHOD("get_session_state"), &SandboxSessionNode::get_session_state);
    ClassDB::bind_method(D_METHOD("get_tick"), &SandboxSessionNode::get_tick);
    ClassDB::bind_method(D_METHOD("has_session"), &SandboxSessionNode::has_session);
    ClassDB::bind_method(D_METHOD("get_last_error"), &SandboxSessionNode::get_last_error);
    ClassDB::bind_method(D_METHOD("get_content_count"), &SandboxSessionNode::get_content_count);
    ADD_SIGNAL(MethodInfo("session_changed"));
}

bool SandboxSessionNode::report_error(const String& message) {
    last_error_ = message;
    UtilityFunctions::push_error("[session] ", message);
    return false;
}

void SandboxSessionNode::_enter_tree() {
    set_physics_process(false);
    if (Engine::get_singleton()->is_editor_hint()) return;
    if (active_host_ && active_host_ != this) {
        report_error("Only one SandboxSessionNode may own a game session.");
        return;
    }
    active_host_ = this;
    smoke_mode_ = OS::get_singleton()->get_cmdline_user_args().has("--smoke-test") || OS::get_singleton()->get_cmdline_user_args().has("--material-soak-test");
    set_physics_process(true);
}

void SandboxSessionNode::_exit_tree() {
    end_session();
    if (active_host_ == this) active_host_ = nullptr;
}

std::unique_ptr<GameSession> SandboxSessionNode::make_session() {
    auto loaded = load_lab_content();
    if (!loaded) { last_error_ = String::utf8(loaded.error.c_str()); return nullptr; }
    return std::make_unique<GameSession>(std::move(loaded.registry),make_box2d_solver());
}
bool SandboxSessionNode::start_world() { return start(0); }
bool SandboxSessionNode::start_wall_world() { return start(1); }
bool SandboxSessionNode::start_material_lab() { return start(2); }
bool SandboxSessionNode::start(int scenario) {
    if (active_host_ != this) return report_error("This node is not the active session owner.");
    if (session_) return report_error("A session already exists; return to the main menu first.");
    if (Engine::get_singleton()->get_physics_ticks_per_second() != GameSession::ticks_per_second) {
        return report_error("Project physics tick rate must be 60 Hz.");
    }
    auto candidate = make_session();
    if (!candidate) return report_error(last_error_);
    const auto result = scenario==2 ? candidate->create_material_lab(20261027,true) : scenario==1 ? candidate->create_stone_wall_world(20261027) : candidate->create_empty_world(1);
    if (!result) return report_error(String::utf8(result.error.c_str()));
    session_ = std::move(candidate);
    simulation_failed_ = false;
    last_error_ = "";
    ended_ = false;
    action_status_ = scenario==2 ? "Liquids: falling water / oil-water layers / empty basin. Left paint, right erase, Tab panels." : scenario==1 ? "Left: local dig / Right: fill 8x8 / Pick up: collect all" : "Empty world; no initial stone.";
    UtilityFunctions::print("[session] Created ", scenario==2 ? "material laboratory" : scenario==1 ? "stone wall" : "empty world", " tick=0; converted materials=", get_content_count());
    emit_signal("session_changed");
    return true;
}

bool SandboxSessionNode::toggle_pause() {
    if(simulation_failed_)return report_error("Simulation failed; load or reset the world before resuming.");
    if (!session_) return report_error("There is no active session to pause or resume.");
    const auto result = session_->state() == SessionState::paused ? session_->resume() : session_->pause();
    if (!result) return report_error(String::utf8(result.error.c_str()));
    UtilityFunctions::print("[session] ", get_session_state(), " tick=", get_tick());
    emit_signal("session_changed");
    return true;
}

void SandboxSessionNode::end_session() {
    simulation_failed_=false;
    boundary_request_ = BoundaryRequest::none;
    single_step_=false;
    if (!session_) return;
    UtilityFunctions::print("[session] Ended tick=", get_tick(), "; releasing world and content registry");
    session_->end();
    session_.reset();
    ended_ = true;
    emit_signal("session_changed");
}

void SandboxSessionNode::_physics_process(double /*delta*/) {
    if (active_host_ != this) return;
    // Loads commit at a boundary without advancing either old or loaded world.
    if (boundary_request_ == BoundaryRequest::load || boundary_request_==BoundaryRequest::reset) { process_boundary_request(); return; }
    if(simulation_failed_)return;
    const bool step=std::exchange(single_step_,false) && session_ && session_->state()==SessionState::paused;
    if(step) static_cast<void>(session_->resume());
    try { if (session_ && session_->advance_fixed_step()) {
        for (const auto& result : session_->command_results()) {
            action_status_ = String::utf8(result.message.c_str());
            UtilityFunctions::print("[command] ", action_status_);
        }
    }} catch(const std::exception& error){
        simulation_failed_=true;boundary_request_=BoundaryRequest::none;
        if(session_ && session_->state()==SessionState::running)static_cast<void>(session_->pause());
        static_cast<void>(report_error(String("Native simulation stopped: ")+String::utf8(error.what())));
        action_status_=last_error_+" Load or reset to recover; saving this incomplete step is disabled.";
        io_ok_=false;io_status_="Simulation failed; previous save preserved.";++io_serial_;return;
    }
    if(step) static_cast<void>(session_->pause());
    // A save snapshots the fully completed step, including this frame's commands.
    if (boundary_request_ == BoundaryRequest::save) process_boundary_request();
}

bool SandboxSessionNode::submit_command(WorldCommand command) {
    if(simulation_failed_){action_status_="Simulation failed; load or reset before editing.";return false;}
    const auto result = session_ ? session_->submit(command) : SessionResult{false, "No active world."};
    action_status_ = result ? "Command queued for next fixed step." : String::utf8(result.error.c_str());
    return result.ok;
}
ChangeBatch SandboxSessionNode::consume_changes() { return session_ ? session_->take_changes() : ChangeBatch{}; }
std::filesystem::path SandboxSessionNode::save_path() const {
    const String relative = smoke_mode_ ? "user://smoke/stone_wall-" + String::num_int64(OS::get_singleton()->get_process_id()) + ".psw" : "user://saves/stone_wall.psw";
    const auto path = ProjectSettings::get_singleton()->globalize_path(relative);
    const auto utf8 = path.utf8();
    return std::filesystem::path(std::u8string(utf8.get_data(), utf8.get_data() + utf8.length()));
}
void SandboxSessionNode::request_save() { if(simulation_failed_){io_ok_=false;io_status_="Save blocked: incomplete simulation step. Load or reset first.";++io_serial_;return;}boundary_request_ = BoundaryRequest::save; }
void SandboxSessionNode::request_load() { boundary_request_ = BoundaryRequest::load; }
void SandboxSessionNode::request_single_step() {
    if(simulation_failed_){action_status_="Simulation failed; load or reset before stepping.";return;}
    if(session_ && session_->state()==SessionState::paused)single_step_=true;
    else action_status_="Pause the world before single stepping.";
}
void SandboxSessionNode::request_reset_lab() { boundary_request_=BoundaryRequest::reset; }
void SandboxSessionNode::process_boundary_request() {
    const auto request = std::exchange(boundary_request_, BoundaryRequest::none);
    single_step_=false;
    ++io_serial_;
    io_ok_ = false;
    if(request==BoundaryRequest::reset){
        if(!session_ || !session_->published_world()->material_lab){io_status_="Reset requires material laboratory.";return;}
        auto candidate=make_session();
        const auto result=candidate?candidate->create_material_lab(session_->published_world()->seed,true):SessionResult{false,"Cannot reload material catalog."};
        if(result){
            if(session_->state()==SessionState::paused)static_cast<void>(candidate->pause());
            session_=std::move(candidate);simulation_failed_=false;last_error_="";action_status_="Laboratory reset to tick 0; commands cleared.";io_ok_=true;io_status_="Laboratory reset to tick 0.";
        }
        else io_status_="Reset failed: "+String::utf8(result.error.c_str());
    } else if (request == BoundaryRequest::save) {
        const auto snapshot = session_ ? session_->capture_snapshot() : std::nullopt;
        if (!snapshot) { io_status_ = "Save failed: no active world."; return; }
        const auto result = persistence::save_world_file(save_path(), *snapshot);
        io_ok_ = result.ok;
        io_status_ = result.ok ? "Saved tick " + String::num_uint64(snapshot->tick) : "Save failed: " + String::utf8(result.error.c_str());
        io_status_ += " / " + String::num(result.elapsed_ms, 3) + " ms";
    } else if (request == BoundaryRequest::load) {
        const auto loaded = persistence::load_world_file(save_path());
        if (!loaded.snapshot) io_status_ = "Load failed: " + String::utf8(loaded.error.c_str());
        else {
            auto candidate = session_ ? nullptr : make_session();
            auto* target = session_ ? session_.get() : candidate.get();
            const auto result = target ? target->restore_world(*loaded.snapshot) : SessionResult{false, last_error_.utf8().get_data()};
            if (result && candidate) session_ = std::move(candidate);
            io_ok_ = result.ok;
            io_status_ = result ? "Loaded tick " + String::num_uint64(loaded.snapshot->tick) + " (paused)" : "Load failed: " + String::utf8(result.error.c_str());
            if (result) { simulation_failed_=false;ended_ = false; last_error_ = ""; action_status_ = "Loaded exact snapshot. Resume to edit."; }
        }
        io_status_ += " / " + String::num(loaded.elapsed_ms, 3) + " ms";
    }
    UtilityFunctions::print("[persistence] ", io_status_);
    emit_signal("session_changed");
}

String SandboxSessionNode::get_session_state() const {
    return session_ ? state_name(session_->state()) : (ended_ ? "Ended" : "Idle");
}

int64_t SandboxSessionNode::get_tick() const {
    const auto view = session_ ? session_->published_world() : std::nullopt;
    return view ? static_cast<int64_t>(view->tick) : 0;
}

int64_t SandboxSessionNode::get_content_count() const {
    return session_ ? static_cast<int64_t>(session_->content().material_count()) : 0;
}
} // namespace pixel::host
