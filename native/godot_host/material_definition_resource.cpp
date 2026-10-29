#include "godot_host/material_definition_resource.hpp"
#include "content/reaction_table.hpp"
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <cmath>

namespace pixel::host {
using namespace godot;
void SandboxMaterialDefinition::_bind_methods(){
#define FIELD(kind,name) ClassDB::bind_method(D_METHOD("set_" #name,"value"),&SandboxMaterialDefinition::set_##name);ClassDB::bind_method(D_METHOD("get_" #name),&SandboxMaterialDefinition::get_##name);ADD_PROPERTY(PropertyInfo(Variant::kind,#name),"set_" #name,"get_" #name);
    FIELD(STRING,content_id) FIELD(STRING,display_name) FIELD(STRING,source_id) FIELD(STRING,cell_type)
    FIELD(STRING,texture_path) FIELD(STRING,tags) FIELD(STRING,warmth_output)
    FIELD(INT,hardness) FIELD(BOOL,sand) FIELD(BOOL,is_static) FIELD(BOOL,collectible)
    FIELD(FLOAT,density) FIELD(FLOAT,solid_friction) FIELD(INT,color) FIELD(INT,lifetime) FIELD(INT,fuel) FIELD(BOOL,initially_burning) FIELD(INT,ignition)
    FIELD(INT,heat) FIELD(INT,flow_interval) FIELD(INT,cooling) FIELD(INT,warmth_threshold)
    FIELD(BOOL,source_liquid) FIELD(FLOAT,liquid_gravity) FIELD(FLOAT,liquid_damping) FIELD(FLOAT,liquid_flow_speed)
    FIELD(INT,gravity_q8)
    FIELD(INT,damping_milli)
    FIELD(INT,lateral_speed_q8)
    FIELD(INT,flow_search)
    FIELD(INT,terminal_speed_q8)
    FIELD(INT,powder_liquid_speed_q8)
    FIELD(INT,gas_upward_speed_q8)
    FIELD(INT,gas_horizontal_speed_q8)
    FIELD(INT,gas_downward_speed_q8)
    FIELD(BOOL,requires_oxygen)
    FIELD(STRING,flame_output)
    FIELD(STRING,smoke_output)
    FIELD(STRING,burn_output)
    FIELD(INT,burn_heat)
    FIELD(BOOL,source_burning) FIELD(BOOL,burnable) FIELD(INT,fire_hp)
    FIELD(INT,generates_flames) FIELD(INT,generates_smoke) FIELD(STRING,ignition_output)
    FIELD(INT,cold_threshold)
    FIELD(STRING,cold_output)
    FIELD(BOOL,can_loosen) FIELD(INT,settle_ticks) FIELD(BOOL,slime)
    FIELD(BOOL,source_adhesion) FIELD(INT,liquid_sticks_to_ceiling)
    FIELD(INT,adhesion_strength_q8) FIELD(INT,adhesion_span) FIELD(INT,adhesion_hold_ticks)
    FIELD(INT,durability) FIELD(INT,hit_points)
    FIELD(INT,explosion_radius) FIELD(INT,explosion_ray_energy) FIELD(INT,explosion_max_durability)
    FIELD(INT,explosion_heat) FIELD(INT,explosion_impulse_q8) FIELD(INT,explosion_delay_ticks) FIELD(INT,explosion_trigger_energy)
    FIELD(BOOL,explosion_hole_enabled) FIELD(BOOL,explosion_destroy_liquids) FIELD(STRING,explosion_output) FIELD(STRING,explosion_fire_material)
    FIELD(BOOL,source_phase) FIELD(BOOL,source_gas) FIELD(BOOL,source_electricity) FIELD(INT,cold_chance_rev) FIELD(INT,warmth_chance_rev)
    FIELD(BOOL,explosion_source_rules) FIELD(INT,explosion_create_probability)
    FIELD(BOOL,electrical_conductivity) FIELD(INT,electric_cost)
#undef FIELD
}
bool SandboxMaterialDefinition::to_native(MaterialDefinition& out,String& error)const{
    if(source_adhesion_ && cell_type_!="liquid"){error="Source adhesion requires a liquid-class material: "+content_id_;return false;}
    if(cold_chance_rev_<0 || cold_chance_rev_>32767 || warmth_chance_rev_<0 || warmth_chance_rev_>32767 || explosion_create_probability_<0 || explosion_create_probability_>101){error="Invalid source interaction gate: "+content_id_;return false;}
    if(fire_hp_<-1 || fire_hp_>1000000000 || generates_flames_<0 || generates_flames_>101 || generates_smoke_<0 || generates_smoke_>101 ||
       (source_burning_ && (ignition_<0 || ignition_>100 || burn_heat_<0 || burn_heat_>255))){error="Invalid combustion threshold, emission gate or intensity: "+content_id_;return false;}
    if(!std::isfinite(solid_friction_) || solid_friction_<0 || solid_friction_>10000){error="Invalid solid_friction (finite 0..10000 required): "+content_id_;return false;}
    if(hardness_<=0 || lifetime_<0 || lifetime_>60000 || fuel_<0 || fuel_>1000000000 || color_<0 || color_>0xffffffffLL){error="Invalid hardness, lifetime, fuel or color: "+content_id_;return false;}
    out.id=content_id_.utf8().get_data();out.display_name=display_name_.utf8().get_data();out.hardness=static_cast<std::uint32_t>(hardness_);
    out.source_id=source_id_.utf8().get_data();out.cell_type=cell_type_.utf8().get_data();out.texture_path=texture_path_.utf8().get_data();
    out.sand=sand_;out.is_static=is_static_;out.collectible=collectible_;out.density=static_cast<float>(density_);out.color=static_cast<std::uint32_t>(color_);
    out.solid_friction=static_cast<float>(solid_friction_);
    out.lifetime=static_cast<std::uint16_t>(lifetime_);out.fuel=static_cast<std::uint32_t>(fuel_);out.initially_burning=initially_burning_;
    out.ignition=ignition_;out.heat=heat_;out.flow_interval=flow_interval_;out.cooling=cooling_;
    out.warmth_threshold=warmth_threshold_;out.warmth_output=warmth_output_.utf8().get_data();
    out.source_liquid=source_liquid_;out.liquid_gravity=static_cast<float>(liquid_gravity_);
    out.liquid_damping=static_cast<float>(liquid_damping_);out.liquid_flow_speed=static_cast<float>(liquid_flow_speed_);
    out.gravity_q8=gravity_q8_;
    out.damping_milli=damping_milli_;
    out.lateral_speed_q8=lateral_speed_q8_;
    out.flow_search=flow_search_;
    out.terminal_speed_q8=terminal_speed_q8_;
    out.powder_liquid_speed_q8=powder_liquid_speed_q8_;
    out.gas_upward_speed_q8=gas_upward_speed_q8_;
    out.gas_horizontal_speed_q8=gas_horizontal_speed_q8_;
    out.gas_downward_speed_q8=gas_downward_speed_q8_;
    out.requires_oxygen=requires_oxygen_;
    out.flame_output=flame_output_.utf8().get_data();
    out.smoke_output=smoke_output_.utf8().get_data();
    out.burn_output=burn_output_.utf8().get_data();
    out.burn_heat=burn_heat_;
    out.source_burning=source_burning_;out.burnable=burnable_;out.fire_hp=fire_hp_;
    out.generates_flames=generates_flames_;out.generates_smoke=generates_smoke_;out.ignition_output=ignition_output_.utf8().get_data();
    out.source_phase=source_phase_;out.source_gas=source_gas_;out.source_electricity=source_electricity_;out.cold_chance_rev=cold_chance_rev_;out.warmth_chance_rev=warmth_chance_rev_;
    out.explosion.source_rules=explosion_source_rules_;out.explosion.create_probability=explosion_create_probability_;
    out.cold_threshold=cold_threshold_;
    out.cold_output=cold_output_.utf8().get_data();
    out.can_loosen=can_loosen_;out.settle_ticks=settle_ticks_;out.slime=slime_;
    out.source_adhesion=source_adhesion_;out.liquid_sticks_to_ceiling=liquid_sticks_to_ceiling_;
    out.adhesion_strength_q8=adhesion_strength_q8_;out.adhesion_span=adhesion_span_;out.adhesion_hold_ticks=adhesion_hold_ticks_;
    out.durability=durability_;out.hit_points=hit_points_;
    out.explosion.radius=explosion_radius_;out.explosion.ray_energy=explosion_ray_energy_;out.explosion.max_durability=explosion_max_durability_;
    out.explosion.heat=explosion_heat_;out.explosion.impulse_q8=explosion_impulse_q8_;out.explosion.delay_ticks=explosion_delay_ticks_;
    out.explosion.trigger_energy=explosion_trigger_energy_;out.explosion.hole_enabled=explosion_hole_enabled_;
    out.explosion.destroy_liquids=explosion_destroy_liquids_;out.explosion.output=explosion_output_.utf8().get_data();
    out.explosion.fire_material=explosion_fire_material_.utf8().get_data();
    out.electrical_conductivity=electrical_conductivity_;out.electric_cost=electric_cost_;
    for(const auto& tag:tags_.split(",",false))out.tags.emplace_back(tag.strip_edges().utf8().get_data());
    return true;
}
RegistryResult load_content(const PackedStringArray& paths){
    std::vector<MaterialDefinition> definitions;
    for(int64_t i=0;i<paths.size();++i){
        const auto path=paths[i].strip_edges();
        if(!ResourceLoader::get_singleton()->exists(path))return {nullptr,"Content resource not found: "+std::string(path.utf8().get_data())};
        Ref<SandboxMaterialDefinition> resource=ResourceLoader::get_singleton()->load(path);
        if(resource.is_null())return {nullptr,"Expected SandboxMaterialDefinition at "+std::string(path.utf8().get_data())};
        MaterialDefinition definition;String error;
        if(!resource->to_native(definition,error))return {nullptr,std::string(path.utf8().get_data())+": "+error.utf8().get_data()};
        definitions.push_back(std::move(definition));
    }
    return ContentRegistry::build(std::move(definitions));
}
RegistryResult load_lab_content(){
    const auto paths=FileAccess::get_file_as_string("res://content/materials/catalog.txt").strip_edges().split("\n",false);
    if(paths.is_empty())return {nullptr,"Missing or empty material catalog.txt"};
    auto basic=load_content(paths);if(!basic)return basic;
    if(!FileAccess::file_exists("res://content/reactions.txt"))return {nullptr,"Missing reaction table: content/reactions.txt"};
    const auto text=FileAccess::get_file_as_string("res://content/reactions.txt").utf8();
    auto parsed=parse_reaction_table(std::string_view(text.get_data(),text.length()));
    if(!parsed)return {nullptr,parsed.error};
    return ContentRegistry::build(basic.registry->materials(),std::move(parsed.reactions));
}
}
