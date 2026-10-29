#pragma once

#include "session/game_session.hpp"
#include <godot_cpp/classes/node.hpp>
#include <memory>
#include <filesystem>

namespace pixel::host {

class SandboxSessionNode : public godot::Node {
    GDCLASS(SandboxSessionNode, godot::Node)
public:
    void _enter_tree() override;
    void _exit_tree() override;
    void _physics_process(double delta) override;
    [[nodiscard]] bool start_world();
    [[nodiscard]] bool start_wall_world();
    [[nodiscard]] bool start_material_lab();
    void request_single_step();
    void request_reset_lab();
    [[nodiscard]] bool submit_command(WorldCommand command);
    void request_save();
    void request_load();
    [[nodiscard]] ChangeBatch consume_changes();
    [[nodiscard]] godot::String get_action_status() const { return action_status_; }
    [[nodiscard]] godot::String get_io_status() const { return io_status_; }
    [[nodiscard]] std::uint64_t io_serial() const { return io_serial_; }
    [[nodiscard]] bool io_ok() const { return io_ok_; }
    [[nodiscard]] std::filesystem::path save_path() const;
    [[nodiscard]] bool toggle_pause();
    void end_session();
    [[nodiscard]] godot::String get_session_state() const;
    [[nodiscard]] int64_t get_tick() const;
    [[nodiscard]] bool has_session() const { return session_ != nullptr; }
    [[nodiscard]] godot::String get_last_error() const { return last_error_; }
    [[nodiscard]] int64_t get_content_count() const;
    [[nodiscard]] const GameSession* session() const { return session_.get(); }

protected:
    static void _bind_methods();

private:
    static inline SandboxSessionNode* active_host_ = nullptr;
    std::unique_ptr<GameSession> session_;
    godot::String last_error_;
    bool ended_ = false;
    enum class BoundaryRequest { none, save, load, reset };
    BoundaryRequest boundary_request_ = BoundaryRequest::none;
    godot::String action_status_ = "Left: dig / Right: repair selected 8x8 / Pick up: collect all drops";
    godot::String io_status_ = "No save/load operation yet.";
    std::uint64_t io_serial_ = 0;
    bool io_ok_ = false;
    bool smoke_mode_ = false;
    std::unique_ptr<GameSession> make_session();
    bool single_step_ = false;
    bool simulation_failed_ = false;
    bool start(int scenario);
    void process_boundary_request();
    bool report_error(const godot::String& message);
};

} // namespace pixel::host
