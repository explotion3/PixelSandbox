#include "content/content_registry.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <cmath>
#include <array>
#include <optional>
#include <functional>

namespace pixel {
namespace {
struct TagVariant {std::string tag,suffix;};
std::optional<TagVariant> tag_variant(std::string_view expression){
    if(!expression.starts_with('['))return {};
    const auto close=expression.find(']');
    if(close==expression.npos || close<=1)return {};
    const auto suffix=expression.substr(close+1);
    if(!suffix.empty() && (!suffix.starts_with('_') || suffix.size()>64 ||
        !std::all_of(suffix.begin(),suffix.end(),[](char c){return c=='_' || (c>='a' && c<='z') || (c>='0' && c<='9');})))return {};
    return TagVariant{std::string(expression.substr(1,close-1)),std::string(suffix)};
}
bool matches(const MaterialDefinition& d,std::string_view input){
    const auto tag=tag_variant(input);
    return tag && tag->suffix.empty()?d.has_tag(tag->tag):d.id==input;
}
// Resolve source-name families once; the simulation only sees ordinary IDs/tags.
bool expand_variants(const std::vector<MaterialDefinition>& definitions,std::vector<ReactionDefinition>& reactions,std::string& error){
    std::unordered_map<std::string,std::vector<const MaterialDefinition*>> sources;
    for(const auto& d:definitions)if(!d.source_id.empty())sources[d.source_id].push_back(&d);
    auto find_source=[&](const std::string& name)->const MaterialDefinition*{
        const auto found=sources.find(name);
        if(found==sources.end() || found->second.size()!=1){error="Missing or ambiguous source material variant: "+name;return nullptr;}
        return found->second.front();
    };
    std::vector<ReactionDefinition> expanded;
    for(const auto& r:reactions){
        if(r.probability_gate>=0){
            std::function<bool(ReactionDefinition)> expand=[&](ReactionDefinition value){
                std::array<std::string*,6> fields{&value.input_a,&value.input_b,&value.input_c,&value.output_a,&value.output_b,&value.output_c};
                std::string selector;
                for(const auto* field:fields){const auto begin=field->find('[');if(begin==field->npos)continue;const auto end=field->find(']',begin);
                    if(end==field->npos || end==begin+1){error="Invalid source reaction label: "+*field;return false;}
                    selector=field->substr(begin,end-begin+1);break;}
                if(!selector.empty()){
                    const auto tag=selector.substr(1,selector.size()-2);
                    for(const auto& member:definitions){if(!member.has_tag(tag))continue;
                        if(member.source_id.empty() || member.source_id.find('[')!=member.source_id.npos || !find_source(member.source_id)){error="Source label requires a unique source_id: "+member.id;return false;}
                        auto copy=value;std::array<std::string*,6> targets{&copy.input_a,&copy.input_b,&copy.input_c,&copy.output_a,&copy.output_b,&copy.output_c};
                        for(auto* field:targets){std::size_t at=0;while((at=field->find(selector,at))!=field->npos){field->replace(at,selector.size(),member.source_id);at+=member.source_id.size();}}
                        if(!expand(std::move(copy)))return false;
                    }
                    return true; // Empty source tag membership registers no records.
                }
                for(auto* field:fields){if(field->empty() || *field=="air" || *field=="keep")continue;
                    if(std::any_of(definitions.begin(),definitions.end(),[&](const auto& d){return d.id==*field;}))continue;
                    const auto* resolved=find_source(*field);if(!resolved)return false;*field=resolved->id;
                }
                if(expanded.size()>=65534){error="Too many expanded reaction variants (maximum 65534).";return false;}
                expanded.push_back(std::move(value));return true;
            };
            if(!expand(r))return false;continue;
        }
        std::array<std::vector<std::pair<std::string,std::string>>,3> slots;
        const std::array<std::string,3> inputs{r.input_a,r.input_b,r.input_c},outputs{r.output_a,r.output_b,r.output_c};
        for(int i=0;i<3;++i){
            const auto in=tag_variant(inputs[i]),out=tag_variant(outputs[i]);
            if(out && (!in || out->tag!=in->tag)){error="Reaction output tag must bind to its own input selector: "+outputs[i];return false;}
            const bool derive=in && (!in->suffix.empty() || (out && !out->suffix.empty()));
            if(!derive){slots[i].push_back({inputs[i],out?"keep":outputs[i]});continue;}
            for(const auto& base:definitions){
                if(!base.has_tag(in->tag))continue;
                if(base.source_id.empty()){error="Variant reaction requires source_id on "+base.id;return false;}
                const auto* origin=find_source(base.source_id+in->suffix);if(!origin)return false;
                const auto* result=out?find_source(base.source_id+out->suffix):nullptr;if(out && !result)return false;
                slots[i].push_back({origin->id,out?result->id:outputs[i]});
            }
            if(slots[i].empty()){error="Variant reaction has no base materials for ["+in->tag+"]";return false;}
        }
        for(const auto& a:slots[0])for(const auto& b:slots[1])for(const auto& c:slots[2]){
            if(expanded.size()>=65534){error="Too many expanded reaction variants (maximum 65534).";return false;}
            auto copy=r;copy.input_a=a.first;copy.output_a=a.second;copy.input_b=b.first;copy.output_b=b.second;copy.input_c=c.first;copy.output_c=c.second;
            expanded.push_back(std::move(copy));
        }
    }
    reactions=std::move(expanded);return true;
}
}

Motion MaterialDefinition::motion(bool mobilized) const noexcept {
    if (cell_type == "solid") return Motion::rigid;
    if (is_static && !(mobilized && can_loosen)) return Motion::fixed;
    if (cell_type == "fire") return Motion::fire;
    if (cell_type == "gas") return Motion::gas;
    return sand ? Motion::powder : Motion::liquid;
}
bool MaterialDefinition::has_tag(std::string_view tag) const {
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

ContentRegistry::ContentRegistry(std::vector<MaterialDefinition> definitions)
    : materials_(std::move(definitions)) {
    for (std::size_t i = 0; i < materials_.size(); ++i) {
        material_indices_.emplace(materials_[i].id, i);
    }
}

RegistryResult ContentRegistry::build(std::vector<MaterialDefinition> definitions, std::vector<ReactionDefinition> reactions) {
    if (definitions.empty()) {
        return {nullptr, "Content registry has no material definitions."};
    }
    std::unordered_set<std::string> ids;
    if (definitions.size() > 65534) return {nullptr, "Maximum 65534 material definitions."};
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto& definition = definitions[i];
        if (definition.id.empty()) {
            return {nullptr, "Material definition #" + std::to_string(i) + " is missing an ID."};
        }
        const bool valid_id = std::all_of(definition.id.begin(), definition.id.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
        });
        if (!valid_id || definition.id.size()>128) {
            return {nullptr, "Invalid material ID '" + definition.id + "': use lowercase ASCII, digits, dot or underscore."};
        }
        if (!ids.insert(definition.id).second) {
            return {nullptr, "Duplicate material ID '" + definition.id + "'."};
        }
        if (definition.display_name.empty() || definition.hardness == 0) {
            return {nullptr, "Material '" + definition.id + "' requires a display name and positive hardness."};
        }
        if(!std::isfinite(definition.solid_friction) || definition.solid_friction<0 || definition.solid_friction>10000)
            return {nullptr,"Invalid solid_friction for '"+definition.id+"': expected finite 0..10000."};
        if ((definition.cell_type != "liquid" && definition.cell_type != "gas" && definition.cell_type != "fire" && definition.cell_type != "solid") ||
            !std::isfinite(definition.density) || definition.density <= 0 || definition.density > 10000 ||
            definition.flow_interval < 1 || definition.flow_interval > 60 || definition.ignition < (definition.source_burning?0:1) || definition.ignition > (definition.source_burning?100:10000) ||
            definition.heat < 0 || definition.heat > 10000 || definition.lifetime > 60000 || definition.fuel > 1000000000 ||
            (!definition.source_burning && definition.initially_burning && !definition.fuel))
            return {nullptr, "Invalid motion, density, heat or lifetime for '" + definition.id + "'."};
        if(definition.gravity_q8<0 || definition.gravity_q8>2048 || definition.damping_milli<0 || definition.damping_milli>1000 ||
           definition.lateral_speed_q8<1 || definition.lateral_speed_q8>2048 || definition.flow_search<1 || definition.flow_search>128 ||
           definition.terminal_speed_q8<256 || definition.terminal_speed_q8>8192 || definition.burn_heat<0 || definition.burn_heat>10000 ||
           definition.cold_threshold<-1000 || definition.cold_threshold>0)
            return {nullptr,"Invalid transport or cold/burning parameters for '"+definition.id+"'."};
        if(definition.cold_chance_rev<0 || definition.cold_chance_rev>32767 || definition.warmth_chance_rev<0 || definition.warmth_chance_rev>32767 || (definition.source_gas && definition.cell_type!="gas"))return {nullptr,"Invalid source phase probability or gas mode: "+definition.id};
        if(!std::isfinite(definition.liquid_gravity) || definition.liquid_gravity<0 || definition.liquid_gravity>8 ||
           !std::isfinite(definition.liquid_damping) || definition.liquid_damping<0 || definition.liquid_damping>1 ||
           !std::isfinite(definition.liquid_flow_speed) || definition.liquid_flow_speed<0 || definition.liquid_flow_speed>8 ||
           (definition.source_liquid && (definition.cell_type!="liquid" || definition.sand || definition.slime || definition.is_static)))
            return {nullptr,"Invalid ordinary source liquid parameters: "+definition.id};
        if(definition.source_adhesion && definition.cell_type!="liquid")return {nullptr,"Source adhesion requires a liquid-class material: "+definition.id};
        if(definition.fire_hp < -1 || definition.fire_hp>1000000000 || definition.generates_flames<0 || definition.generates_flames>101 ||
           definition.generates_smoke<0 || definition.generates_smoke>101 || (definition.source_burning && definition.burn_heat>255))
            return {nullptr,"Invalid combustion fire_hp, emission gate or intensity byte for '"+definition.id+"'."};
        if (definition.gas_upward_speed_q8<0 || definition.gas_upward_speed_q8>8192 ||
            definition.gas_horizontal_speed_q8<0 || definition.gas_horizontal_speed_q8>8192 ||
            definition.gas_downward_speed_q8<0 || definition.gas_downward_speed_q8>8192)
            return {nullptr,"Invalid gas direction speed for '"+definition.id+"'."};
        if ((definition.can_loosen && (!definition.is_static || !definition.sand || definition.cell_type!="liquid")) ||
            definition.settle_ticks<1 || definition.settle_ticks>600 || definition.adhesion_strength_q8<0 || definition.adhesion_strength_q8>8192 ||
            definition.adhesion_span<1 || definition.adhesion_span>16 || definition.adhesion_hold_ticks<0 || definition.adhesion_hold_ticks>60000 ||
            (!definition.slime && definition.adhesion_span!=1) || (definition.slime && (definition.is_static || definition.cell_type!="liquid")) ||
            (definition.adhesion_strength_q8 && (definition.is_static || definition.cell_type!="liquid")))
            return {nullptr,"Invalid loosening, settling or adhesion parameters for '"+definition.id+"'."};
        if (definition.cell_type == "fire" && (!definition.has_tag("fire") || !definition.lifetime || definition.collectible || definition.is_static))
            return {nullptr, "Fire requires [fire], finite lifetime, moving mode and collectible=false: " + definition.id};
        const auto& e=definition.explosion;
        if(definition.electric_cost<1 || definition.electric_cost>1000)return {nullptr,"Invalid electrical transport cost for '"+definition.id+"'."};
        if(definition.durability<0 || definition.durability>10000 || definition.hit_points<0 || definition.hit_points>100000000 ||
            e.create_probability<0 || e.create_probability>101 || e.radius<0 || e.radius>48 || e.ray_energy<1 || e.ray_energy>1000000000 || e.max_durability<0 || e.max_durability>10000 ||
            e.heat<0 || e.heat>10000 || e.impulse_q8<0 || e.impulse_q8>8192 || e.delay_ticks<1 || e.delay_ticks>600 ||
            e.trigger_energy<1 || e.trigger_energy>1000000000 || (e.radius && !definition.fuel))
            return {nullptr,"Invalid durability, material HP or explosion parameters for '"+definition.id+"'."};
    }
    auto registry = std::shared_ptr<ContentRegistry>(new ContentRegistry(std::move(definitions)));
    std::string expansion_error;
    if(!expand_variants(registry->materials_,reactions,expansion_error))return {nullptr,std::move(expansion_error)};
    auto input_valid = [&](const std::string& input) {
        if(input=="air")return true;
        if (input.size() > 2 && input.front() == '[' && input.back() == ']') {
            const auto tag = input.substr(1,input.size()-2);
            return std::any_of(registry->materials_.begin(),registry->materials_.end(),[&](const auto& m) { return m.has_tag(tag); });
        }
        return registry->find_material(input) != nullptr;
    };
    auto output_valid = [&](const std::string& output) { return output == "air" || output == "keep" || registry->find_material(output); };
    for (const auto& r : reactions)
        if (!input_valid(r.input_a) || !input_valid(r.input_b) || !output_valid(r.output_a) || !output_valid(r.output_b) || r.interval < 1 || r.interval > 10000 ||
            r.minimum_age>60000 || r.minimum_temperature<-1000 || r.maximum_temperature>10000 || r.minimum_temperature>r.maximum_temperature ||
            (!r.input_c.empty() && (!input_valid(r.input_c) || !output_valid(r.output_c))) ||
            (r.input_c.empty() && r.output_c!="keep") || (r.input_a=="air" && r.input_b=="air" && (r.input_c.empty() || r.input_c=="air")))
            return {nullptr,"Invalid reaction input/output reference or interval: " + r.input_a + " + " + r.input_b};
    for (const auto& r : reactions)
        if (r.blob_radius_a<0 || r.blob_radius_a>64 || r.blob_radius_b<0 || r.blob_radius_b>64 ||
            (r.probability_gate<0 && ((r.blob_radius_a && r.input_a=="air") || (r.blob_radius_b && r.input_b=="air"))) ||
            r.probability_gate < -1 || r.probability_gate>10000 || r.direction < -1 || r.direction>3 ||
            (r.probability_gate<0 && (r.fast || r.required_neighbor || r.direction!=-1 || r.destroy_lonely || !r.restrict_a || !r.restrict_b)) ||
            (r.probability_gate>=0 && r.input_a=="air") ||
            (r.required_neighbor && (r.probability_gate<0 || r.fast || !r.input_c.empty() || r.output_b!="air" || r.blob_radius_a || r.blob_radius_b)))
            return {nullptr,"Invalid restricted reaction blob radius (0..64, non-air anchor required): " + r.input_a + " + " + r.input_b};
    for (const auto& m : registry->materials_)
        if (m.cooling < 0 || m.cooling > 1000 || m.warmth_threshold < 0 || m.warmth_threshold > 10000 ||
            (!m.warmth_output.empty() && (!registry->find_material(m.warmth_output) || (!m.source_phase && m.warmth_threshold == 0))))
            return {nullptr,"Invalid thermal conversion for " + m.id};
    for(const auto& m:registry->materials_) {
        if(m.explosion.radius)for(const auto* output:{&m.explosion.output,&m.explosion.fire_material})
            if(*output!="air" && !registry->find_material(*output))return {nullptr,"Missing explosion output "+*output+" for "+m.id};
        if(m.explosion.radius && (m.explosion.output==m.id || (m.explosion.fire_material!="air" && registry->find_material(m.explosion.fire_material)->cell_type!="fire")))
            return {nullptr,"Explosion residue must differ from its fuel and fire output must be a fire material: "+m.id};
        if(!m.cold_output.empty() && (!registry->find_material(m.cold_output) || (!m.source_phase && m.cold_threshold>=m.warmth_threshold)))
            return {nullptr,"Invalid cold conversion for "+m.id};
        if(m.fuel || (m.source_burning && (m.burnable || m.initially_burning || m.cell_type=="fire")))for(const auto* output:{&m.flame_output,&m.smoke_output,&m.burn_output})
            if(*output!="air" && !registry->find_material(*output))return {nullptr,"Missing burning output "+*output+" for "+m.id};
        if(!m.ignition_output.empty() && m.ignition_output!="air" && !registry->find_material(m.ignition_output))return {nullptr,"Missing ignition output "+m.ignition_output+" for "+m.id};
    }
    registry->reactions_ = std::move(reactions);
    registry->reaction_candidates_.resize(registry->materials_.size());
    std::size_t dispatch_entries=0;
    for(std::size_t i=0;i<registry->materials_.size();++i)for(std::size_t j=0;j<registry->reactions_.size();++j){
        const auto& r=registry->reactions_[j];const auto& d=registry->materials_[i];
        if(matches(d,r.input_a) || (r.probability_gate<0 && (matches(d,r.input_b) || (!r.input_c.empty() && matches(d,r.input_c))))){
            if(++dispatch_entries>8000000)return {nullptr,"Reaction candidate index exceeds 8000000 entries."};
            registry->reaction_candidates_[i].push_back(j);
        }
    }
    return {std::move(registry), {}};
}

const MaterialDefinition* ContentRegistry::find_material(std::string_view id) const {
    const auto it = material_indices_.find(std::string(id));
    return it == material_indices_.end() ? nullptr : &materials_[it->second];
}
std::span<const std::size_t> ContentRegistry::reaction_candidates(std::string_view id) const {
    const auto found=material_indices_.find(std::string(id));
    return found==material_indices_.end()?std::span<const std::size_t>{}:reaction_candidates_[found->second];
}

} // namespace pixel
