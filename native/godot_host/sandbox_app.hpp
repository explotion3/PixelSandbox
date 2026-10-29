#pragma once

#include "godot_host/session_node.hpp"
#include "godot_host/world_canvas.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>

namespace pixel::host {

class SandboxApp : public godot::Control {
    GDCLASS(SandboxApp, godot::Control)
public:
    void _ready() override;
    void _process(double delta) override;
    void _input(const godot::Ref<godot::InputEvent>& event) override;
    void _exit_tree() override;

protected:
    static void _bind_methods();

private:
    SandboxSessionNode* session_node_ = nullptr;
    godot::Control* menu_ = nullptr;
    godot::Control* world_ = nullptr;
    godot::Label* status_ = nullptr;
    godot::Label* tick_ = nullptr;
    godot::Label* content_ = nullptr;
    godot::Label* error_ = nullptr;
    godot::Label* menu_status_ = nullptr;
    godot::Label* operation_ = nullptr;
    godot::Label* io_ = nullptr;
    SandboxWorldCanvas* canvas_ = nullptr;
    godot::Control* inspector_ = nullptr;
    bool data_tab_ = false;
    void on_tools_tab();
    void on_data_tab();
    void on_toggle_panel();
    void refresh_panel();
    godot::Button* wall_button_ = nullptr;
    godot::Button* menu_load_button_ = nullptr;
    godot::Button* pickup_button_ = nullptr;
    godot::Button* save_button_ = nullptr;
    godot::Button* load_button_ = nullptr;
    godot::Button* new_button_ = nullptr;
    godot::Button* pause_button_ = nullptr;
    godot::Button* return_button_ = nullptr;
    godot::Button* lab_button_ = nullptr;
    godot::Button* step_button_ = nullptr;
    godot::Button* reset_button_ = nullptr;
    godot::OptionButton* material_picker_ = nullptr;
    godot::OptionButton* tool_picker_ = nullptr;
    godot::OptionButton* radius_picker_ = nullptr;
    std::vector<std::string> brush_ids_;
    bool smoke_ = false;
    bool smoke_finished_ = false;
    int smoke_phase_ = 0;
    int wait_frames_ = 0;
    int64_t observed_tick_ = 0;
    uint64_t observed_physics_frame_ = 0;
    double smoke_elapsed_ = 0.0;
    std::optional<WorldSnapshot> smoke_snapshot_;
    std::uint64_t smoke_io_serial_ = 0;
    std::uint64_t smoke_texture_creations_ = 0;
    std::uint64_t smoke_texture_updates_ = 0;
    std::uint64_t smoke_removed_ = 0;
    std::uint32_t smoke_body_id_ = 0;
    std::uint64_t smoke_acid_gas_ = 0;
    std::uint64_t smoke_steam_peak_ = 0;
    std::uint32_t smoke_blast_target_ = 0;
    std::uint64_t smoke_basin_water_=0,smoke_fluid_displaced_=0;
    void on_new_world();
    void on_lab_world();
    void on_step();
    void on_reset();
    void on_brush_changed(int64_t index);
    void on_wall_world();
    void on_pickup();
    void on_save();
    void on_load();
    void on_pause();
    void on_return();
    void on_quit();
    void refresh_view();
    void smoke_step(double delta);
    void click_button(godot::Button* button);
    void click_canvas(CellPos p, godot::MouseButton button);
    bool check(bool condition, const godot::String& message);
};

} // namespace pixel::host
