#include "godot_host/world_canvas.hpp"
#include "godot_host/material_definition_resource.hpp"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cmath>

namespace pixel::host {
using namespace godot;
namespace {
std::uint8_t burned_channel(int base,int boost,std::uint32_t original,std::uint32_t changed,int shift){
    const double value=base+boost;
    if(!changed)return static_cast<std::uint8_t>(std::min(255.0,value));
    const auto before=(original>>shift)&255,after=(changed>>shift)&255;
    return static_cast<std::uint8_t>(std::clamp(before?value*after/before:static_cast<double>(after),0.0,255.0));
}
}
void SandboxWorldCanvas::_ready() {
    set_texture_filter(CanvasItem::TEXTURE_FILTER_NEAREST);
    set_mouse_filter(Control::MOUSE_FILTER_STOP);
    set_clip_contents(true);
    connect("mouse_exited",callable_mp(this,&SandboxWorldCanvas::on_mouse_exited));
    if (Engine::get_singleton()->is_editor_hint()) { set_process(false); return; }
    const Ref<Texture2D> texture = ResourceLoader::get_singleton()->load("res://assets/prototype_noita/rock.png");
    const Ref<Image> image = texture.is_valid() ? texture->get_image() : Ref<Image>{};
    if (image.is_null() || image->is_empty() || (image->is_compressed() && image->decompress() != OK)) {
        UtilityFunctions::push_error("[render] Cannot load prototype Noita rock texture.");
        set_process(false);
        return;
    }
    image->convert(Image::FORMAT_RGBA8);
    rock_width_ = image->get_width(); rock_height_ = image->get_height();
    rock_pixels_ = image->get_data();
    const auto catalog=load_lab_content();
    if(!catalog){UtilityFunctions::push_error(String::utf8(catalog.error.c_str()));return;}
    for(const auto& d:catalog.registry->materials()){
        if(d.texture_path.empty() || art_.contains(d.texture_path))continue;
        const Ref<Texture2D> t=ResourceLoader::get_singleton()->load(String::utf8(d.texture_path.c_str()));
        const Ref<Image> im=t.is_valid()?t->get_image():Ref<Image>{};
        if(im.is_null() || im->is_empty() || (im->is_compressed() && im->decompress()!=OK)){
            UtilityFunctions::push_error("Cannot decode material texture: ",String::utf8(d.texture_path.c_str()));continue;
        }
        im->convert(Image::FORMAT_RGBA8);const auto data=im->get_data();
        art_.emplace(d.texture_path,Art{{data.ptr(),data.ptr()+data.size()},im->get_width(),im->get_height()});
    }
    UtilityFunctions::print("[render] Loaded prototype Noita rock texture ", rock_width_, "x", rock_height_);
}
Rect2 SandboxWorldCanvas::world_rect() const {
    if (!session_node_ || !session_node_->session()) return {};
    const auto bounds = session_node_->session()->published_world()->bounds;
    const auto size = get_size();
    const auto scale = std::max(0.01, std::min(static_cast<double>(size.x) / bounds.width, static_cast<double>(size.y) / bounds.height));
    const Vector2 dimensions(static_cast<real_t>(bounds.width * scale), static_cast<real_t>(bounds.height * scale));
    return {(size - dimensions) * 0.5, dimensions};
}
Vector2 SandboxWorldCanvas::cell_center(CellPos p) const {
    if (!session_node_ || !session_node_->session()) return {};
    const auto b = session_node_->session()->published_world()->bounds;
    const auto rect = world_rect();
    const auto scale = rect.size.x / b.width;
    return rect.position + Vector2(static_cast<real_t>(p.x - b.origin.x + 0.5), static_cast<real_t>(p.y - b.origin.y + 0.5)) * scale;
}
std::optional<CellPos> SandboxWorldCanvas::cell_at(Vector2 local) const {
    if (!session_node_ || !session_node_->session()) return std::nullopt;
    const auto rect = world_rect();
    if (!rect.has_point(local)) return std::nullopt;
    const auto b = session_node_->session()->published_world()->bounds;
    const auto relative = (local - rect.position) / (rect.size.x / b.width);
    const CellPos p{b.origin.x + static_cast<int>(std::floor(relative.x)), b.origin.y + static_cast<int>(std::floor(relative.y))};
    return b.contains(p) ? std::optional{p} : std::nullopt;
}
void SandboxWorldCanvas::upload(Page& page, const MaterialChunk& chunk, bool create) {
    if (page.bytes.is_empty()) page.bytes.resize(chunk_size * chunk_size * 4);
    auto* bytes = page.bytes.ptrw();
    for (int i = 0; i < chunk_size * chunk_size; ++i) {
        const auto material=static_cast<std::uint16_t>(chunk.cells[static_cast<std::size_t>(i)]);
        const auto& render=palette_[material];
        const int x = chunk.position.x * chunk_size + i % chunk_size;
        const int y = chunk.position.y * chunk_size + i / chunk_size;
        const auto* texture=render.art;
        const int source=texture?(((y%texture->height+texture->height)%texture->height)*texture->width+(x%texture->width+texture->width)%texture->width)*4:0;
        for(int c=0;c<3;++c){
            const int base=texture?texture->pixels[static_cast<std::size_t>(source+c)]:static_cast<int>((render.color>>(16-c*8))&255);
            bytes[i*4+c]=material?burned_channel(base,texture?65:32,render.color,chunk.states[static_cast<std::size_t>(i)].fire_color,16-c*8):0;
        }
        if(render.fire || (!render.source_burning && chunk.states[static_cast<std::size_t>(i)].burning)){bytes[i*4]=255;bytes[i*4+1]=static_cast<std::uint8_t>(110+chunk.states[static_cast<std::size_t>(i)].life%80);bytes[i*4+2]=20;}
        if(chunk.states[static_cast<std::size_t>(i)].electrified_ticks){bytes[i*4]=100;bytes[i*4+1]=190;bytes[i*4+2]=255;}
        bytes[i*4+3]=material?static_cast<std::uint8_t>(std::max(110u,render.color>>24)):0;
    }
    if (create) {
        page.image = Image::create_from_data(chunk_size, chunk_size, false, Image::FORMAT_RGBA8, page.bytes);
        page.texture = ImageTexture::create_from_image(page.image);
        ++creations_;
    } else {
        page.image->set_data(chunk_size, chunk_size, false, Image::FORMAT_RGBA8, page.bytes);
        page.texture->update(page.image);
        ++updates_;
    }
}
void SandboxWorldCanvas::_process(double /*delta*/) {
    if (rock_pixels_.is_empty()) return;
    if (!session_node_ || !session_node_->session()) {
        if (!pages_.empty() || !body_pages_.empty()) { pages_.clear();body_pages_.clear(); hover_.reset(); queue_redraw(); }
        return;
    }
    const auto* world = session_node_->session()->world();
    const auto changes = session_node_->consume_changes();
    if (changes.full_refresh) {
        body_pages_.clear();
        palette_.clear();palette_.push_back({});
        for(const auto& id:world->material_ids()){
            const auto* d=session_node_->session()->content().find_material(id);
            const auto texture=art_.find(d->texture_path);
            palette_.push_back({d->color,texture==art_.end()?nullptr:&texture->second,d->cell_type=="fire",d->source_burning});
        }
        pages_.clear();
        pages_.reserve(world->chunks().size());
        for (const auto& chunk : world->chunks()) { pages_.push_back({chunk.position, {}, {}, {}}); upload(pages_.back(), chunk, true); }
    } else for (auto position : changes.dirty_chunks) {
        auto page = std::find_if(pages_.begin(), pages_.end(), [position](const auto& value) { return value.position == position; });
        if (page != pages_.end()) upload(*page, *world->chunk_at(position), false);
    }
    if(changes.full_refresh || changes.bodies_changed)update_body_pages(*world);
    if (changes.full_refresh || changes.bodies_changed || changes.drops_changed || !changes.dirty_chunks.empty() || previous_size_ != get_size()) queue_redraw();
    previous_size_ = get_size();
}
void SandboxWorldCanvas::update_body_pages(const WorldState& world){
    body_pages_.erase(std::remove_if(body_pages_.begin(),body_pages_.end(),[&](const auto& page){return std::none_of(world.bodies().begin(),world.bodies().end(),[&](const auto& b){return b.id==page.id;});}),body_pages_.end());
    for(const auto& b:world.bodies()){
        auto found=std::find_if(body_pages_.begin(),body_pages_.end(),[&](const auto& p){return p.id==b.id;});
        if(found==body_pages_.end()){body_pages_.push_back({});found=body_pages_.end()-1;found->id=b.id;}
        std::vector<std::uint64_t> flags;flags.reserve(b.pixels.size());for(const auto& p:b.pixels)flags.push_back((static_cast<std::uint64_t>(p.state.fire_color)<<2)|(p.state.burning?1u:0u)|(p.state.electrified_ticks?2u:0u));
        auto& page=*found;if(page.revision==b.shape_revision && page.visual_flags==flags)continue;
        int left=32,top=32,right=-32,bottom=-32;
        for(const auto& p:b.pixels){left=std::min(left,p.local.x);top=std::min(top,p.local.y);right=std::max(right,p.local.x);bottom=std::max(bottom,p.local.y);}
        const int width=right-left+1,height=bottom-top+1;
        PackedByteArray bytes;bytes.resize(width*height*4);bytes.fill(0);auto* output=bytes.ptrw();
        for(const auto& p:b.pixels){
            const auto at=((p.local.y-top)*width+p.local.x-left)*4;const auto& render=palette_[p.material];const auto* art=render.art;
            const int source=art?(((p.local.y%art->height+art->height)%art->height)*art->width+(p.local.x%art->width+art->width)%art->width)*4:0;
            for(int c=0;c<3;++c){const int base=art?art->pixels[static_cast<std::size_t>(source+c)]:static_cast<int>((render.color>>(16-c*8))&255);output[at+c]=burned_channel(base,art?65:32,render.color,p.state.fire_color,16-c*8);}output[at+3]=255;
            if(render.fire || (p.state.burning && !render.source_burning)){output[at]=255;output[at+1]=110;output[at+2]=20;}
            if(p.state.electrified_ticks){output[at]=100;output[at+1]=190;output[at+2]=255;}
        }
        const bool same_size=page.image.is_valid() && page.image->get_width()==width && page.image->get_height()==height;
        if(same_size)page.image->set_data(width,height,false,Image::FORMAT_RGBA8,bytes);
        else page.image=Image::create_from_data(width,height,false,Image::FORMAT_RGBA8,bytes);
        if(page.texture.is_null()){page.texture=ImageTexture::create_from_image(page.image);++creations_;}
        else{if(same_size)page.texture->update(page.image);else page.texture->set_image(page.image);++updates_;}
        page.origin={left,top};page.revision=b.shape_revision;page.visual_flags=std::move(flags);
    }
}
void SandboxWorldCanvas::_draw() {
    draw_rect({{}, get_size()}, Color(0.045f,0.075f,0.085f));
    if (!session_node_ || !session_node_->session()) return;
    const auto* world = session_node_->session()->world();
    const auto b = world->view().bounds;
    const auto rect = world_rect();
    const auto scale = rect.size.x / b.width;
    draw_rect(rect, Color(0.08f,0.12f,0.13f));
    for (const auto& page : pages_) {
        const auto origin = rect.position + Vector2(static_cast<real_t>(page.position.x * chunk_size - b.origin.x), static_cast<real_t>(page.position.y * chunk_size - b.origin.y)) * scale;
        draw_texture_rect(page.texture, {origin, Vector2(chunk_size,chunk_size) * scale}, false);
    }
    for(const auto& page:body_pages_){
        const auto found=std::find_if(world->bodies().begin(),world->bodies().end(),[&](const auto& b){return b.id==page.id;});if(found==world->bodies().end())continue;
        const auto position=rect.position+Vector2(static_cast<real_t>(found->x-b.origin.x),static_cast<real_t>(found->y-b.origin.y))*scale;
        draw_set_transform(position,static_cast<real_t>(found->angle),Vector2(scale,scale));
        draw_texture(page.texture,Vector2(page.origin.x-0.5f,page.origin.y-0.5f));draw_set_transform({},0,Vector2(1,1));
    }
    for (int x = b.origin.x; x <= b.origin.x + b.width; ++x) if (x % chunk_size == 0) {
        const auto px = rect.position.x + (x - b.origin.x) * scale;
        draw_line({px,rect.position.y}, {px,rect.get_end().y}, Color(0.45f,0.72f,0.66f,0.4f));
    }
    for (int y = b.origin.y; y <= b.origin.y + b.height; ++y) if (y % chunk_size == 0) {
        const auto py = rect.position.y + (y - b.origin.y) * scale;
        draw_line({rect.position.x,py}, {rect.get_end().x,py}, Color(0.45f,0.72f,0.66f,0.4f));
    }
    for (const auto& drop : world->drops()) draw_rect({cell_center(drop.position) - Vector2(3,3), Vector2(6,6)}, Color(0.96f,0.68f,0.26f));
    if (hover_) {
        if(world->view().material_lab){draw_arc(cell_center(*hover_),radius_*scale,0,6.2831853f,32,Color(1,0.83f,0.4f),1.5);return;}
        const auto grid = building_cell_origin(*hover_);
        const auto start = cell_center(grid) - Vector2(0.5,0.5) * scale;
        draw_rect({start, Vector2(building_grid_size,building_grid_size) * scale}, Color(0.97f,0.79f,0.40f), false, 1.5);
    }
}
void SandboxWorldCanvas::_gui_input(const Ref<InputEvent>& event) {
    const Ref<InputEventMouseButton> button = event;
    const Ref<InputEventMouseMotion> motion = event;
    if (motion.is_valid()) {
        hover_=cell_at(motion->get_position());queue_redraw();
        if(hover_ && session_node_ && session_node_->session() && session_node_->session()->published_world()->material_lab){
            if(motion->get_button_mask()&MOUSE_BUTTON_MASK_LEFT)brush_at(*hover_,false);
            else if(motion->get_button_mask()&MOUSE_BUTTON_MASK_RIGHT)brush_at(*hover_,true);
        }
    }
    if (button.is_null())return;
    if (!button->is_pressed()){last_stroke_.reset();return;}
    hover_ = cell_at(button->get_position());
    if (!hover_) return;
    if(session_node_->session()->published_world()->material_lab){
        if(button->get_button_index()==MOUSE_BUTTON_LEFT || button->get_button_index()==MOUSE_BUTTON_RIGHT)brush_at(*hover_,button->get_button_index()==MOUSE_BUTTON_RIGHT);
        accept_event();queue_redraw();return;
    }
    if (button->get_button_index() == MOUSE_BUTTON_LEFT) static_cast<void>(session_node_->submit_command({CommandKind::dig,*hover_,4}));
    else if (button->get_button_index() == MOUSE_BUTTON_RIGHT) static_cast<void>(session_node_->submit_command({CommandKind::repair,*hover_}));
    accept_event(); queue_redraw();
}
void SandboxWorldCanvas::on_mouse_exited(){last_stroke_.reset();hover_.reset();queue_redraw();}
void SandboxWorldCanvas::brush_at(CellPos p,bool erase){
    const auto first=last_stroke_.value_or(p);const int dx=p.x-first.x,dy=p.y-first.y;
    const int distance=std::max(std::abs(dx),std::abs(dy));
    const int steps=std::max(1,distance/std::max(1,radius_));
    for(int i=1;i<=steps;++i){
        const CellPos q{first.x+dx*i/steps,first.y+dy*i/steps};
        static_cast<void>(session_node_->submit_command({erase?CommandKind::erase:tool_,q,radius_,selected_}));
    }
    last_stroke_=p;
}
String SandboxWorldCanvas::hover_status() const {
    if(!hover_ || !session_node_ || !session_node_->session())return {};
    const auto* world=session_node_->session()->world();
    const auto material=world->surface_material_at(*hover_);
    if(!material || *material==Material::air)return "Hover: Air";
    const auto* definition=session_node_->session()->content().find_material(world->material_ids()[static_cast<std::uint16_t>(*material)-1]);
    const auto* state=world->surface_cell_state(*hover_);
    const String interaction=" / HP "+String::num_int64(std::max(0,definition->hit_points-static_cast<int>(state->damage_taken)))+(definition->source_gas?" / gas counter "+String::num_int64(state->gas_counter):"");
    const String combustion=interaction+(definition->source_burning?
        " / fire_hp "+String::num_int64(definition->fire_hp)+" / intensity "+String::num_int64(state->fire_intensity)+" / burn updates "+String::num_int64(state->burning_ticks):
        " / fuel "+String::num_int64(state->fuel));
    if(const auto body=world->body_pixel_at(*hover_)){
        const auto& b=world->bodies()[body->body];
        std::uint16_t fuse=0;for(const auto& p:b.pixels)if(p.state.detonation_ticks && (!fuse || p.state.detonation_ticks<fuse))fuse=p.state.detonation_ticks;
        return String::utf8(definition->display_name.c_str())+" / body "+String::num_int64(b.id)+" / pixels "+String::num_int64(b.pixels.size())+
            " / T "+String::num_int64(state->temperature)+combustion+(fuse?" / body fuse "+String::num_int64(fuse):"")+
            (state->electrified_ticks?" / electric "+String::num_int64(state->electrified_ticks):"")+" / velocity ("+String::num(b.vx,2)+", "+String::num(b.vy,2)+") / angle "+String::num(b.angle,2);
    }
    return String::utf8(definition->display_name.c_str())+" / T "+String::num_int64(state->temperature)+
        (definition->source_liquid
            ? " / flow credit ("+String::num(state->liquid_lateral,3)+", "+String::num(state->liquid_fall,3)+") / direction "+String::num_int64(state->liquid_direction)
            : " / velocity ("+String::num((state->velocity_x+state->momentum_remainder_x/256.0)/256,4)+", "+String::num((state->velocity_y+state->momentum_remainder_y/256.0)/256,4)+")")+
        combustion+" / life "+String::num_int64(state->life)+
        (state->mobilized?" / loose "+String::num_int64(state->quiet_ticks):"")+(state->detonation_ticks?" / fuse "+String::num_int64(state->detonation_ticks):"")+(state->electrified_ticks?" / electric "+String::num_int64(state->electrified_ticks):"");
}
} // namespace pixel::host
