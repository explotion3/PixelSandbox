#include "godot_host/sandbox_app.hpp"
#include "godot_host/material_definition_resource.hpp"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace pixel::host {
using namespace godot;

void SandboxApp::_bind_methods() {
    ClassDB::bind_method(D_METHOD("on_lab_world"), &SandboxApp::on_lab_world);
    ClassDB::bind_method(D_METHOD("on_step"), &SandboxApp::on_step);
    ClassDB::bind_method(D_METHOD("on_reset"), &SandboxApp::on_reset);
    ClassDB::bind_method(D_METHOD("on_brush_changed","index"), &SandboxApp::on_brush_changed);
    ClassDB::bind_method(D_METHOD("on_new_world"), &SandboxApp::on_new_world);
    ClassDB::bind_method(D_METHOD("on_wall_world"), &SandboxApp::on_wall_world);
    ClassDB::bind_method(D_METHOD("on_pickup"), &SandboxApp::on_pickup);
    ClassDB::bind_method(D_METHOD("on_save"), &SandboxApp::on_save);
    ClassDB::bind_method(D_METHOD("on_load"), &SandboxApp::on_load);
    ClassDB::bind_method(D_METHOD("on_pause"), &SandboxApp::on_pause);
    ClassDB::bind_method(D_METHOD("on_return"), &SandboxApp::on_return);
    ClassDB::bind_method(D_METHOD("on_quit"), &SandboxApp::on_quit);
}

void SandboxApp::_ready() {
    if (Engine::get_singleton()->is_editor_hint()) {
        set_process(false);
        return;
    }
    session_node_ = get_node<SandboxSessionNode>("Session");
    menu_ = get_node<Control>("MainMenu");
    world_ = get_node<Control>("WorldView");
    status_ = get_node<Label>("WorldView/Margin/Scroll/Column/Status");
    tick_ = get_node<Label>("WorldView/Margin/Scroll/Column/Tick");
    content_ = get_node<Label>("WorldView/Margin/Scroll/Column/Content");
    error_ = get_node<Label>("MainMenu/Center/Column/Error");
    menu_status_ = get_node<Label>("MainMenu/Center/Column/SessionStatus");
    new_button_ = get_node<Button>("MainMenu/Center/Column/NewWorld");
    pause_button_ = get_node<Button>("WorldView/Margin/Scroll/Column/Actions/Pause");
    return_button_ = get_node<Button>("WorldView/Margin/Scroll/Column/Actions/Return");
    wall_button_ = get_node<Button>("MainMenu/Center/Column/NewWall");
    menu_load_button_ = get_node<Button>("MainMenu/Center/Column/Load");
    pickup_button_ = get_node<Button>("WorldView/Margin/Scroll/Column/Tools/Pickup");
    save_button_ = get_node<Button>("WorldView/Margin/Scroll/Column/Tools/Save");
    load_button_ = get_node<Button>("WorldView/Margin/Scroll/Column/Tools/Load");
    operation_ = get_node<Label>("WorldView/Margin/Scroll/Column/Operation");
    io_ = get_node<Label>("WorldView/Margin/Scroll/Column/IO");
    canvas_ = get_node<SandboxWorldCanvas>("WorldView/Canvas");
    inspector_ = get_node<Control>("WorldView/Margin");
    get_node<Button>("WorldView/Tabs/Tools")->connect("pressed", callable_mp(this, &SandboxApp::on_tools_tab));
    get_node<Button>("WorldView/Tabs/Data")->connect("pressed", callable_mp(this, &SandboxApp::on_data_tab));
    get_node<Button>("WorldView/Tabs/Toggle")->connect("pressed", callable_mp(this, &SandboxApp::on_toggle_panel));
    if (!session_node_ || !menu_ || !world_ || !status_ || !tick_ || !content_ || !error_ || !menu_status_ || !new_button_ || !pause_button_ || !return_button_ || !wall_button_ || !menu_load_button_ || !pickup_button_ || !save_button_ || !load_button_ || !operation_ || !io_ || !canvas_) {
        UtilityFunctions::push_error("[host] Scene contract broken: required UI or Session node missing.");
        get_tree()->quit(1);
        return;
    }
    new_button_->connect("pressed", callable_mp(this, &SandboxApp::on_new_world));
    wall_button_->connect("pressed", callable_mp(this, &SandboxApp::on_wall_world));
    menu_load_button_->connect("pressed", callable_mp(this, &SandboxApp::on_load));
    pickup_button_->connect("pressed", callable_mp(this, &SandboxApp::on_pickup));
    save_button_->connect("pressed", callable_mp(this, &SandboxApp::on_save));
    load_button_->connect("pressed", callable_mp(this, &SandboxApp::on_load));
    canvas_->set_session_node(session_node_);
    lab_button_=get_node<Button>("MainMenu/Center/Column/MaterialLab");
    step_button_=get_node<Button>("WorldView/Margin/Scroll/Column/Brush/Step");
    reset_button_=get_node<Button>("WorldView/Margin/Scroll/Column/Brush/Reset");
    material_picker_=get_node<OptionButton>("WorldView/Margin/Scroll/Column/Brush/Material");
    tool_picker_=get_node<OptionButton>("WorldView/Margin/Scroll/Column/Brush/Tool");
    radius_picker_=get_node<OptionButton>("WorldView/Margin/Scroll/Column/Brush/Radius");
    lab_button_->connect("pressed",callable_mp(this,&SandboxApp::on_lab_world));
    step_button_->connect("pressed",callable_mp(this,&SandboxApp::on_step));
    reset_button_->connect("pressed",callable_mp(this,&SandboxApp::on_reset));
    const auto catalog=load_lab_content();
    if(!catalog){UtilityFunctions::push_error(String::utf8(catalog.error.c_str()));get_tree()->quit(1);return;}
    for(const auto& d:catalog.registry->materials()){
        brush_ids_.push_back(d.id);material_picker_->add_item(String::utf8(d.display_name.c_str()));
        if(d.id=="material.water")material_picker_->select(static_cast<int>(brush_ids_.size()-1));
    }
    for(const auto text:{"画材料 / Paint","擦除 / Erase","加热 / Heat","冷却 / Cool","松动固体 / Loosen","冲击材料 / Push","爆炸 / Blast","通电 / Electrify","创建刚体 / Body","切出刚体 / Detach","原版暖转换 T10","原版冷转换 T-100","正常环境","寒冷环境 5","炎热环境 12"})tool_picker_->add_item(String::utf8(text));
    for(int i=1;i<=8;++i)radius_picker_->add_item(String::utf8("半径 ")+String::num_int64(i));
    radius_picker_->select(3);
    for(auto* picker:{material_picker_,tool_picker_,radius_picker_})picker->connect("item_selected",callable_mp(this,&SandboxApp::on_brush_changed));
    on_brush_changed(0);
    pause_button_->connect("pressed", callable_mp(this, &SandboxApp::on_pause));
    return_button_->connect("pressed", callable_mp(this, &SandboxApp::on_return));
    get_node<Button>("MainMenu/Center/Column/Quit")->connect("pressed", callable_mp(this, &SandboxApp::on_quit));
    refresh_view();
    lab_button_->grab_focus();
    const auto args = OS::get_singleton()->get_cmdline_user_args();
    smoke_ = args.has("--smoke-test") || args.has("--material-soak-test");
    UtilityFunctions::print("[host] PixelSandbox extension ready; fixed logic=60 Hz; smoke=", smoke_);
}

void SandboxApp::on_lab_world(){
    if(!session_node_->start_material_lab()){error_->set_text(session_node_->get_last_error());return;}
    error_->set_text("");refresh_view();pause_button_->grab_focus();
}
void SandboxApp::on_tools_tab(){data_tab_=false;inspector_->show();refresh_panel();}
void SandboxApp::on_data_tab(){data_tab_=true;inspector_->show();refresh_panel();}
void SandboxApp::on_toggle_panel(){inspector_->set_visible(!inspector_->is_visible());refresh_panel();}
void SandboxApp::_input(const Ref<InputEvent>& event){
    if(!session_node_ || !world_->is_visible_in_tree())return;
    const Ref<InputEventKey> key=event;
    if(key.is_valid() && key->get_keycode()==KEY_TAB && key->is_pressed() && !key->is_echo()){
        on_toggle_panel();get_viewport()->set_input_as_handled();
    }
}
void SandboxApp::refresh_panel(){
    const auto view=session_node_->session()?session_node_->session()->published_world():std::nullopt;
    const bool lab=view && view->material_lab;
    get_node<Control>("WorldView/Margin/Scroll/Column/Brush")->set_visible(lab && !data_tab_);
    get_node<Control>("WorldView/Margin/Scroll/Column/Tools")->set_visible(!data_tab_);
    get_node<Control>("WorldView/Margin/Scroll/Column/Actions")->set_visible(!data_tab_);
    status_->set_visible(data_tab_);tick_->set_visible(data_tab_);content_->set_visible(data_tab_);
    get_node<Label>("WorldView/Margin/Scroll/Column/Title")->set_text(data_tab_?String::utf8("世界数据"):String::utf8("世界工具"));
    get_node<Button>("WorldView/Tabs/Tools")->set_pressed_no_signal(!data_tab_);
    get_node<Button>("WorldView/Tabs/Data")->set_pressed_no_signal(data_tab_);
    get_node<Button>("WorldView/Tabs/Toggle")->set_text(inspector_->is_visible()?String::utf8("收起 [Tab]"):String::utf8("展开 [Tab]"));
}
void SandboxApp::on_step(){session_node_->request_single_step();}
void SandboxApp::on_reset(){session_node_->request_reset_lab();}
void SandboxApp::on_brush_changed(int64_t){
    constexpr CommandKind tools[]{CommandKind::paint,CommandKind::erase,CommandKind::heat,CommandKind::cool,CommandKind::loosen,CommandKind::push,CommandKind::blast,CommandKind::electrify,CommandKind::spawn_body,CommandKind::detach_body,CommandKind::source_warm,CommandKind::source_cold,CommandKind::environment_normal,CommandKind::environment_cold,CommandKind::environment_hot};
    const auto selected=material_picker_->get_selected();
    if(selected>=0 && static_cast<std::size_t>(selected)<brush_ids_.size())
        canvas_->set_brush(brush_ids_[static_cast<std::size_t>(selected)],tools[tool_picker_->get_selected()],radius_picker_->get_selected()+1);
}

void SandboxApp::on_wall_world() {
    if (!session_node_->start_wall_world()) { error_->set_text(session_node_->get_last_error()); return; }
    error_->set_text(""); refresh_view(); pause_button_->grab_focus();
}
void SandboxApp::on_pickup() { static_cast<void>(session_node_->submit_command({CommandKind::pick_up,{}})); }
void SandboxApp::on_save() { session_node_->request_save(); }
void SandboxApp::on_load() { session_node_->request_load(); }

void SandboxApp::on_new_world() {
    if (!session_node_->start_world()) {
        error_->set_text(session_node_->get_last_error());
        return;
    }
    error_->set_text("");
    refresh_view();
    pause_button_->grab_focus();
}

void SandboxApp::on_pause() {
    if (!session_node_->toggle_pause()) {
        UtilityFunctions::push_error("[ui] ", session_node_->get_last_error());
    }
    refresh_view();
}

void SandboxApp::on_return() {
    session_node_->end_session();
    refresh_view();
    new_button_->grab_focus();
}

void SandboxApp::on_quit() {
    session_node_->end_session();
    get_tree()->quit();
}

void SandboxApp::_exit_tree() {
    // Children leave before their parent: SandboxSessionNode::_exit_tree owns teardown.
    session_node_ = nullptr;
}

void SandboxApp::refresh_view() {
    const bool active = session_node_->has_session();
    menu_->set_visible(!active);
    world_->set_visible(active);
    const String state = session_node_->get_session_state();
    menu_status_->set_text("SESSION  /  " + state + "     TICK  /  " + String::num_int64(session_node_->get_tick()));
    status_->set_text("SESSION  /  " + state + String::utf8("     左键挖掘 / 右键补齐 8×8 格 / 金色标记为聚合掉落"));
    tick_->set_text("LOGIC TICK  /  " + String::num_int64(session_node_->get_tick()));
    const auto view = active ? session_node_->session()->published_world() : std::nullopt;
    if (view && !view->material_lab) content_->set_text(String::utf8("石材 / 世界 ") + String::num_uint64(view->world_stone) + String::utf8("   掉落 ") + String::num_uint64(view->dropped_stone) + String::utf8("   持有 ") + String::num_uint64(view->held_stone) + String::utf8("   总量 ") + String::num_uint64(view->world_stone + view->dropped_stone + view->held_stone) + " / " + String::num_uint64(view->initial_stone));
    const bool lab=view && view->material_lab;
    refresh_panel();
    pickup_button_->set_visible(!lab);
    if(lab){
        status_->set_text(String::utf8("左键拖动画材料 / 右键擦除 · 加热点燃木头和油 · 暂停后可排队操作并单步执行")+" / env "+String::num_int64(view->freeze_chance)+" / "+String::num_int64(view->unfreeze_chance));
        tick_->set_text("TICK "+String::num_int64(session_node_->get_tick())+"  /  "+state+"  /  active "+String::num_int64(view->active_chunks)+"  /  moved "+String::num_int64(view->moved_cells)+"  /  reactions "+String::num_int64(view->reactions)+"  /  blasts "+String::num_int64(view->explosions)+"  /  E "+String::num_uint64(view->electric_energy)+" ("+String::num_int64(view->electric_pulses)+")"+"  /  B "+String::num_int64(view->rigid_bodies)+"/"+String::num_int64(view->rigid_pixels)+"  /  "+String::num(view->simulation_ms,2)+" ms");
        String quantities;
        const auto* w=session_node_->session()->world();
        for(const auto& id:w->material_ids()){
            const auto count=w->material_count(w->material_index(id));
            if(count)quantities+=String::utf8(session_node_->session()->content().find_material(id)->display_name.c_str())+" "+String::num_uint64(count)+"   ";
        }
        content_->set_text(quantities);
    }
    operation_->set_text(session_node_->get_action_status());
    if(lab && !canvas_->hover_status().is_empty())operation_->set_text(session_node_->get_action_status()+"\n"+canvas_->hover_status());
    if(lab && view->deferred_reactions)operation_->set_text(operation_->get_text()+String::utf8(" · 转化等待空间/碎片容量：")+String::num_int64(view->deferred_reactions));
    if(lab && view->fluid_displaced)operation_->set_text(operation_->get_text()+String::utf8(" · 刚体排液：")+String::num_int64(view->fluid_displaced));
    if(lab && view->fluid_blocked)operation_->set_text(operation_->get_text()+String::utf8(" · 排液空间不足，本步刚体运动已撤回"));
    io_->set_text(session_node_->get_io_status());
    if (!active) {
        error_->set_text(session_node_->get_last_error().is_empty() ? session_node_->get_io_status() : session_node_->get_last_error());
        error_->add_theme_color_override("font_color", session_node_->io_ok() ? Color(0.55f,0.80f,0.65f) : Color(0.83f,0.66f,0.43f));
    }
    pause_button_->set_text(state == "Paused" ? String::utf8("继续世界  /  Resume") : String::utf8("暂停世界  /  Pause"));
}

void SandboxApp::_process(double delta) {
    if (!session_node_) return;
    refresh_view(); // Pure presentation; never advances a GameSession.
    if (smoke_ && !smoke_finished_) smoke_step(delta);
}

} // namespace pixel::host
