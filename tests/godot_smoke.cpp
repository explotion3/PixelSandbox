#include "godot_host/sandbox_app.hpp"
#include "godot_host/material_definition_resource.hpp"
#include "simulation/material_simulation.hpp"
#include "persistence/world_file.hpp"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <godot_cpp/classes/os.hpp>

namespace pixel::host {
using namespace godot;
bool SandboxApp::check(bool condition, const String& message) {
    if (condition) return true;
    smoke_finished_ = true;
    UtilityFunctions::push_error("SMOKE FAIL: ", message);
    get_tree()->quit(1);
    return false;
}

void SandboxApp::click_button(Button* button) {
    // Send input through Godot's GUI hit testing, focus and pressed signal path.
    const auto center = button->get_global_rect().get_center();
    Ref<InputEventMouseButton> event;
    event.instantiate();
    event->set_position(center);
    event->set_global_position(center);
    event->set_button_index(MOUSE_BUTTON_LEFT);
    event->set_pressed(true);
    get_viewport()->push_input(event, true);
    event->set_pressed(false);
    get_viewport()->push_input(event, true);
}

void SandboxApp::click_canvas(CellPos p, MouseButton button) {
    // Full-window canvas is covered by the inspector while it is expanded.
    // Exercise painting with the overlay closed, as a player would do.
    const bool panel_visible=inspector_->is_visible();
    inspector_->hide();
    const auto position = canvas_->get_global_position() + canvas_->cell_center(p);
    Ref<InputEventMouseButton> event;
    event.instantiate();
    event->set_position(position); event->set_global_position(position);
    event->set_button_index(button); event->set_pressed(true);
    get_viewport()->push_input(event, true);
    event->set_pressed(false);
    get_viewport()->push_input(event, true);
    inspector_->set_visible(panel_visible);
}

void SandboxApp::smoke_step(double delta) {
    // Automation drives actual GUI input and observes physics callbacks.
    // All worlds, counters and rules still belong to the runtime/session node.
    smoke_elapsed_ += delta;
    if (!check(smoke_elapsed_ < 75.0, "Timed out waiting for the physics-driven lifecycle.")) return;
    switch (smoke_phase_) {
        case 0: {
            if (!check(menu_->is_visible() && !session_node_->has_session(), "Initial main menu")) return;
            if (++wait_frames_ < 6) return; // Allow initial GUI layout and rendering.
            PackedStringArray paths;
            paths.push_back("res://content/materials/stone.tres");
            auto converted = load_content(paths);
            if (!check(bool(converted), "Resource conversion")) return;
            const auto* stone = converted.registry->find_material("material.stone");
            if (!check(stone && stone->hardness == 8 && stone->display_name == "Stone", "Native definition values")) return;
            const auto contacts=load_lab_content();
            const auto* ice_contact=contacts?contacts.registry->find_material("material.ice"):nullptr;
            const auto* wood_contact=contacts?contacts.registry->find_material("material.wood_prop"):nullptr;
            const auto* steel_contact=contacts?contacts.registry->find_material("material.steel"):nullptr;
            if(!check(ice_contact && wood_contact && steel_contact && std::abs(ice_contact->solid_friction-0.05f)<0.00001f && std::abs(wood_contact->solid_friction-0.9f)<0.00001f && std::abs(steel_contact->solid_friction-0.75f)<0.00001f,"Actual ice/wood/steel resources retain source contact friction"))return;
            const auto* original_fire=contacts.registry->find_material("material.fire");
            const auto* original_sand=contacts.registry->find_material("material.sand");
            if(!check(original_sand && original_sand->powder_liquid_speed_q8==102,"Actual sand resource exposes configured liquid sinking rate"))return;
            const auto* original_glue=contacts.registry->find_material("material.glue");
            const auto* original_slime=contacts.registry->find_material("material.slime");
            if(!check(original_glue && original_slime && original_glue->source_adhesion && original_glue->liquid_sticks_to_ceiling==100 &&
                original_slime->source_adhesion && original_slime->liquid_sticks_to_ceiling==40,"Actual glue/slime resources preserve raw source adhesion thresholds"))return;
            if(!check(wood_contact->source_burning && wood_contact->burnable && wood_contact->fire_hp==420 && wood_contact->generates_smoke==0 &&
                original_fire && original_fire->initially_burning && !original_fire->burnable,"Actual resources keep source combustibility, initial fire, signed threshold and independent smoke gate"))return;
            Ref<SandboxMaterialDefinition> direction_resource;direction_resource.instantiate();
            direction_resource->set_solid_friction(0.125);
            direction_resource->set("powder_liquid_speed_q8",77);
            direction_resource->set_content_id("material.direction_probe");direction_resource->set_gas_upward_speed_q8(128);
            direction_resource->set_gas_horizontal_speed_q8(512);direction_resource->set_gas_downward_speed_q8(768);
            MaterialDefinition direction_values;String conversion_error;
            if(!check(direction_resource->to_native(direction_values,conversion_error) && direction_values.gas_upward_speed_q8==128 &&
                direction_values.gas_horizontal_speed_q8==512 && direction_values.gas_downward_speed_q8==768,"C++ Resource converts nondefault gas direction parameters"))return;
            if(!check(direction_values.solid_friction==0.125f,"C++ Resource converts fractional contact friction"))return;
            if(!check(direction_values.powder_liquid_speed_q8==77,"Godot property binding converts nondefault powder sinking rate"))return;
            direction_resource->set_solid_friction(-0.1);
            if(!check(!direction_resource->to_native(direction_values,conversion_error) && conversion_error.contains("solid_friction"),"Resource rejects invalid contact friction with a specific field error"))return;
            direction_resource->set_solid_friction(0.6);
            direction_resource->set_can_loosen(true);direction_resource->set_settle_ticks(17);
            if(!check(direction_resource->to_native(direction_values,conversion_error) && direction_values.can_loosen && direction_values.settle_ticks==17,"Resource converts mechanical phase settings"))return;
            direction_resource->set_can_loosen(false);direction_resource->set_is_static(false);direction_resource->set_sand(false);
            direction_resource->set_slime(true);direction_resource->set_adhesion_strength_q8(300);direction_resource->set_adhesion_span(3);direction_resource->set_adhesion_hold_ticks(19);
            if(!check(direction_resource->to_native(direction_values,conversion_error) && direction_values.slime && direction_values.adhesion_strength_q8==300 &&
                direction_values.adhesion_span==3 && direction_values.adhesion_hold_ticks==19,"Resource converts adhesion strength, span and duration"))return;
            direction_resource->set_durability(9);direction_resource->set_hit_points(2500);direction_resource->set_explosion_radius(7);
            direction_resource->set_explosion_ray_energy(12345);direction_resource->set_explosion_max_durability(8);direction_resource->set_explosion_heat(80);
            direction_resource->set_explosion_impulse_q8(1000);direction_resource->set_explosion_delay_ticks(6);direction_resource->set_explosion_trigger_energy(1000);
            direction_resource->set_explosion_hole_enabled(false);direction_resource->set_explosion_destroy_liquids(true);
            if(!check(direction_resource->to_native(direction_values,conversion_error) && direction_values.durability==9 && direction_values.hit_points==2500 &&
                direction_values.explosion.radius==7 && direction_values.explosion.ray_energy==12345 && direction_values.explosion.max_durability==8 &&
                direction_values.explosion.heat==80 && direction_values.explosion.impulse_q8==1000 && direction_values.explosion.delay_ticks==6 &&
                direction_values.explosion.trigger_energy==1000 && !direction_values.explosion.hole_enabled && direction_values.explosion.destroy_liquids,"Resource converts native explosion parameters"))return;
            direction_resource->set_electrical_conductivity(true);direction_resource->set_electric_cost(7);
            if(!check(direction_resource->to_native(direction_values,conversion_error) && direction_values.electrical_conductivity && direction_values.electric_cost==7,"Resource converts electrical material parameters"))return;
            direction_resource->set_fuel(1000000);direction_resource->set_initially_burning(true);
            if(!check(direction_resource->to_native(direction_values,conversion_error) && direction_values.fuel==1000000 && direction_values.initially_burning,"Resource converts wide source fuel and initial burning without truncation"))return;
            direction_resource->set_source_burning(true);direction_resource->set_burnable(true);direction_resource->set_fire_hp(-1);
            direction_resource->set_generates_flames(37);direction_resource->set_generates_smoke(0);direction_resource->set_ignition_output("material.char");
            if(!check(direction_resource->to_native(direction_values,conversion_error) && direction_values.source_burning && direction_values.burnable &&
                direction_values.fire_hp==-1 && direction_values.generates_flames==37 && direction_values.generates_smoke==0 && direction_values.ignition_output=="material.char","Resource converts signed fire_hp, emission gates and ignition-entry material"))return;
            direction_resource->set_generates_flames(-1);
            if(!check(!direction_resource->to_native(direction_values,conversion_error) && conversion_error.contains("combustion"),"Resource rejects negative combustion gate"))return;
            const auto phases=load_lab_content();
            direction_resource->set_generates_flames(37);direction_resource->set_source_phase(true);direction_resource->set_source_gas(false);direction_resource->set_source_electricity(true);direction_resource->set_cold_chance_rev(100);direction_resource->set_warmth_chance_rev(500);direction_resource->set_explosion_source_rules(true);direction_resource->set_explosion_create_probability(40);
            if(!check(direction_resource->to_native(direction_values,conversion_error) && direction_values.source_phase && direction_values.source_electricity && direction_values.warmth_chance_rev==500 && direction_values.explosion.source_rules && direction_values.explosion.create_probability==40,"Resource converts recovered phase/electricity/explosion modes and gates"))return;
            direction_resource->set_cold_chance_rev(-1);if(!check(!direction_resource->to_native(direction_values,conversion_error),"Resource rejects invalid reverse phase gate"))return;direction_resource->set_cold_chance_rev(100);
            if(!check(phases && phases.registry->material_count()>=54 && phases.registry->reactions().size()>=57,"Actual source family recipes expand at registry load"))return;
            if(!check(std::count_if(phases.registry->reactions().begin(),phases.registry->reactions().end(),[](const auto& r){return r.probability_gate>=0;})>=145 &&
                std::any_of(phases.registry->reactions().begin(),phases.registry->reactions().end(),[](const auto& r){return r.input_a=="material.lava" && r.input_b=="material.water" && r.probability_gate==8000 && r.blob_radius_a==6 && r.restrict_a && r.restrict_b;}),"Actual text resources load concrete source gates and restricted radial parameters"))return;
            paths.push_back(paths[0]);
            auto duplicate = load_content(paths);
            if (!check(!duplicate && duplicate.error.find("Duplicate material ID") != std::string::npos, "Duplicate resource ID rejected")) return;
            PackedStringArray missing;
            missing.push_back("res://content/missing.tres");
            auto absent = load_content(missing);
            if (!check(!absent && absent.error.find("not found") != std::string::npos, "Missing resource rejected")) return;
            click_button(new_button_);
            if (!check(session_node_->has_session() && world_->is_visible() && session_node_->get_tick() == 0, "New world starts at zero")) return;
            observed_physics_frame_ = Engine::get_singleton()->get_physics_frames();
            ++smoke_phase_;
            break;
        }
        case 1:
            if (session_node_->get_tick() < 3) return;
            if (!check(static_cast<uint64_t>(session_node_->get_tick()) == Engine::get_singleton()->get_physics_frames() - observed_physics_frame_, "Exactly one logic tick per physics callback")) return;
            click_button(pause_button_);
            observed_tick_ = session_node_->get_tick();
            observed_physics_frame_ = Engine::get_singleton()->get_physics_frames();
            wait_frames_ = 0;
            if (!check(session_node_->get_session_state() == "Paused", "Pause button")) return;
            ++smoke_phase_;
            break;
        case 2:
            if (!check(session_node_->get_tick() == observed_tick_, "Paused world must not tick")) return;
            if (Engine::get_singleton()->get_physics_frames() - observed_physics_frame_ < 6) return;
            click_button(pause_button_);
            if (!check(session_node_->get_session_state() == "Running", "Resume button")) return;
            ++smoke_phase_;
            break;
        case 3:
            if (session_node_->get_tick() <= observed_tick_) return;
            click_button(return_button_);
            if (!check(!session_node_->has_session() && menu_->is_visible() && session_node_->get_session_state() == "Ended", "Return releases session")) return;
            observed_physics_frame_ = Engine::get_singleton()->get_physics_frames();
            ++smoke_phase_;
            break;
        case 4:
            if (!check(session_node_->get_tick() == 0 && !session_node_->has_session(), "Ended session stays released")) return;
            if (Engine::get_singleton()->get_physics_frames() - observed_physics_frame_ < 4) return;
            click_button(new_button_);
            if (!check(session_node_->get_tick() == 0 && session_node_->has_session(), "Reentry starts fresh")) return;
            ++smoke_phase_;
            break;
        case 5:
            if (session_node_->get_tick() < 2) return;
            click_button(pause_button_);
            click_button(return_button_);
            if (!check(!session_node_->has_session() && session_node_->get_content_count() == 0, "Exit paused world releases content")) return;
            click_button(new_button_);
            if (!check(session_node_->get_tick() == 0, "Third entry starts at zero")) return;
            session_node_->set_physics_process(false);
            observed_physics_frame_ = Engine::get_singleton()->get_physics_frames();
            ++smoke_phase_;
            break;
        case 6:
            if (!check(session_node_->get_tick() == 0, "Render callbacks must not advance logic")) return;
            if (Engine::get_singleton()->get_physics_frames() - observed_physics_frame_ < 4) return;
            session_node_->set_physics_process(true);
            ++smoke_phase_;
            break;
        case 7:
            if (session_node_->get_tick() < 2) return;
            click_button(return_button_);
            click_button(wall_button_);
            if (!check(session_node_->session()->published_world()->world_stone == 7168, "Authored stone wall starts with 7168 units")) return;
            wait_frames_ = 0;
            ++smoke_phase_;
            break;
        case 8:
            if (++wait_frames_ < 4) return;
            smoke_texture_creations_ = canvas_->texture_creations();
            smoke_texture_updates_ = canvas_->texture_updates();
            if (!check(canvas_->material_art_loaded(), "Noita source stone art loaded and decoded")) return;
            if (!check(smoke_texture_creations_ > 0, "World chunk textures created")) return;
            click_canvas({0,36}, MOUSE_BUTTON_LEFT);
            ++smoke_phase_;
            break;
        case 9: {
            const auto view = *session_node_->session()->published_world();
            if (view.dropped_stone == 0) return;
            smoke_removed_ = view.dropped_stone;
            if (!check(view.world_stone + view.dropped_stone + view.held_stone == view.initial_stone && smoke_removed_ == 49, "Cross-chunk GUI dig conserves exact amount")) return;
            click_canvas({0,36}, MOUSE_BUTTON_LEFT);
            observed_tick_ = session_node_->get_tick();
            ++smoke_phase_;
            break;
        }
        case 10:
            if (session_node_->get_tick() <= observed_tick_) return;
            if (!check(session_node_->session()->published_world()->dropped_stone == smoke_removed_, "Repeated GUI dig gives no extra drops")) return;
            if (!check(canvas_->texture_creations() == smoke_texture_creations_ && canvas_->texture_updates() > smoke_texture_updates_ && canvas_->texture_updates() <= smoke_texture_updates_ + 2, "Dirty chunk uploads reuse textures")) return;
            smoke_snapshot_ = session_node_->session()->capture_snapshot();
            click_canvas({-1,36}, MOUSE_BUTTON_RIGHT);
            observed_tick_ = session_node_->get_tick();
            ++smoke_phase_;
            break;
        case 11: {
            if (session_node_->get_tick() <= observed_tick_) return;
            auto actual = *session_node_->session()->capture_snapshot();
            actual.tick = smoke_snapshot_->tick;
            if (!check(actual == *smoke_snapshot_ && session_node_->get_action_status().contains("Not enough stone"), "GUI repair before pickup fails without partial mutation")) return;
            click_button(pickup_button_);
            ++smoke_phase_;
            break;
        }
        case 12:
            if (session_node_->session()->published_world()->held_stone != smoke_removed_) return;
            if (!check(session_node_->session()->published_world()->dropped_stone == 0, "GUI pickup transfers and removes all drops")) return;
            click_canvas({-1,36}, MOUSE_BUTTON_RIGHT);
            ++smoke_phase_;
            break;
        case 13: {
            const auto view = *session_node_->session()->published_world();
            if (view.held_stone == smoke_removed_) return;
            if (!check(view.held_stone > 0 && view.world_stone < view.initial_stone && view.world_stone + view.held_stone == view.initial_stone, "Partial repair leaves saved hole and held resource")) return;
            click_button(pause_button_);
            smoke_snapshot_ = session_node_->session()->capture_snapshot();
            smoke_io_serial_ = session_node_->io_serial();
            click_button(save_button_);
            ++smoke_phase_;
            break;
        }
        case 14:
            if (session_node_->io_serial() == smoke_io_serial_) return;
            if (!check(session_node_->io_ok() && session_node_->get_io_status().contains("Saved tick"), "GUI save at completed-step boundary")) return;
            click_button(return_button_);
            if (!check(!session_node_->has_session(), "Return releases stone world")) return;
            smoke_io_serial_ = session_node_->io_serial();
            click_button(menu_load_button_);
            ++smoke_phase_;
            break;
        case 15:
            if (session_node_->io_serial() == smoke_io_serial_) return;
            if (!check(session_node_->io_ok() && session_node_->has_session() && world_->is_visible() && session_node_->get_session_state() == "Paused", "Main-menu load opens paused world")) return;
            if (!check(session_node_->session()->capture_snapshot() == smoke_snapshot_, "GUI reload preserves complete shape, resources, seed and tick")) return;
            wait_frames_ = 0;
            ++smoke_phase_;
            break;
        case 16: {
            if (++wait_frames_ < 6) return; // Record loaded shape for graphical review.
            std::ofstream corrupt(session_node_->save_path(), std::ios::binary | std::ios::trunc);
            corrupt << "intentionally damaged smoke-test save";
            corrupt.close();
            if (!check(bool(corrupt), "Create corrupted fixture in isolated smoke path")) return;
            smoke_io_serial_ = session_node_->io_serial();
            click_button(load_button_);
            ++smoke_phase_;
            break;
        }
        case 17:
            if (session_node_->io_serial() == smoke_io_serial_) return;
            if (!check(!session_node_->io_ok() && session_node_->get_io_status().contains("Load failed") && session_node_->session()->capture_snapshot() == smoke_snapshot_, "GUI invalid load preserves active paused world")) return;
            smoke_io_serial_ = session_node_->io_serial();
            click_button(save_button_); // Restore valid test save for later inspection.
            ++smoke_phase_;
            break;
        case 18:
            if (session_node_->io_serial() == smoke_io_serial_) return;
            if (!check(session_node_->io_ok(), "Restore valid isolated smoke save")) return;
            click_button(return_button_);
            click_button(lab_button_);
            if(!check(session_node_->session()->published_world()->material_lab && session_node_->get_content_count()>=40,"Material laboratory loads full catalog"))return;
            click_button(pause_button_);
            observed_tick_=session_node_->get_tick();wait_frames_=0;
            smoke_phase_=180;
            break;
        case 180: {
            if(++wait_frames_<4)return;
            const auto* w=session_node_->session()->world();
            for(const auto& id:w->material_ids())if(id!="material.stone" && id!="material.water" && id!="material.oil")
                if(!check(w->material_count(w->material_index(id))==0,"Default liquid lab has no unrelated fixtures"))return;
            const auto* water=session_node_->session()->content().find_material("material.water");
            if(!check(water->source_liquid && water->liquid_gravity==1.5f && water->liquid_damping==0.8f && water->liquid_flow_speed==0.9f,"Godot converts raw source liquid parameters"))return;
            smoke_removed_=w->material_count(w->material_index("material.water"));
            click_canvas({140,35},MOUSE_BUTTON_LEFT);click_button(step_button_);smoke_phase_=181;break;
        }
        case 181: {
            if(session_node_->get_tick()==observed_tick_)return;
            const auto* w=session_node_->session()->world();
            if(!check(w->material_count(w->material_index("material.water"))==smoke_removed_+49 && !w->view().liquid_pressure_iterations && !w->view().liquid_substeps,"Default water brush advances source liquids without pressure"))return;
            // The remaining existing GUI regression explicitly loads its mixed
            // fixture; normal entry/reset above stays the liquid-only scene.
            const auto fixture=MaterialSimulation::make_lab(w->view().seed,session_node_->session()->content()).snapshot();
            const auto saved=persistence::save_world_file(session_node_->save_path(),fixture);if(!check(saved.ok,String::utf8(saved.error.c_str())))return;
            smoke_io_serial_=session_node_->io_serial();click_button(load_button_);smoke_phase_=182;break;
        }
        case 182: {
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Load isolated mixed-material regression fixture"))return;
            const auto sand=std::find(brush_ids_.begin(),brush_ids_.end(),"material.sand");
            material_picker_->select(static_cast<int>(sand-brush_ids_.begin()));material_picker_->emit_signal("item_selected",material_picker_->get_selected());
            observed_tick_=session_node_->get_tick();wait_frames_=0;smoke_phase_=19;break;
        }
        case 19:
            if(++wait_frames_<4)return;
            if(!check(session_node_->get_tick()==observed_tick_,"Paused laboratory stops all simulation"))return;
            if(!check(canvas_->get_size().is_equal_approx(world_->get_size()),"Canvas occupies the full world view"))return;
            {
                const auto size=canvas_->get_size();
                Ref<InputEventKey> key;key.instantiate();key->set_keycode(KEY_TAB);key->set_pressed(true);
                get_viewport()->push_input(key,true);
                if(!check(!inspector_->is_visible(),"Tab hides inspector"))return;
                key->set_pressed(false);get_viewport()->push_input(key,true);
                click_button(get_node<Button>("WorldView/Tabs/Data"));
                if(!check(inspector_->is_visible() && data_tab_ && content_->is_visible() && !material_picker_->is_visible_in_tree(),"Data tab opens statistics without tools"))return;
                click_button(get_node<Button>("WorldView/Tabs/Tools"));
                if(!check(!data_tab_ && material_picker_->is_visible_in_tree() && !content_->is_visible() && canvas_->get_size().is_equal_approx(size),"Tools tab preserves full canvas size"))return;
            }
            smoke_removed_=session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.sand"));
            smoke_texture_creations_=canvas_->texture_creations();
            smoke_texture_updates_=canvas_->texture_updates();
            click_canvas({0,35},MOUSE_BUTTON_LEFT); // Default sand brush, queued while paused.
            if(!check(session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.sand"))==smoke_removed_,"UI brush only queues commands"))return;
            click_button(step_button_);++smoke_phase_;break;
        case 20:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(session_node_->get_tick()==observed_tick_+1 && session_node_->get_session_state()=="Paused","Single step advances exactly once and remains paused"))return;
            if(!check(session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.sand"))==smoke_removed_+49,"GUI paint creates exact circular sand volume"))return;
            tool_picker_->select(2);tool_picker_->emit_signal("item_selected",2);
            click_canvas({65,67},MOUSE_BUTTON_LEFT);click_button(step_button_);observed_tick_=session_node_->get_tick();++smoke_phase_;break;
        case 21:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(session_node_->session()->world()->cell_state({65,67})->burning,"GUI heat tool ignites actual wood cells"))return;
            if(!check(canvas_->texture_creations()==smoke_texture_creations_ && canvas_->texture_updates()>smoke_texture_updates_,"Live material changes reuse chunk textures"))return;
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();
            click_button(save_button_);++smoke_phase_;break;
        case 22:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save burning multi-material world"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);++smoke_phase_;break;
        case 23:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Menu reload preserves all materials, heat, life and fuel"))return;
            wait_frames_=0;++smoke_phase_;break;
        case 24:
            if(++wait_frames_<12)return;
            tool_picker_->select(0);tool_picker_->emit_signal("item_selected",0);
            {
                const auto fire=std::find(brush_ids_.begin(),brush_ids_.end(),"material.fire");
                if(!check(fire!=brush_ids_.end(),"Fire brush registered"))return;
                const auto selected=static_cast<int>(fire-brush_ids_.begin());
                material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
            }
            click_canvas({100,40},MOUSE_BUTTON_LEFT);click_button(step_button_);observed_tick_=session_node_->get_tick();++smoke_phase_;break;
        case 25:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.fire"))>=49,"GUI fire brush uses native fire material"))return;
            click_button(pause_button_);observed_tick_=session_node_->get_tick();++smoke_phase_;break;
        case 26:
            if(session_node_->get_tick()<observed_tick_+95)return;
            click_button(pause_button_);smoke_io_serial_=session_node_->io_serial();click_button(reset_button_);++smoke_phase_;break;
        case 27:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->get_tick()==0,"Reset rebuilds authored lab at boundary"))return;
            if(!check(session_node_->get_session_state()=="Paused","Reset preserves pause state"))return;
            {
                const auto* w=session_node_->session()->world();
                if(!check(w->material_count(w->material_index("material.fire"))==0 && w->material_count(w->material_index("material.sand"))==0,"Reset restores liquid-only fixtures"))return;
                const auto fixture=MaterialSimulation::make_lab(w->view().seed,session_node_->session()->content()).snapshot();
                const auto saved=persistence::save_world_file(session_node_->save_path(),fixture);if(!check(saved.ok,String::utf8(saved.error.c_str())))return;
                smoke_io_serial_=session_node_->io_serial();click_button(load_button_);smoke_phase_=183;
            }
            break;
        case 183:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Load mixed regression after verifying liquid reset"))return;
            wait_frames_=0;smoke_phase_=28;break;
        case 28:
            if(++wait_frames_<12)return;
            smoke_steam_peak_=0;click_button(pause_button_);observed_tick_=session_node_->get_tick();smoke_phase_=32;break;
        case 32:
            smoke_steam_peak_=std::max(smoke_steam_peak_,session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.steam")));
            if(session_node_->get_tick()<observed_tick_+20)return;
            click_button(pause_button_);
            {
                const auto* w=session_node_->session()->world();int salt=0,steam=0;
                for(int y=88;y<123;++y)for(int x=50;x<76;++x){
                    const auto m=w->material_at({x,y});
                    if(m==w->material_index("material.salt"))++salt;
                    if(m==w->material_index("material.steam"))++steam;
                }
                if(!check(salt>0 && smoke_steam_peak_>0,"Actual imported brine produces salt and observable steam before condensation"))return;
                UtilityFunctions::print("[three-input GUI] local salt=",salt," steam=",steam," peak steam=",smoke_steam_peak_," tick=",session_node_->get_tick());
            }
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();
            click_button(save_button_);smoke_phase_=33;break;
        case 33:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save actual three-input reaction products"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=34;break;
        case 34:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Three-input products and motion survive GUI save/menu/load exactly"))return;
            smoke_removed_=session_node_->session()->published_world()->world_stone;
            tool_picker_->select(4);tool_picker_->emit_signal("item_selected",4);
            click_canvas({100,109},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=37;break;
        case 37:
            if(session_node_->get_tick()==observed_tick_)return;
            smoke_snapshot_=session_node_->session()->capture_snapshot();
            if(!check(session_node_->session()->published_world()->world_stone==smoke_removed_ &&
                std::any_of(smoke_snapshot_->states.begin(),smoke_snapshot_->states.end(),[](const auto& state){return state.mobilized;}),"GUI loosening mobilizes the actual rock slab without material loss"))return;
            smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=38;break;
        case 38:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save mobilized rock and live adhesion timers"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=39;break;
        case 39:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Mechanical phase, velocities and adhesion timers survive GUI save/menu/load"))return;
            click_button(pause_button_);observed_tick_=session_node_->get_tick();smoke_phase_=40;break;
        case 40:
            if(session_node_->get_tick()<observed_tick_+120)return;
            click_button(pause_button_);
            {
                const auto snap=session_node_->session()->capture_snapshot();const auto* w=session_node_->session()->world();
                if(!check(w->view().world_stone==smoke_removed_ && std::none_of(snap->states.begin(),snap->states.end(),[](const auto& state){return state.mobilized;}),"Imported rock settles back to a fixed phase with conserved stone"))return;
                int glue=0,slime=0;
                for(int x=130;x<134;++x)if(w->material_at({x,64})==w->material_index("material.glue"))++glue;
                for(int y=64;y<68;++y)for(int x=145;x<150;++x)if(w->material_at({x,y})==w->material_index("material.slime"))++slime;
                UtilityFunctions::print("[source adhesion GUI] original slots glue=",glue," slime=",slime,
                    " totals=",w->material_count(w->material_index("material.glue")),"/",w->material_count(w->material_index("material.slime")));
                if(!check(w->material_count(w->material_index("material.glue"))==4 && w->material_count(w->material_index("material.slime"))==20 &&
                    (glue<4 || slime<20),"Actual imported glue/slime use source release and transport instead of a permanent ceiling strand; material quantities are retained"))return;
                smoke_removed_=w->material_count(w->material_index("material.glue"));
            }
            tool_picker_->select(1);tool_picker_->emit_signal("item_selected",1);radius_picker_->select(1);radius_picker_->emit_signal("item_selected",1);
            click_canvas({131,61},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=41;break;
        case 41:
            if(session_node_->get_tick()==observed_tick_)return;
            click_button(pause_button_);observed_tick_=session_node_->get_tick();smoke_phase_=42;break;
        case 42:
            if(session_node_->get_tick()<observed_tick_+12)return;
            click_button(pause_button_);
            {
                const auto* w=session_node_->session()->world();
                if(!check(w->material_count(w->material_index("material.glue"))==smoke_removed_,"GUI terrain removal preserves actual released glue quantity"))return;
            }
            UtilityFunctions::print("[mechanics GUI] mobile rock save/load/settle; source glue/slime release; terrain removal conserved glue; tick=",session_node_->get_tick());
            tool_picker_->select(2);tool_picker_->emit_signal("item_selected",2);radius_picker_->select(0);radius_picker_->emit_signal("item_selected",0);
            click_canvas({17,28},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=43;break;
        case 43:
            if(session_node_->get_tick()==observed_tick_)return;
            smoke_snapshot_=session_node_->session()->capture_snapshot();
            if(!check(std::any_of(smoke_snapshot_->states.begin(),smoke_snapshot_->states.end(),[](const auto& state){return state.detonation_ticks>0;}),"GUI heat primes actual imported explosive powder"))return;
            smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=44;break;
        case 44:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save a live explosive fuse"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=45;break;
        case 45:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"GUI reload preserves a live fuse and complete shape"))return;
            smoke_removed_=session_node_->session()->published_world()->world_stone;
            observed_tick_=session_node_->get_tick();click_button(pause_button_);smoke_phase_=46;break;
        case 46:
            if(session_node_->get_tick()<observed_tick_+10)return;
            click_button(pause_button_);
            {
                const auto* w=session_node_->session()->world();const auto view=w->view();
                if(!check(w->material_count(w->material_index("material.explosive_powder"))==0 && view.world_stone<smoke_removed_ &&
                    view.world_stone+view.dropped_stone+view.held_stone==view.initial_stone && !view.held_stone && !view.dropped_stone,"Actual powder detonates after reload, excavates terrain and gives no duplicate inventory"))return;
                smoke_removed_=view.world_stone;
            }
            tool_picker_->select(6);tool_picker_->emit_signal("item_selected",6);
            click_canvas({32,20},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=47;break;
        case 47:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(session_node_->session()->published_world()->world_stone<smoke_removed_ && session_node_->get_action_status().contains("Explosion affected"),"GUI blast tool follows the native command path and removes real terrain"))return;
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=48;break;
        case 48:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save the resulting blast holes and moving fragments"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=49;break;
        case 49:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Post-explosion geometry, resources and fragments survive GUI reload exactly"))return;
            UtilityFunctions::print("[explosion GUI] imported powder heat/fuse/save/load/detonate; blast tool; terrain and fragments save/load; tick=",session_node_->get_tick());
            tool_picker_->select(7);tool_picker_->emit_signal("item_selected",7);
            click_canvas({48,14},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=50;break;
        case 50:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(session_node_->session()->published_world()->electric_energy>0 && session_node_->session()->world()->cell_state({64,14})->electrified_ticks>0,"Actual imported steel conducts a live pulse across x=64"))return;
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=51;break;
        case 51:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save in-flight electrical pulse"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=52;break;
        case 52:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Pulse energy, direction and afterglow survive GUI menu/reload"))return;
            observed_tick_=session_node_->get_tick();click_button(pause_button_);smoke_phase_=53;break;
        case 53:
            if(session_node_->get_tick()<observed_tick_+16)return;
            click_button(pause_button_);
            if(!check(!session_node_->session()->published_world()->electric_energy && !session_node_->session()->world()->cell_state({100,14})->electrified_ticks,"Finite source arc energy and recent afterglow expire; short gaps are not guaranteed insulation"))return;
            tool_picker_->select(1);tool_picker_->emit_signal("item_selected",1);click_canvas({82,14},MOUSE_BUTTON_LEFT);
            observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=54;break;
        case 54:
            if(session_node_->get_tick()==observed_tick_)return;
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.steel");if(!check(found!=brush_ids_.end(),"Steel brush exists"))return;
                const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
            }
            tool_picker_->select(0);tool_picker_->emit_signal("item_selected",0);click_canvas({82,14},MOUSE_BUTTON_LEFT);
            observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=55;break;
        case 55:
            if(session_node_->get_tick()==observed_tick_)return;
            tool_picker_->select(7);tool_picker_->emit_signal("item_selected",7);click_canvas({48,14},MOUSE_BUTTON_LEFT);
            observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=56;break;
        case 56:
            if(session_node_->get_tick()==observed_tick_)return;
            observed_tick_=session_node_->get_tick();click_button(pause_button_);smoke_phase_=57;break;
        case 57:
            if(session_node_->get_tick()<observed_tick_+3)return;
            click_button(pause_button_);
            if(!check(session_node_->session()->world()->cell_state({86,14})->electrified_ticks>0,"Replacing the insulating gap with real steel reconnects electricity"))return;
            UtilityFunctions::print("[electric GUI] charge/step; steel crossing; active pulse save/menu/load; oil insulation; actual brush repair reconnects; tick=",session_node_->get_tick());
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.lava");if(!check(found!=brush_ids_.end(),"Lava brush exists"))return;
                const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
            }
            radius_picker_->select(2);radius_picker_->emit_signal("item_selected",2);tool_picker_->select(0);tool_picker_->emit_signal("item_selected",0);
            click_canvas({143,18},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=58;break;
        case 58:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(session_node_->get_action_status().contains("changed 29 cells"),"GUI paints actual lava into the empty reaction trough"))return;
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.salt_water");if(!check(found!=brush_ids_.end(),"Salt-water brush exists"))return;
                const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
            }
            click_canvas({148,18},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=59;break;
        case 59:
            if(session_node_->get_tick()==observed_tick_)return;
            {
                const auto* w=session_node_->session()->world();
                if(!check(session_node_->get_content_count()>=43 && w->material_count(w->material_index("material.glowrock"))>=3 &&
                    w->material_count(w->material_index("material.steam"))>=3,"Actual salt-water/lava source recipe creates regional glowing rock and steam in one fixed step"))return;
                UtilityFunctions::print("[blob GUI] source salt-water/lava radius 6/3; glowing rock=",w->material_count(w->material_index("material.glowrock")),"; tick=",session_node_->get_tick());
            }
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=60;break;
        case 60:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save real regional conversion and steam motion"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=61;break;
        case 61:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Regional outputs, exact shape, resource counts, seed and tick survive GUI menu/reload"))return;
            observed_tick_=session_node_->get_tick();click_button(pause_button_);smoke_phase_=62;break;
        case 62:
            if(session_node_->get_tick()<observed_tick_+4)return;
            click_button(pause_button_);
            if(!check(session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.glowrock"))>=3,"Regional solid outputs persist while remaining materials resume"))return;
            UtilityFunctions::print("[blob GUI] exact regional save/menu/load/resume; tick=",session_node_->get_tick());
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.copper");if(!check(found!=brush_ids_.end(),"Copper brush exists"))return;
                const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
            }
            radius_picker_->select(1);radius_picker_->emit_signal("item_selected",1);click_canvas({90,54},MOUSE_BUTTON_LEFT);
            observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=63;break;
        case 63:
            if(session_node_->get_tick()==observed_tick_)return;
            smoke_removed_=session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.copper"));
            if(!check(smoke_removed_==13,"GUI paints thirteen real copper units in the separate crucible"))return;
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.lava");if(!check(found!=brush_ids_.end(),"Lava remains available"))return;
                const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
            }
            click_canvas({93,56},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=64;break;
        case 64:
            if(session_node_->get_tick()==observed_tick_)return;
            {
                const auto* w=session_node_->session()->world();
                const auto liquid=w->material_index("material.copper_molten");
                bool high_fuel=false;for(int y=38;y<58;++y)for(int x=82;x<110;++x)if(w->material_at({x,y})==liquid && w->cell_state({x,y})->fuel>65535)high_fuel=true;
                if(!check(w->material_count(liquid)>0 && high_fuel && w->material_count(liquid)+w->material_count(w->material_index("material.copper"))==smoke_removed_,"Actual lava creates copper-specific molten liquid with wide fuel and conserved metal units"))return;
                UtilityFunctions::print("[phase GUI] copper + lava -> molten copper; solid=",w->material_count(w->material_index("material.copper"))," molten=",w->material_count(liquid),"; tick=",session_node_->get_tick());
            }
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=65;break;
        case 65:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save live molten copper with fuel above uint16"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=66;break;
        case 66:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Version 7 restores exact molten shape, burning, wide fuel, seed and tick via GUI"))return;
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.copper_molten");if(!check(found!=brush_ids_.end(),"Molten-copper brush exists"))return;
                const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
                radius_picker_->select(0);radius_picker_->emit_signal("item_selected",0);click_canvas({119,84},MOUSE_BUTTON_LEFT);
            }
            observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=67;break;
        case 67:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(session_node_->get_action_status().contains("changed 5 cells"),"Separate cold crucible receives five explicitly sourced molten units"))return;
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.water");if(!check(found!=brush_ids_.end(),"Ordinary quench-water brush exists"))return;
                const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
                radius_picker_->select(2);radius_picker_->emit_signal("item_selected",2);click_canvas({119,84},MOUSE_BUTTON_LEFT);
            }
            observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=69;break;
        case 69:
            if(session_node_->get_tick()==observed_tick_)return;
            observed_tick_=session_node_->get_tick();click_button(pause_button_);smoke_phase_=68;break;
        case 68:
            {
                const auto* w=session_node_->session()->world();
                if(!check(w->material_count(w->material_index("material.copper"))+w->material_count(w->material_index("material.copper_molten"))==smoke_removed_+5,"Copper count includes only the original thirteen plus five explicitly painted units at every quench step"))return;
            }
            if(session_node_->get_tick()<observed_tick_+12)return;
            click_button(pause_button_);
            {
                const auto* w=session_node_->session()->world();std::uint64_t solid=0,liquid=0;
                for(int y=72;y<88;++y)for(int x=116;x<125;++x){if(w->material_at({x,y})==w->material_index("material.copper"))++solid;else if(w->material_at({x,y})==w->material_index("material.copper_molten"))++liquid;}
                UtilityFunctions::print("[phase quench] cold crucible solid=",solid," molten=",liquid,"; global copper=",smoke_removed_+5);
                if(!check(solid>0 && solid+liquid==5,"Ordinary water in the separate cold crucible solidifies real molten copper and retains all five units"))return;
                UtilityFunctions::print("[phase GUI] exact live save/menu/load; separate ordinary-water quench; copper units=",smoke_removed_+5,"; tick=",session_node_->get_tick());
            }
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.wood_prop");if(!check(found!=brush_ids_.end(),"Original rigid wood definition exists"))return;
                const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
            }
            tool_picker_->select(8);tool_picker_->emit_signal("item_selected",8);radius_picker_->select(2);radius_picker_->emit_signal("item_selected",2);
            click_canvas({58,38},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=70;break;
        case 70:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(session_node_->session()->published_world()->rigid_bodies==1 && session_node_->session()->published_world()->rigid_pixels==29 &&
                session_node_->session()->world()->bodies()[0].y>38.5,"Actual rigid-wood resource creates a falling 29-pixel body through the native solver"))return;
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=71;break;
        case 71:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save flying pixel rigid body"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=72;break;
        case 72:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Rigid pixels, pose, velocity and unique ID survive GUI menu/reload exactly"))return;
            observed_tick_=session_node_->get_tick();click_button(pause_button_);smoke_phase_=73;break;
        case 73:
            if(session_node_->get_tick()<observed_tick_+90)return;
            click_button(pause_button_);
            {
                const auto& b=session_node_->session()->world()->bodies()[0];
                if(!check(b.y>50 && b.y<56 && std::abs(b.vy)<2,"Loaded body lands on the actual cross-chunk platform"))return;
                tool_picker_->select(1);tool_picker_->emit_signal("item_selected",1);radius_picker_->select(0);radius_picker_->emit_signal("item_selected",0);
                smoke_texture_creations_=canvas_->texture_creations();click_canvas({static_cast<int>(std::floor(b.x)),static_cast<int>(std::floor(b.y))},MOUSE_BUTTON_LEFT);
            }
            observed_tick_=session_node_->get_tick();click_button(step_button_);wait_frames_=0;smoke_phase_=74;break;
        case 74:
            if(session_node_->get_tick()==observed_tick_ || ++wait_frames_<3)return;
            if(!check(session_node_->session()->published_world()->rigid_bodies==1 && session_node_->session()->published_world()->rigid_pixels>0 && session_node_->session()->published_world()->rigid_pixels<29 && canvas_->texture_creations()==smoke_texture_creations_,"GUI erases a real local hole, rebuilds collision geometry and reuses the body texture"))return;
            smoke_removed_=session_node_->session()->published_world()->rigid_pixels;smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=75;break;
        case 75:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save the actual rigid hole"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=76;break;
        case 76:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Rigid holes and remaining material survive the second GUI save/menu/load"))return;
            {
                const auto& b=session_node_->session()->world()->bodies()[0];tool_picker_->select(5);tool_picker_->emit_signal("item_selected",5);radius_picker_->select(1);radius_picker_->emit_signal("item_selected",1);
                click_canvas({static_cast<int>(std::floor(b.x)),static_cast<int>(std::floor(b.y))},MOUSE_BUTTON_LEFT);
            }
            observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=77;break;
        case 77:
            if(session_node_->get_tick()==observed_tick_)return;
            smoke_texture_creations_=canvas_->texture_creations();observed_tick_=session_node_->get_tick();click_button(pause_button_);smoke_phase_=78;break;
        case 78:
            if(session_node_->get_tick()<observed_tick_+12)return;
            click_button(pause_button_);
            {
                const auto& b=session_node_->session()->world()->bodies()[0];const auto& old=smoke_snapshot_->bodies[0];
                if(!check(b.x>old.x+3 && std::abs(b.angle-old.angle)>0.05 && session_node_->session()->published_world()->rigid_pixels==smoke_removed_ && canvas_->texture_creations()==smoke_texture_creations_,"GUI push produces real rigid translation/rotation without pixel loss or per-frame textures"))return;
                UtilityFunctions::print("[rigid GUI] source wood_prop; flying save/menu/load; collision landing; erase/reused texture; hole save/menu/load; push/rotation; pixels=",b.pixels.size(),"; tick=",session_node_->get_tick());
                tool_picker_->select(2);tool_picker_->emit_signal("item_selected",2);radius_picker_->select(7);radius_picker_->emit_signal("item_selected",7);
                click_canvas({static_cast<int>(std::floor(b.x)),static_cast<int>(std::floor(b.y))},MOUSE_BUTTON_LEFT);
            }
            observed_tick_=session_node_->get_tick();click_button(step_button_);wait_frames_=0;smoke_phase_=80;break;
        case 80:
            if(session_node_->get_tick()==observed_tick_ || ++wait_frames_<3)return;
            {
                const auto& b=session_node_->session()->world()->bodies()[0];
                if(!check(std::any_of(b.pixels.begin(),b.pixels.end(),[](const auto& p){return p.state.burning && p.state.fuel==420 && p.state.burning_ticks==1 && p.state.fire_intensity>0;}) &&
                    canvas_->texture_creations()==smoke_texture_creations_,"GUI heat ignites source rigid wood, advances burn counter without a fictitious fuel decrement, and reuses texture"))return;
                smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=81;break;
            }
        case 81:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save actively burning original rigid wood"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=82;break;
        case 82:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Burning body temperature, per-pixel fuel and shape reload exactly through the GUI"))return;
            {
                const auto& b=session_node_->session()->world()->bodies()[0];tool_picker_->select(3);tool_picker_->emit_signal("item_selected",3);
                click_canvas({static_cast<int>(std::floor(b.x)),static_cast<int>(std::floor(b.y))},MOUSE_BUTTON_LEFT);
            }
            observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=83;break;
        case 83:
            if(session_node_->get_tick()==observed_tick_)return;
            {
                const auto& pixels=session_node_->session()->world()->bodies()[0].pixels;
                const auto& prior=smoke_snapshot_->bodies[0].pixels;
                if(!check(std::all_of(pixels.begin(),pixels.end(),[&](const auto& p){
                    const auto old=std::find_if(prior.begin(),prior.end(),[&](const auto& q){return p.local==q.local;});
                    return old!=prior.end() && p.state.temperature<50 && p.state.burning_ticks==old->state.burning_ticks && !p.state.ignition_pending;
                }),"GUI cooling clears the active burn update and cools every pixel; nearby fire may ignite it again"))return;
                UtilityFunctions::print("[combustion GUI] cooled pixels=",pixels.size()," preserved burn counters; relit=",std::count_if(pixels.begin(),pixels.end(),[](const auto& p){return p.state.burning;}));
            }
            tool_picker_->select(8);tool_picker_->emit_signal("item_selected",8);radius_picker_->select(0);radius_picker_->emit_signal("item_selected",0);
            click_canvas({136,12},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=84;break;
        case 84:
            if(session_node_->get_tick()==observed_tick_)return;
            {
                const auto& b=session_node_->session()->world()->bodies().back();smoke_body_id_=b.id;
                if(!check(b.pixels.size()==5,"Create a separate five-pixel rigid target in the reaction trough"))return;
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.acid");const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
                smoke_acid_gas_=session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.acid_gas"));
            }
            tool_picker_->select(0);tool_picker_->emit_signal("item_selected",0);radius_picker_->select(2);radius_picker_->emit_signal("item_selected",2);
            click_canvas({136,10},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=85;break;
        case 85:
            if(session_node_->get_tick()==observed_tick_)return;
            observed_tick_=session_node_->get_tick();click_button(pause_button_);smoke_phase_=86;break;
        case 86:
            if(session_node_->get_tick()<observed_tick_+20)return;
            click_button(pause_button_);
            {
                const auto* w=session_node_->session()->world();const auto found=std::find_if(w->bodies().begin(),w->bodies().end(),[&](const auto& b){return b.id==smoke_body_id_;});
                if(!check((found==w->bodies().end() || found->pixels.size()<5) && w->material_count(w->material_index("material.acid_gas"))>smoke_acid_gas_,"Original acid resource corrodes the actual body and releases acid gas into the grid"))return;
                UtilityFunctions::print("[rigid interaction GUI] source ignition/intensity/body counter/reused texture; burning save/menu/load; cooling; acid/body corrosion and gas; tick=",session_node_->get_tick());
            }
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.wood_prop");const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
                tool_picker_->select(8);tool_picker_->emit_signal("item_selected",8);radius_picker_->select(2);radius_picker_->emit_signal("item_selected",2);
                click_canvas({58,38},MOUSE_BUTTON_LEFT);
            }
            observed_tick_=session_node_->get_tick();click_button(step_button_);wait_frames_=0;smoke_phase_=87;break;
        case 87:
            if(session_node_->get_tick()==observed_tick_ || ++wait_frames_<3)return;
            smoke_blast_target_=session_node_->session()->world()->bodies().back().id;
            if(!check(session_node_->session()->world()->bodies().back().pixels.size()==29,"Create actual rigid wood for local blast damage"))return;
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.tnt");if(!check(found!=brush_ids_.end(),"Imported original solid TNT is available"))return;
                const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
            }
            tool_picker_->select(6);tool_picker_->emit_signal("item_selected",6);radius_picker_->select(0);radius_picker_->emit_signal("item_selected",0);
            smoke_texture_creations_=canvas_->texture_creations();click_canvas({54,38},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);wait_frames_=0;smoke_phase_=88;break;
        case 88:
            if(session_node_->get_tick()==observed_tick_ || ++wait_frames_<3)return;
            {
                const auto* w=session_node_->session()->world();const auto body=std::find_if(w->bodies().begin(),w->bodies().end(),[&](const auto& b){return b.id==smoke_blast_target_;});
                if(!check(body!=w->bodies().end() && body->pixels.size()>0 && body->pixels.size()<29 && body->vx>0 && w->view().explosions==1 && canvas_->texture_creations()==smoke_texture_creations_,"Real TNT blast profile cuts and propels wood while reusing its body texture"))return;
            }
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=89;break;
        case 89:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save actual blast-damaged body"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=90;break;
        case 90:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Blast shape, impulse and all material quantities reload exactly"))return;
            tool_picker_->select(8);tool_picker_->emit_signal("item_selected",8);radius_picker_->select(0);radius_picker_->emit_signal("item_selected",0);
            click_canvas({58,46},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=91;break;
        case 91:
            if(session_node_->get_tick()==observed_tick_)return;
            smoke_body_id_=session_node_->session()->world()->bodies().back().id;
            if(!check(session_node_->session()->world()->bodies().back().pixels.size()==5 && session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.tnt"))==5,"Create five actual original TNT pixels as a dynamic body"))return;
            tool_picker_->select(2);tool_picker_->emit_signal("item_selected",2);radius_picker_->select(2);radius_picker_->emit_signal("item_selected",2);
            click_canvas({58,46},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=92;break;
        case 92:
            if(session_node_->get_tick()==observed_tick_)return;
            {
                const auto& body=session_node_->session()->world()->bodies().back();
                if(!check(std::any_of(body.pixels.begin(),body.pixels.end(),[](const auto& p){return p.state.detonation_ticks==4 && p.state.fuel==600;}),"Original TNT heat starts a saved four-step body fuse without consuming pixels early"))return;
            }
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=93;break;
        case 93:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save armed original TNT body"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=94;break;
        case 94:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"GUI restores armed body-local TNT fuses, fuel, pose and shape exactly"))return;
            wait_frames_=0;observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=95;break;
        case 95:
            if(session_node_->get_tick()==observed_tick_)return;
            if(++wait_frames_<4){
                if(!check(session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.tnt"))==5,"Armed TNT survives until its configured subsequent fixed step"))return;
                observed_tick_=session_node_->get_tick();click_button(step_button_);break;
            }
            if(!check(session_node_->session()->world()->material_count(session_node_->session()->world()->material_index("material.tnt"))==0 && session_node_->session()->world()->view().explosions==1 &&
                std::none_of(session_node_->session()->world()->bodies().begin(),session_node_->session()->world()->bodies().end(),[&](const auto& b){return b.id==smoke_body_id_;}),"One connected five-pixel TNT body detonates once after GUI reload and four actual physics steps"))return;
            UtilityFunctions::print("[rigid blast GUI] actual source tnt; local wood damage/impulse/reused texture; exact damaged save; armed TNT save/menu/load; four-step single detonation; tick=",session_node_->get_tick());
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=96;break;
        case 96:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save body detonation products and survivors"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=97;break;
        case 97:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"GUI reload preserves exact detonation products, surviving bodies and material ledger"))return;
            {
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.steel");const auto selected=static_cast<int>(found-brush_ids_.begin());material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
                tool_picker_->select(9);tool_picker_->emit_signal("item_selected",9);radius_picker_->select(7);radius_picker_->emit_signal("item_selected",7);click_canvas({82,14},MOUSE_BUTTON_LEFT);
            }
            observed_tick_=session_node_->get_tick();click_button(step_button_);wait_frames_=0;smoke_phase_=98;break;
        case 98:
            if(session_node_->get_tick()==observed_tick_ || ++wait_frames_<3)return;
            {
                const auto& body=session_node_->session()->world()->bodies().back();smoke_body_id_=body.id;
                const auto steel=static_cast<std::uint16_t>(session_node_->session()->world()->material_index("material.steel"));
                if(!check(std::count_if(body.pixels.begin(),body.pixels.end(),[&](const auto& p){return p.material==steel;})>10,"Detach actual imported steel wire and support into one physical conductor"))return;
            }
            smoke_snapshot_=session_node_->session()->capture_snapshot();
            tool_picker_->select(5);tool_picker_->emit_signal("item_selected",5);radius_picker_->select(1);radius_picker_->emit_signal("item_selected",1);click_canvas({82,14},MOUSE_BUTTON_LEFT);
            tool_picker_->select(7);tool_picker_->emit_signal("item_selected",7);radius_picker_->select(0);radius_picker_->emit_signal("item_selected",0);click_canvas({48,14},MOUSE_BUTTON_LEFT);
            smoke_texture_creations_=canvas_->texture_creations();observed_tick_=session_node_->get_tick();click_button(step_button_);wait_frames_=0;smoke_phase_=99;break;
        case 99:
            if(session_node_->get_tick()==observed_tick_ || ++wait_frames_<3)return;
            {
                const auto snapshot=session_node_->session()->capture_snapshot();const auto body=std::find_if(snapshot->bodies.begin(),snapshot->bodies.end(),[&](const auto& b){return b.id==smoke_body_id_;});
                const auto& before=smoke_snapshot_->bodies.back();
                if(body!=snapshot->bodies.end())UtilityFunctions::print("[rigid electric] body=",body->id," pose=",body->x,",",body->y," angle=",body->angle," glow=",std::count_if(body->pixels.begin(),body->pixels.end(),[](const auto& p){return p.state.electrified_ticks>0;})," textures=",canvas_->texture_creations()," expected=",smoke_texture_creations_);
                for(const auto& p:snapshot->electric_pulses)UtilityFunctions::print("[rigid electric] head owner=",p.body," local/grid=",p.position.x,",",p.position.y," energy=",p.energy);
                if(!check(body!=snapshot->bodies.end() && std::any_of(snapshot->electric_pulses.begin(),snapshot->electric_pulses.end(),[&](const auto& p){return p.body==smoke_body_id_ && p.energy>0;}) &&
                    std::any_of(body->pixels.begin(),body->pixels.end(),[](const auto& p){return p.state.electrified_ticks>0;}) && (std::abs(body->y-before.y)>0.01 || std::abs(body->angle-before.angle)>0.005) && canvas_->texture_creations()==smoke_texture_creations_,"GUI charge crosses into a moving physical steel bridge; its pulse stays body-local and its blue texture is reused"))return;
            }
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=100;break;
        case 100:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save actual moving body-local pulse"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);wait_frames_=0;smoke_phase_=101;break;
        case 101:
            if(session_node_->io_serial()==smoke_io_serial_ || ++wait_frames_<3)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"Format 9 GUI reload restores exact body-local energy, previous owner, afterglow and pose"))return;
            tool_picker_->select(1);tool_picker_->emit_signal("item_selected",1);radius_picker_->select(0);radius_picker_->emit_signal("item_selected",0);click_canvas({91,14},MOUSE_BUTTON_LEFT);
            smoke_texture_creations_=canvas_->texture_creations();observed_tick_=session_node_->get_tick();click_button(step_button_);wait_frames_=0;smoke_phase_=102;break;
        case 102:
            if(session_node_->get_tick()==observed_tick_ || ++wait_frames_<3)return;
            UtilityFunctions::print("[source arc gap] energy=",session_node_->session()->published_world()->electric_energy," remote glow=",session_node_->session()->world()->cell_state({100,14})->electrified_ticks," remote stamp=",session_node_->session()->world()->cell_state({100,14})->electric_stamp," tick=",session_node_->get_tick());
            if(!check(session_node_->session()->published_world()->electric_energy<smoke_snapshot_->electric_pulses.front().energy && session_node_->session()->world()->material_at({100,14})==session_node_->session()->world()->material_index("material.steel") && session_node_->session()->world()->cell_state({100,14})->electrified_ticks>0 &&
                canvas_->texture_creations()==smoke_texture_creations_,"Source nearby search crosses the short erased contact and charges intact remote steel while spending energy"))return;
            wait_frames_=0;observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=103;break;
        case 103:
            if(session_node_->get_tick()==observed_tick_)return;
            if(++wait_frames_<12){observed_tick_=session_node_->get_tick();click_button(step_button_);break;}
            if(!check(!session_node_->session()->published_world()->electrified_cells && !session_node_->session()->published_world()->electric_energy && canvas_->texture_creations()==smoke_texture_creations_,"Body and grid afterglow expire without stale counters or repeated texture allocation"))return;
            UtilityFunctions::print("[rigid electric GUI] original steel detach; moving bridge charge; body-local pulse/pose/glow exact save/menu/load; actual short contact cut/source arc crossing; finite energy and afterglow; tick=",session_node_->get_tick());
            // The preceding chemistry/ignition sequence now leaves burning oil
            // in the left trough. Exercise conservation in a fresh cold lab,
            // using the actual reset button rather than suppressing reactions.
            smoke_io_serial_=session_node_->io_serial();click_button(reset_button_);smoke_phase_=108;break;
        case 108:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->get_tick()==0 && session_node_->session()->world()->bodies().empty(),"Actual GUI resets a cold liquid-entry fixture after the chemistry sequence"))return;
            {
                const auto* w=session_node_->session()->world();smoke_basin_water_=0;smoke_fluid_displaced_=0;
                smoke_basin_water_=w->material_count(w->material_index("material.water"));
                const auto found=std::find(brush_ids_.begin(),brush_ids_.end(),"material.wood_prop");
                if(!check(found!=brush_ids_.end(),"Original rigid wood definition exists for actual liquid entry"))return;
                const auto selected=static_cast<int>(found-brush_ids_.begin());
                material_picker_->select(selected);material_picker_->emit_signal("item_selected",selected);
                tool_picker_->select(8);tool_picker_->emit_signal("item_selected",8);radius_picker_->select(2);radius_picker_->emit_signal("item_selected",2);click_canvas({-30,54},MOUSE_BUTTON_LEFT);
                observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=104;break;
            }
        case 104:
            if(session_node_->get_tick()==observed_tick_)return;
            {
                const auto& bodies=session_node_->session()->world()->bodies();
                const auto found=std::find_if(bodies.begin(),bodies.end(),[](const auto& b){return b.x<-20 && b.x>-40 && b.y<68;});
                if(!check(found!=bodies.end() && found->pixels.size()==29,"Actual GUI creates a body above the water trough"))return;
                smoke_body_id_=found->id;
            }
            wait_frames_=0;observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=105;break;
        case 105:
            if(session_node_->get_tick()==observed_tick_)return;
            smoke_fluid_displaced_+=session_node_->session()->published_world()->fluid_displaced;
            if(++wait_frames_<90){observed_tick_=session_node_->get_tick();click_button(step_button_);break;}
            {
                const auto* w=session_node_->session()->world();std::uint64_t water=0;
                water=w->material_count(w->material_index("material.water"));
                const auto found=std::find_if(w->bodies().begin(),w->bodies().end(),[&](const auto& b){return b.id==smoke_body_id_;});
                if(!check(found!=w->bodies().end() && found->y>68 && found->pixels.size()==29 && smoke_fluid_displaced_>0 && water==smoke_basin_water_,"Actual body enters liquid, displaces material and conserves the entire liquid-lab water quantity"))return;
                for(int y=32;y<126;++y)for(int x=-60;x<15;++x)if(const auto owner=w->body_pixel_at({x,y});owner && w->bodies()[owner->body].id==smoke_body_id_)
                    if(!check(w->material_at({x,y})!=w->material_index("material.water"),"Actual moving-body raster contains no hidden water"))return;
                UtilityFunctions::print("[rigid fluid GUI] body=",smoke_body_id_," y=",found->y," vy=",found->vy," water=",water," displaced=",smoke_fluid_displaced_);
            }
            smoke_snapshot_=session_node_->session()->capture_snapshot();smoke_io_serial_=session_node_->io_serial();click_button(save_button_);smoke_phase_=106;break;
        case 106:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok(),"Save actual immersed body and displaced water"))return;
            click_button(return_button_);smoke_io_serial_=session_node_->io_serial();click_button(menu_load_button_);smoke_phase_=107;wait_frames_=0;break;
        case 107:
            if(session_node_->io_serial()==smoke_io_serial_ || ++wait_frames_<3)return;
            if(!check(session_node_->io_ok() && session_node_->session()->capture_snapshot()==smoke_snapshot_,"GUI menu/reload preserves exact displaced water, body pose and quantities"))return;
            UtilityFunctions::print("[rigid fluid GUI] exact immersed-body/material save/menu/load; reused chunk textures; tick=",session_node_->get_tick());
            if(OS::get_singleton()->get_cmdline_user_args().has("--material-soak-test")){
                smoke_io_serial_=session_node_->io_serial();click_button(reset_button_);smoke_phase_=35;break;
            }
            wait_frames_=0;smoke_phase_=36;break;
        case 35:
            if(session_node_->io_serial()==smoke_io_serial_)return;
            if(!check(session_node_->io_ok() && session_node_->get_tick()==0 && session_node_->session()->published_world()->rigid_bodies==0,"Reset releases rigid bodies before full material settling check"))return;
            click_button(pause_button_);observed_tick_=session_node_->get_tick();smoke_phase_=29;break;
        case 36:
            if(++wait_frames_<12)return;
            tool_picker_->select(13);tool_picker_->emit_signal("item_selected",13);click_canvas({140,12},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=200;break;
        case 200:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(session_node_->session()->published_world()->freeze_chance==5 && session_node_->get_action_status().contains("Environment"),"Actual environment UI tool commits cold byte at fixed-step boundary"))return;
            tool_picker_->select(12);tool_picker_->emit_signal("item_selected",12);click_canvas({140,12},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=201;break;
        case 201:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(!session_node_->session()->published_world()->freeze_chance && !session_node_->session()->published_world()->unfreeze_chance,"Actual normal-environment UI clears both phase gates"))return;
            tool_picker_->select(10);tool_picker_->emit_signal("item_selected",10);click_canvas({140,12},MOUSE_BUTTON_LEFT);observed_tick_=session_node_->get_tick();click_button(step_button_);smoke_phase_=202;break;
        case 202:
            if(session_node_->get_tick()==observed_tick_)return;
            if(!check(session_node_->get_action_status().contains("Source temperature T=10"),"Actual source warm UI uses native random-threshold command"))return;
            smoke_finished_=true;
            UtilityFunctions::print("SMOKE PASS: lifecycle, wall conservation, invalid-load atomicity, reused textures; material tools and exact saves; Resource conversion; reactions, mechanical/adhesion, explosions/fuses; electrical pulse/steel/insulation/real repair; regional lava/salt-water/glowing rock; molten copper and quench; real pixel rigid bodies/landing/local holes/rotation/exact saves; rigid burning/fuel/cooling/acid/gas; rigid blast damage/pressure; actual TNT body/fuse/detonation/exact saves; moving steel body/electricity/insulation/format 9/afterglow; actual rigid/liquid displacement/conservation/exact save");
            get_tree()->quit(0);break;
        case 29:
            if(session_node_->get_tick()<600)return;
            if(session_node_->session()->published_world()->moved_cells!=0 || session_node_->session()->published_world()->active_chunks!=0){
                if(session_node_->get_tick()<3000){
                    if(session_node_->get_tick()%200==0)UtilityFunctions::print("[settling] tick=",session_node_->get_tick()," moved=",session_node_->session()->published_world()->moved_cells);
                    return;
                }
                // Improved liquid flow changes how the authored fire reaches
                // fuel. Do not mistake finite post-burn smoke for liquid jitter.
                // Retain the 75-second total limit and the 200-tick exact-state
                // gate; only a non-burning finite-effect tail gets extra ticks.
                if(session_node_->get_tick()<3600){
                    const auto* w=session_node_->session()->world();bool burning=false,finite_effect=false;
                    for(const auto& chunk:w->chunks())for(std::size_t i=0;i<chunk.cells.size();++i){
                        if(chunk.cells[i]==Material::air)continue;
                        const auto& state=chunk.states[i];burning=burning || state.burning;
                        if(state.life && !finite_effect){
                            const auto* d=session_node_->session()->content().find_material(w->material_ids()[static_cast<std::uint16_t>(chunk.cells[i])-1]);
                            finite_effect=d && (d->motion()==Motion::gas || d->motion()==Motion::fire);
                        }
                    }
                    if(!burning && finite_effect){
                        if(session_node_->get_tick()%200==0)UtilityFunctions::print("[settling finite-effect tail] tick=",session_node_->get_tick());
                        return;
                    }
                }
                smoke_snapshot_=session_node_->session()->capture_snapshot();observed_tick_=session_node_->get_tick();smoke_phase_=31;break;
            }
            if(!check(session_node_->session()->published_world()->moved_cells==0,"Full original-parameter scene settles after chemistry"))return;
            {
                const auto* w=session_node_->session()->world();
                if(!check(w->material_count(w->material_index("material.salt_water"))>0 &&
                    w->material_count(w->material_index("material.sludge"))==0 &&
                    w->material_count(w->material_index("material.concrete"))>0,"Actual imported salt/purification/cement reactions execute"))return;
            }
            smoke_snapshot_=session_node_->session()->capture_snapshot();observed_tick_=session_node_->get_tick();++smoke_phase_;break;
        case 30:
            if(session_node_->session()->published_world()->moved_cells!=0 || session_node_->session()->published_world()->active_chunks!=0){smoke_phase_=29;return;}
            if(session_node_->get_tick()<observed_tick_+200)return;
            click_button(pause_button_);
            {
                auto actual=*session_node_->session()->capture_snapshot();actual.tick=smoke_snapshot_->tick;
                if(!check(actual==smoke_snapshot_ && session_node_->session()->published_world()->active_chunks==0,"Settled native state remains identical for 200 more logic ticks and sleeps"))return;
            }
            UtilityFunctions::print("MATERIAL SOAK PASS: tick=",session_node_->get_tick(),"; 200 identical steady logic ticks; salt/purification/cement; materials=",session_node_->get_content_count());
            smoke_finished_=true;get_tree()->quit(0);break;
        case 31:
            if(session_node_->get_tick()<=observed_tick_)return;
            {
                const auto actual=*session_node_->session()->capture_snapshot();int reported=0;
                auto steady=actual;steady.tick=smoke_snapshot_->tick;
                const auto& chunks=session_node_->session()->world()->chunks();
                const bool pending=std::any_of(chunks.begin(),chunks.end(),[](const auto& chunk){return chunk.awake;});
                if(steady==*smoke_snapshot_ && !session_node_->session()->published_world()->moved_cells && !pending){
                    // Expiring the last effect wakes its chunk once. Input
                    // activity can outlive the final physical change by a tick.
                    // Return through the normal chemistry + 200-tick gate.
                    UtilityFunctions::print("[settling] final activity mask cleared at tick=",session_node_->get_tick());
                    smoke_phase_=29;break;
                }
                UtilityFunctions::print("[motion diagnostic] tick=",actual.tick," moves=",session_node_->session()->published_world()->moved_cells);
                for(int y=68;y<76;++y){
                    String row="[motion neighborhood] y="+String::num_int64(y)+" ";
                    for(int x=-25;x<-8;++x){
                        const auto index=static_cast<std::size_t>(y-actual.bounds.origin.y)*actual.bounds.width+static_cast<std::size_t>(x-actual.bounds.origin.x);
                        row+=String::num_int64(actual.cells[index])+",";
                    }
                    UtilityFunctions::print(row);
                }
                for(std::size_t i=0;i<actual.material_ids.size();++i)UtilityFunctions::print("[motion mapping] ",i+1," ",String::utf8(actual.material_ids[i].c_str()));
                for(std::size_t i=0;i<actual.cells.size() && reported<40;++i)if(actual.cells[i]!=smoke_snapshot_->cells[i]){
                    const auto old=smoke_snapshot_->cells[i],now=actual.cells[i];
                    const auto& state=actual.states[i];
                    UtilityFunctions::print("[motion cell] ",actual.bounds.origin.x+static_cast<int>(i%actual.bounds.width),",",actual.bounds.origin.y+static_cast<int>(i/actual.bounds.width),
                        " ",String::utf8(old?actual.material_ids[old-1].c_str():"air")," -> ",String::utf8(now?actual.material_ids[now-1].c_str():"air"),
                        " vx=",state.velocity_x," vy=",state.velocity_y," fraction=",state.vertical_fraction);++reported;
                }
            }
            static_cast<void>(check(false,"Full original-parameter scene settles after chemistry"));break;

    }
}
} // namespace pixel::host

