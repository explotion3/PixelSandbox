#pragma once
#include "godot_host/session_node.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <unordered_map>

namespace pixel::host {
class SandboxWorldCanvas : public godot::Control {
    GDCLASS(SandboxWorldCanvas, godot::Control)
public:
    void _ready() override;
    void _process(double delta) override;
    void _draw() override;
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
    void set_session_node(SandboxSessionNode* node) { session_node_ = node; }
    [[nodiscard]] godot::Vector2 cell_center(CellPos p) const;
    [[nodiscard]] std::uint64_t texture_creations() const { return creations_; }
    [[nodiscard]] std::uint64_t texture_updates() const { return updates_; }
    [[nodiscard]] bool material_art_loaded() const { return !rock_pixels_.is_empty(); }
    [[nodiscard]] godot::String hover_status() const;
    void set_brush(const std::string& material, CommandKind tool, int radius) { selected_=material;tool_=tool;radius_=radius; }
protected:
    static void _bind_methods() {}
private:
    struct Page {
        ChunkPos position;
        godot::PackedByteArray bytes;
        godot::Ref<godot::Image> image;
        godot::Ref<godot::ImageTexture> texture;
    };
    SandboxSessionNode* session_node_ = nullptr;
    std::vector<Page> pages_;
    struct BodyPage {
        std::uint32_t id=0;
        std::uint64_t revision=0;
        std::vector<std::uint64_t> visual_flags;
        CellPos origin;
        godot::Ref<godot::Image> image;
        godot::Ref<godot::ImageTexture> texture;
    };
    std::vector<BodyPage> body_pages_;
    godot::Vector2 previous_size_;
    std::optional<CellPos> hover_;
    std::uint64_t creations_ = 0, updates_ = 0;
    godot::PackedByteArray rock_pixels_;
    int rock_width_ = 0, rock_height_ = 0;
    struct Art { std::vector<std::uint8_t> pixels;int width=0,height=0; };
    struct RenderMaterial { std::uint32_t color=0;const Art* art=nullptr;bool fire=false;bool source_burning=false; };
    std::unordered_map<std::string,Art> art_;
    std::vector<RenderMaterial> palette_;
    std::string selected_="material.sand";
    CommandKind tool_=CommandKind::paint;
    int radius_=4;
    std::optional<CellPos> last_stroke_;
    void brush_at(CellPos p, bool erase);
    void on_mouse_exited();
    [[nodiscard]] godot::Rect2 world_rect() const;
    [[nodiscard]] std::optional<CellPos> cell_at(godot::Vector2 local) const;
    void upload(Page& page, const MaterialChunk& chunk, bool create);
    void update_body_pages(const WorldState& world);
};
} // namespace pixel::host
