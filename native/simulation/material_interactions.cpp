#include "simulation/material_interactions.hpp"
#include "simulation/material_simulation.hpp"
#include "simulation/rigid_simulation.hpp"
#include "simulation/electrical_simulation.hpp"
#include "simulation/combustion.hpp"
#include "simulation/reaction_geometry.hpp"
#include "simulation/source_phase.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace pixel {
namespace {
constexpr CellPos directions[]{{0,-1},{-1,0},{1,0},{0,1}};
constexpr CellPos source_directions[]{{0,-1},{0,1},{-1,0},{1,0}};
std::uint64_t noise(std::uint64_t seed,std::uint64_t tick,CellPos p){
    auto n=seed^(tick*0x9e3779b97f4a7c15ULL)^(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x))<<32)^static_cast<std::uint32_t>(p.y);
    n=(n^(n>>30))*0xbf58476d1ce4e5b9ULL;n=(n^(n>>27))*0x94d049bb133111ebULL;return n^(n>>31);
}
bool rigid_material(const MaterialDefinition& d){return d.motion()==Motion::rigid || (d.motion()==Motion::fixed && d.sand);}
}
// Addresses remain stable until finish_material_changes: removed body pixels are
// temporary tombstones, never another material owner or a second raster world.
class MaterialInteractionStage {
    struct Cell {
        CellPos grid{};std::size_t body=std::numeric_limits<std::size_t>::max(),pixel=0;
        bool rigid()const{return body!=std::numeric_limits<std::size_t>::max();}
        bool operator==(const Cell&)const=default;
    };
    struct Write {Cell at;Material material;CellState state;};
    struct Neighbors {
        std::array<Cell,4> values{};std::size_t count=0;
        const Cell* begin()const{return values.data();}const Cell* end()const{return begin()+count;}
        void push_back(Cell p){values[count++]=p;}
    };
    WorldState& w;const ContentRegistry& content;
    std::vector<const MaterialDefinition*> defs{nullptr};
    struct ContactRule {std::size_t index;std::uint8_t first,second;};
    std::vector<std::span<const std::size_t>> applicable;
    std::vector<std::array<int,3>> source_gates; // ordinary, fast, required
    std::vector<std::uint8_t> legacy_contacts;
    // Derived only from the immutable catalog and save-local material indices.
    // Cache encountered pairs, not a quadratic table of every possible material.
    std::unordered_map<std::uint32_t,std::vector<ContactRule>> contact_cache;
    std::vector<ContactRule> uncached_rules;
    std::vector<Cell> cells;
    std::vector<Cell> pending_grid;
    std::vector<std::size_t> starts,parts;
    std::vector<std::array<int,65*65>> local_pixels;
    std::vector<std::uint8_t> reacted,known,thermal_newborn;
    bool in_thermal=false;
    std::size_t grid_size,fragment_total;
    std::vector<RigidSimulation::ShapeBefore> before;
    std::size_t key(Cell p)const{return p.rigid()?starts[p.body]+p.pixel:static_cast<std::size_t>(p.grid.y-w.bounds_.origin.y)*w.bounds_.width+static_cast<std::size_t>(p.grid.x-w.bounds_.origin.x);}
    Material material(Cell p)const{return p.rigid()?static_cast<Material>(w.bodies_[p.body].pixels[p.pixel].material):*w.material_at(p.grid);}
    const CellState& state(Cell p)const{return p.rigid()?w.bodies_[p.body].pixels[p.pixel].state:*w.cell_state(p.grid);}
    const MaterialDefinition* definition(Cell p)const{return defs[static_cast<std::uint16_t>(material(p))];}
    std::pair<double,double> center(Cell p)const{
        if(!p.rigid())return {p.grid.x+0.5,p.grid.y+0.5};
        const auto& b=w.bodies_[p.body];const auto local=b.pixels[p.pixel].local;
        return {b.x+std::cos(b.angle)*local.x-std::sin(b.angle)*local.y,b.y+std::sin(b.angle)*local.x+std::cos(b.angle)*local.y};
    }
    CellPos position(Cell p)const{const auto [x,y]=center(p);return {static_cast<int>(std::floor(x)),static_cast<int>(std::floor(y))};}
    std::optional<Cell> resolve(CellPos p)const{
        if(!w.bounds_.contains(p))return {};
        if(const auto body=w.body_pixel_at(p))return Cell{{},body->body,body->pixel};
        return Cell{p};
    }
    Neighbors neighbors(Cell p)const{
        Neighbors result;
        for(const auto d:directions){
            std::optional<Cell> next;
            if(!p.rigid())next=resolve({p.grid.x+d.x,p.grid.y+d.y});
            else{
                const auto& b=w.bodies_[p.body];const auto local=b.pixels[p.pixel].local;
                const auto x=local.x+d.x,y=local.y+d.y;
                if(x>=-32 && x<=32 && y>=-32 && y<=32){const auto i=local_pixels[p.body][(y+32)*65+x+32];if(i>=0 && b.pixels[static_cast<std::size_t>(i)].material)next=Cell{{},p.body,static_cast<std::size_t>(i)};}
                if(!next){const auto [wx,wy]=center(p);next=resolve({static_cast<int>(std::floor(wx+std::cos(b.angle)*d.x-std::sin(b.angle)*d.y)),static_cast<int>(std::floor(wy+std::sin(b.angle)*d.x+std::cos(b.angle)*d.y))});}
            }
            if(next && *next!=p && std::find(result.begin(),result.end(),*next)==result.end())result.push_back(*next);
        }
        return result;
    }
    void store(Cell p,Material m,CellState s){
        const auto old=material(p);
        if(old==m && state(p)==s)return;
        if(old!=m){if(old==Material::stone)--w.initial_stone_;if(m==Material::stone)++w.initial_stone_;}
        if(!p.rigid()){w.set_cell(p.grid,m,s);return;}
        auto& b=w.bodies_[p.body];auto& pixel=b.pixels[p.pixel];
        if(pixel.state.electrified_ticks && !s.electrified_ticks)--w.electrified_cells_;
        if(!pixel.state.electrified_ticks && s.electrified_ticks)++w.electrified_cells_;
        if(old!=m){
            if(old!=Material::air)--w.counts_[pixel.material];
            if(m!=Material::air)++w.counts_[static_cast<std::uint16_t>(m)];
            if(old==Material::stone)--w.world_stone_;if(m==Material::stone)++w.world_stone_;
            ++b.shape_revision;
        }
        pixel.material=static_cast<std::uint16_t>(m);pixel.state=s;++w.body_revision_;
    }
    // Plan all outputs, release destinations and fragment capacity before writes.
    // A blocked phase conversion keeps its input and is retried on later steps.
    bool commit(const std::vector<Write>& requested){
        std::vector<Write> writes;std::vector<std::size_t> affected;
        std::vector<std::pair<ElectricAddress,ElectricAddress>> transfers;
        for(auto v:requested){
            if(material(v.at)==v.material)continue;
            if(v.material!=Material::air)v.state.created_tick=w.tick_;
            if(v.at.rigid()){
                if(std::find(affected.begin(),affected.end(),v.at.body)==affected.end())affected.push_back(v.at.body);
                if(v.material!=Material::air && !rigid_material(*defs[static_cast<std::uint16_t>(v.material)])){
                    // The nearest free cardinally reachable raster cell is reserved.
                    // Search is deliberately local; products never teleport through walls.
                    const auto origin=position(v.at);std::vector<CellPos> frontier{origin},seen{origin};std::optional<CellPos> destination;
                    for(std::size_t cursor=0;cursor<frontier.size() && !destination;++cursor){
                        const auto q=frontier[cursor];if(!w.bounds_.contains(q))continue;
                        const auto surface=resolve(q);
                        const bool releasing=surface && surface->rigid() && std::any_of(requested.begin(),requested.end(),[&](const auto& r){return r.at==*surface && (r.material==Material::air || !rigid_material(*defs[static_cast<std::uint16_t>(r.material)]));});
                        if(surface && surface->rigid() && !releasing)continue;
                        if(w.material_at(q)!=Material::air)continue;
                        const bool reserved=std::any_of(writes.begin(),writes.end(),[&](const auto& r){return !r.at.rigid() && r.at.grid==q && r.material!=Material::air;}) ||
                            std::any_of(requested.begin(),requested.end(),[&](const auto& r){return !r.at.rigid() && r.at.grid==q && r.material!=Material::air;});
                        if(!reserved){destination=q;break;}
                        for(const auto d:directions){CellPos t{q.x+d.x,q.y+d.y};if(std::abs(t.x-origin.x)+std::abs(t.y-origin.y)>2 || std::find(seen.begin(),seen.end(),t)!=seen.end())continue;seen.push_back(t);frontier.push_back(t);}
                    }
                    if(!destination){++w.simulation_stats_.deferred_reactions;return false;}
                    auto released=v.state;const auto& body=w.bodies_[v.at.body];const auto [x,y]=center(v.at);
                    const auto cx=before[v.at.body].x,cy=before[v.at.body].y; // The stored values are local mass center coordinates.
                    const auto mass_x=body.x+std::cos(body.angle)*cx-std::sin(body.angle)*cy,mass_y=body.y+std::sin(body.angle)*cx+std::cos(body.angle)*cy;
                    released.velocity_x=static_cast<int>(std::clamp((body.vx-body.angular_velocity*(y-mass_y))*256.0/60.0,-2048.0,2048.0));
                    released.velocity_y=static_cast<int>(std::clamp((body.vy+body.angular_velocity*(x-mass_x))*256.0/60.0,-2048.0,2048.0));
                    transfers.push_back({{body.pixels[v.at.pixel].local,body.id},{*destination,0}});
                    writes.push_back({Cell{*destination},v.material,released});v.material=Material::air;v.state={};
                }
            }else if(v.material!=Material::air && defs[static_cast<std::uint16_t>(v.material)]->motion()==Motion::rigid){
                // Authored rules currently do not create solid bodies out of grid cells.
                ++w.simulation_stats_.deferred_reactions;return false;
            }
            writes.push_back(v);
        }
        std::size_t planned_total=fragment_total,new_ids=0;std::vector<std::size_t> counts;
        for(const auto i:affected){
            auto pixels=w.bodies_[i].pixels;
            for(const auto& v:writes)if(v.at.rigid() && v.at.body==i)pixels[v.at.pixel].material=static_cast<std::uint16_t>(v.material);
            const auto count=RigidSimulation::connected_parts(pixels).size();counts.push_back(count);planned_total=planned_total-parts[i]+count;
        }
        // Extra IDs are consumed for fragments of each original body, including
        // intermediate splits still present at the end of this material stage.
        for(std::size_t i=0;i<parts.size();++i){const auto found=std::find(affected.begin(),affected.end(),i);const auto count=found==affected.end()?parts[i]:counts[static_cast<std::size_t>(found-affected.begin())];if(count>1)new_ids+=count-1;}
        if(planned_total>64 || new_ids>std::numeric_limits<std::uint32_t>::max()-w.next_body_id_){++w.simulation_stats_.deferred_reactions;return false;}
        for(const auto& v:writes){
            const bool changed=material(v.at)!=v.material;store(v.at,v.material,v.state);
            if(!changed)continue;
            reacted[key(v.at)]=1;
            if(in_thermal)thermal_newborn[key(v.at)]=1;
            if(!v.at.rigid() && !known[key(v.at)]){known[key(v.at)]=1;pending_grid.push_back(v.at);}
        }
        for(std::size_t i=0;i<affected.size();++i)parts[affected[i]]=counts[i];fragment_total=planned_total;
        for(const auto& [from,to]:transfers)ElectricalSimulation::transfer(w,from,to);
        return true;
    }
    bool matches(Cell p,std::string_view pattern)const{
        const auto* d=definition(p);if(!d)return pattern=="air";
        return pattern.size()>2 && pattern.front()=='['?d->has_tag(pattern.substr(1,pattern.size()-2)):d->id==pattern;
    }
    bool matches(Material m,std::string_view pattern)const{
        const auto* d=defs[static_cast<std::uint16_t>(m)];if(!d)return pattern=="air";
        return pattern.size()>2 && pattern.front()=='['?d->has_tag(pattern.substr(1,pattern.size()-2)):d->id==pattern;
    }
    const std::vector<ContactRule>& pair_rules(Material first,Material second){
        const auto a=static_cast<std::uint16_t>(first),b=static_cast<std::uint16_t>(second);
        const auto pair=(static_cast<std::uint32_t>(a)<<16)|b;
        if(const auto found=contact_cache.find(pair);found!=contact_cache.end())return found->second;
        // Bound scratch-cache memory for catalogs with many distinct neighbors.
        auto& result=contact_cache.size()<4096?contact_cache[pair]:uncached_rules;result.clear();
        for(const auto ri:applicable[a]){
            const auto& r=content.reactions()[ri];
            if(r.probability_gate>=0)continue;
            const std::array<std::string_view,3> inputs{r.input_a,r.input_b,r.input_c};
            std::uint8_t am=0,bm=0;const int count=r.input_c.empty()?2:3;
            for(int slot=0;slot<count;++slot){
                if(matches(first,inputs[slot]))am|=static_cast<std::uint8_t>(1<<slot);
                if(matches(second,inputs[slot]))bm|=static_cast<std::uint8_t>(1<<slot);
            }
            bool distinct=false;
            for(int i=0;i<count;++i)for(int j=0;j<count;++j)if(i!=j && (am&(1<<i)) && (bm&(1<<j)))distinct=true;
            if(distinct)result.push_back({ri,am,bm});
        }
        return result;
    }
    Write output(Cell p,const std::string& id)const{
        const auto m=id=="keep"?material(p):id=="air"?Material::air:w.material_index(id);
        const auto value=noise(w.seed_,w.tick_,position(p));
        auto s=m==Material::air?CellState{}:MaterialSimulation::initial_state(*defs[static_cast<std::uint16_t>(m)],static_cast<std::uint32_t>(value^(value>>32)));
        if(m!=Material::air && material(p)!=Material::air && definition(p)->source_gas && defs[static_cast<std::uint16_t>(m)]->source_gas)s.gas_counter=state(p).gas_counter;
        return {p,m,s};
    }
    bool reaction(const ReactionDefinition& r,const std::array<Cell,3>& positions,Cell sampled){
        const int count=r.input_c.empty()?2:3;int temperature=-1000;
        for(int i=0;i<count;++i)temperature=std::max(temperature,state(positions[i]).temperature);
        if(temperature<r.minimum_temperature || temperature>r.maximum_temperature)return false;
        if(w.tick_-state(positions[0]).created_tick<r.minimum_age || noise(w.seed_,w.tick_+19,position(sampled))%r.interval!=0){w.wake(position(sampled));return false;}
        const std::array<const std::string*,3> outputs{&r.output_a,&r.output_b,&r.output_c};
        struct Proposal {Cell at;int slot;double distance;};std::vector<Proposal> plan;
        const std::array<int,3> radii{r.blob_radius_a,r.blob_radius_b,0};
        for(int slot=0;slot<count;++slot){
            const auto origin=positions[slot];const int radius=radii[slot];
            if(!radius){plan.push_back({origin,slot,0});continue;}
            const auto m=material(origin);const auto [ox,oy]=center(origin);
            std::vector<Cell> frontier{origin};std::vector<std::uint8_t> seen(reacted.size(),0);seen[key(origin)]=1;
            for(std::size_t k=0;k<frontier.size();++k){
                const auto p=frontier[k];const auto [x,y]=center(p);plan.push_back({p,slot,(x-ox)*(x-ox)+(y-oy)*(y-oy)});
                for(const auto q:neighbors(p)){
                    const auto qi=key(q);if(seen[qi])continue;seen[qi]=1;
                    const auto [qx,qy]=center(q);if((qx-ox)*(qx-ox)+(qy-oy)*(qy-oy)>radius*radius+1e-8 || reacted[qi] || material(q)!=m)continue;
                    frontier.push_back(q);
                }
            }
        }
        std::sort(plan.begin(),plan.end(),[&](const auto& a,const auto& b){if(key(a.at)!=key(b.at))return key(a.at)<key(b.at);return a.distance!=b.distance?a.distance<b.distance:a.slot<b.slot;});
        std::vector<Write> writes;std::vector<Cell> consumed;
        for(std::size_t i=0;i<plan.size();){const auto p=plan[i].at;writes.push_back(output(p,*outputs[plan[i].slot]));consumed.push_back(p);do{++i;}while(i<plan.size() && plan[i].at==p);}
        if(!commit(writes)){w.wake(position(sampled));return false;}
        for(const auto p:consumed)reacted[key(p)]=1;++w.simulation_stats_.reactions;return true;
    }
    bool source_reaction(const ReactionDefinition& r,const std::array<Cell,3>& positions){
        const auto p=positions[0],q=positions[1];
        const bool young=r.minimum_age && (definition(p)->source_gas?static_cast<std::int64_t>(state(p).gas_counter)<r.minimum_age:w.tick_-state(p).created_tick<r.minimum_age);
        if(young){w.wake(position(p));return false;}
        int temperature=std::max(state(p).temperature,state(q).temperature);
        if(!r.input_c.empty())temperature=std::max(temperature,state(positions[2]).temperature);
        if(temperature<r.minimum_temperature || temperature>r.maximum_temperature)return false;
        std::vector<Write> writes;std::vector<Cell> consumed;std::unordered_map<std::size_t,std::size_t> planned;
        auto current=[&](Cell at){const auto found=planned.find(key(at));return found==planned.end()?material(at):writes[found->second].material;};
        auto add=[&](Cell at,Material target){
            if(reacted[key(at)] && !planned.contains(key(at)))return;
            const Write value=output(at,target==Material::air?std::string("air"):w.material_ids_[static_cast<std::uint16_t>(target)-1]);
            if(const auto found=planned.find(key(at));found!=planned.end())writes[found->second]=value;
            else{planned[key(at)]=writes.size();writes.push_back(value);consumed.push_back(at);}
        };
        const std::array<std::string,3> outputs{r.output_a,r.output_b,r.output_c};
        const std::array<int,3> radii{r.blob_radius_a,r.blob_radius_b,0};const std::array<bool,3> restricted{r.restrict_a,r.restrict_b,true};
        for(int slot=r.input_c.empty()?1:2;slot>=0;--slot){
            const auto anchor=positions[slot];const auto input=material(anchor);Material target=outputs[slot]=="keep"?input:outputs[slot]=="air"?Material::air:w.material_index(outputs[slot]);
            if(!slot && r.destroy_lonely){const auto at=position(p);const auto left=resolve({at.x-1,at.y}),right=resolve({at.x+1,at.y});
                if((!left || material(*left)==Material::air) && (!right || material(*right)==Material::air))target=Material::air;
            }
            if(!radii[slot]){add(anchor,target);continue;}
            source_reaction_blob(position(anchor),radii[slot],restricted[slot],static_cast<int>(input),static_cast<int>(target),
                [&](CellPos at)->std::optional<int>{const auto cell=resolve(at);return cell?std::optional{static_cast<int>(current(*cell))}:std::nullopt;},
                [&](CellPos at){if(const auto cell=resolve(at))add(*cell,target);});
        }
        if(!commit(writes)){w.wake(position(p));return false;}
        for(const auto at:consumed)reacted[key(at)]=1;++w.simulation_stats_.reactions;return true;
    }
    std::vector<std::size_t> missing_requirements(Cell p,std::uint32_t sample)const{
        const auto m=static_cast<std::uint16_t>(material(p));
        std::vector<std::size_t> pending;
        for(const auto ri:applicable[m]){const auto& r=content.reactions()[ri];if(r.required_neighbor && r.probability_gate>=static_cast<int>(sample))pending.push_back(ri);}
        const auto at=position(p);
        for(int direction=0;direction<4;++direction){const auto delta=source_directions[direction];const auto q=resolve({at.x+delta.x,at.y+delta.y});if(!q)continue;
            for(std::size_t i=0;i<pending.size();++i){const auto& r=content.reactions()[pending[i]];
                if(r.direction!=-1 && r.direction!=direction)break;
                if(matches(*q,r.input_b)){pending[i]=pending.back();pending.pop_back();break;}
            }
        }
        return pending;
    }
    bool required_reaction(Cell p,std::uint32_t sample){
        const auto m=static_cast<std::uint16_t>(material(p));if(static_cast<int>(sample)>=source_gates[m][2])return false;
        const auto pending=missing_requirements(p,sample);if(pending.empty())return false;
        const auto at=position(p);
        const auto chosen=*std::max_element(pending.begin(),pending.end(),[&](auto a,auto b){return content.reactions()[a].probability_gate<content.reactions()[b].probability_gate;});
        const auto& r=content.reactions()[chosen];const auto value=output(p,r.output_a);if(value.material==material(p))return false;
        if(!commit({value})){w.wake(at);return false;}reacted[key(p)]=1;++w.simulation_stats_.reactions;return true;
    }
    bool source_contacts(Cell p){
        const auto m=static_cast<std::uint16_t>(material(p));const auto gates=source_gates[m];
        const auto* source=definition(p);
        const bool environment=source->source_phase && ((w.freeze_chance_ && !source->cold_output.empty()) || (w.unfreeze_chance_ && !source->warmth_output.empty()));
        if(!environment && std::all_of(gates.begin(),gates.end(),[](int value){return value<0;}))return false;
        const auto at=position(p);auto seed=noise(w.seed_,w.tick_+137,at);if(p.rigid())seed^=noise(w.bodies_[p.body].id,1,w.bodies_[p.body].pixels[p.pixel].local);
        CombustionRandom random(static_cast<std::uint32_t>(seed^(seed>>32)));
        // Only potentially reactive neighbors keep a source active. This dirty
        // scheduling choice is separate from the recovered random selection.
        bool pending=gates[2]>0 && !missing_requirements(p,0).empty();
        for(const auto q:neighbors(p))for(const auto ri:applicable[m]){const auto& r=content.reactions()[ri];if(r.probability_gate>=0 && !r.required_neighbor && matches(q,r.input_b))pending=true;}
        if(pending)w.wake(at);
        for(int fast=0;fast<2;++fast){const auto sample=random.next()%10001;
            if(!fast && environment){
                w.wake(at);const auto& s=state(p);
                // Project eligibility bridge; original liquid state-byte/float writers remain unknown.
                const bool blocker=source->cell_type=="liquid" && (source->source_liquid
                    ? (s.liquid_direction!=3 || s.liquid_fall!=0)
                    : (s.mobilized || s.velocity_x || s.velocity_y || s.horizontal_fraction || s.vertical_fraction));
                const auto phase=source_phase_candidates(*source,w.freeze_chance_,w.unfreeze_chance_,blocker,random);
                if(phase.freeze || phase.melt){const auto delta=source_directions[random.neighbor()];const auto neighbor=resolve({at.x+delta.x,at.y+delta.y});
                    if(!neighbor)return true;const auto* adjacent=definition(*neighbor);
                    if(phase.freeze && adjacent && adjacent->cell_type=="liquid" && !adjacent->sand)return true;
                    if(!phase.freeze && material(*neighbor)==material(p))return true;
                    if(commit({output(p,phase.freeze?source->cold_output:source->warmth_output)}))++w.simulation_stats_.reactions;
                    return true; // A blocked candidate consumes this ordinary dispatch, without contact fallback.
                }
            }
            if(!fast && required_reaction(p,sample))return true;
            if(static_cast<int>(sample)>gates[fast])continue;
            const int direction=random.neighbor();const auto delta=source_directions[direction];const auto q=resolve({at.x+delta.x,at.y+delta.y});if(!q || *q==p || reacted[key(*q)])continue;
            std::vector<std::size_t> candidates;
            for(const auto ri:applicable[m]){const auto& r=content.reactions()[ri];if(r.probability_gate>=0 && !r.required_neighbor && r.fast==(fast!=0) && matches(*q,r.input_b))candidates.push_back(ri);}
            if(candidates.empty())continue;
            const auto& r=content.reactions()[candidates[sample%candidates.size()]];
            if(static_cast<int>(sample)>r.probability_gate || (r.direction!=-1 && r.direction!=direction))continue;
            std::array<Cell,3> positions{p,*q,{}};
            if(!r.input_c.empty()){
                const auto third_delta=source_directions[random.neighbor()];CellPos third{at.x+third_delta.x,at.y+third_delta.y};
                const auto second=position(*q);if(third==second)third={2*at.x-second.x,2*at.y-second.y};
                const auto c=resolve(third);if(!c || *c==p || *c==*q || reacted[key(*c)] || !matches(*c,r.input_c))continue;positions[2]=*c;
            }
            if(source_reaction(r,positions))return true;
        }
        return false;
    }
    void contacts(){
        for(const auto p:cells){
            if(reacted[key(p)] || material(p)==Material::air)continue;
            bool done=false;
            if(source_contacts(p) || !legacy_contacts[static_cast<std::uint16_t>(material(p))])continue;
            for(const auto q:neighbors(p)){
                if(reacted[key(q)])continue;
                for(const auto& rule:pair_rules(material(p),material(q))){
                    const auto& r=content.reactions()[rule.index];
                    if(r.input_c.empty()){
                        const bool forward=(rule.first&1) && (rule.second&2);
                        done=reaction(r,{forward?p:q,forward?q:p,{}},p);
                    }else{
                        const std::array<std::string_view,3> inputs{r.input_a,r.input_b,r.input_c};
                        for(int a=0;a<3 && !done;++a){if(!(rule.first&(1<<a)))continue;
                            for(int b=0;b<3 && !done;++b){if(b==a || !(rule.second&(1<<b)))continue;
                                std::array<Cell,3> positions{};positions[a]=p;positions[b]=q;const int third=3-a-b;
                                for(const auto c:{p,q}){for(const auto t:neighbors(c)){if(t==p || t==q || reacted[key(t)] || !matches(t,inputs[third]))continue;positions[third]=t;if(reaction(r,positions,p)){done=true;break;}}if(done)break;}
                            }
                        }
                    }
                    if(done)break;
                }
                if(done)break;
            }
        }
    }
    std::optional<Cell> electric_cell(CellPos p,std::uint32_t owner)const{
        if(!owner)return w.bounds_.contains(p)?std::optional{Cell{p}}:std::nullopt;
        if(p.x<-32 || p.x>32 || p.y<-32 || p.y>32)return {};
        for(std::size_t i=0;i<w.bodies_.size();++i)if(w.bodies_[i].id==owner){const auto pixel=local_pixels[i][(p.y+32)*65+p.x+32];if(pixel>=0 && w.bodies_[i].pixels[static_cast<std::size_t>(pixel)].material)return Cell{{},i,static_cast<std::size_t>(pixel)};break;}
        return {};
    }
    ElectricAddress electric_address(Cell p)const{return p.rigid()?ElectricAddress{w.bodies_[p.body].pixels[p.pixel].local,w.bodies_[p.body].id}:ElectricAddress{p.grid,0};}
    void source_electricity(ElectricPulse pulse){
        const auto initial_position=electric_cell(pulse.position,pulse.body);
        const auto at=initial_position?position(*initial_position):pulse.position;
        const auto seed=noise(w.seed_^std::bit_cast<std::uint32_t>(pulse.avg_x),w.tick_+401,at);
        CombustionRandom random(static_cast<std::uint32_t>(seed^(seed>>32)));
        auto conductor=[&](Cell p){const auto* d=definition(p);return d && d->electrical_conductivity;};
        auto recent=[&](Cell p){const auto stamp=state(p).electric_stamp;return stamp!=UINT64_MAX && w.tick_>=stamp && w.tick_-stamp<10;};
        auto mark=[&](Cell p){if(material(p)==Material::air)return;auto st=state(p);st.electric_stamp=w.tick_;st.electrified_ticks=std::max(st.electrified_ticks,pulse.glow_ticks);store(p,material(p),st);w.wake(position(p));};
        auto normalized=[](float x,float y){const float length=std::sqrt(x*x+y*y);return length>0?std::pair{x/length,y/length}:std::pair{0.0f,0.0f};};
        auto ordinary_ignite=[&](Cell p){const auto* d=definition(p);if(!d || !d->source_burning || !d->burnable || state(p).burning)return;
            auto st=state(p);if(random.percent()<static_cast<unsigned>(100-d->ignition)){
                st.burning=true;st.fire_intensity=static_cast<std::uint8_t>(std::clamp(static_cast<int>(pulse.probability_to_heat*100),0,100));st.ignition_pending=true;store(p,material(p),st);static_cast<void>(ignition_conversion(p,st));
            }
        };
        if(pulse.set_fire){if(initial_position && material(*initial_position)!=Material::air){auto st=state(*initial_position);force_combustion(*definition(*initial_position),st);if(st.burning)st.fire_intensity=127;store(*initial_position,material(*initial_position),st);static_cast<void>(ignition_conversion(*initial_position,st));}
            if(--pulse.energy)w.electric_pulses_.push_back(pulse);return;
        }
        if(initial_position)mark(*initial_position); // Project ingress marks its occupied head before searching.
        for(unsigned step=0;step<pulse.speed && pulse.energy;++step){
            const auto current=electric_cell(pulse.position,pulse.body);const CellPos origin=current?position(*current):pulse.position;
            const auto* d=current?definition(*current):nullptr;std::optional<Cell> chosen;float step_x=0,step_y=0;
            const auto [ux,uy]=normalized(pulse.avg_x,pulse.avg_y);
            if(d && d->electrical_conductivity && d->cell_type=="liquid" && !d->sand){
                for(int attempt=0;attempt<17;++attempt){
                    const float theta=static_cast<float>(random.next())/32767.0f*4.71238899230957f-2.356194496154785f;
                    const float cosine=std::cos(theta),sine=std::sin(theta);step_x=2*(ux*cosine-uy*sine);step_y=2*(ux*sine+uy*cosine);
                    const CellPos delta{static_cast<int>(step_x),static_cast<int>(step_y)};const auto q=resolve({origin.x+delta.x,origin.y+delta.y});
                    if(attempt==16){if(!q || material(*q)==Material::air || !conductor(*q))return;chosen=q;break;}
                    if(q && material(*q)!=Material::air && (delta.x || delta.y) && !recent(*q)){chosen=q;break;}
                }
                if(!chosen)return;mark(*chosen);pulse.avg_x+=step_x;pulse.avg_y+=step_y;
            }else{
                for(int radius=1;radius<=4 && !chosen;++radius){float best=-3;
                    for(int y=-radius;y<=radius;++y)for(int x=-radius;x<=radius;++x){
                        const auto q=resolve({origin.x+x,origin.y+y});if(!q || material(*q)==Material::air)continue;
                        if(conductor(*q)){
                            if((!x&&!y) || recent(*q))continue;const float distance=std::sqrt(static_cast<float>(x*x+y*y));const float score=(ux*x+uy*y)/distance-0.5f*distance;
                            if(score>best){best=score;chosen=q;step_x=static_cast<float>(x);step_y=static_cast<float>(y);}
                        }else if(pulse.probability_to_heat>static_cast<float>(random.next())/32767.0f)ordinary_ignite(*q);
                    }
                }
                if(!chosen)return;
                for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){const auto q=resolve({origin.x+x,origin.y+y});if(q && conductor(*q) && !recent(*q))mark(*q);}
                pulse.avg_x=step_x;pulse.avg_y=step_y;
            }
            const auto from=current?electric_address(*current):ElectricAddress{origin,0},to=electric_address(*chosen);
            pulse.previous=from.position;pulse.previous_body=from.body;pulse.position=to.position;pulse.body=to.body;w.wake(position(*chosen));
            if(!--pulse.energy)return;
            if(pulse.splittings_left && pulse.energy==pulse.splitting_energy){
                const auto [vx,vy]=normalized(pulse.avg_x,pulse.avg_y);
                for(int i=0;i<2;++i){auto child=pulse;--child.splittings_left;child.energy/=2;child.splitting_energy/=2;
                    const float angle=static_cast<float>(random.next())/32767.0f*0.2f-0.1f,cosine=std::cos(angle),sine=std::sin(angle);
                    child.avg_x=30*(vx*cosine-vy*sine);child.avg_y=30*(vx*sine+vy*cosine);
                    if(child.energy && w.electric_pulses_.size()<grid_size)w.electric_pulses_.push_back(child);
                }return; // Project ownership: parent ends now, children start next fixed step.
            }
        }
        if(pulse.energy)w.electric_pulses_.push_back(pulse);
    }
    void electricity(std::span<const CellPos> active){
        if(w.electric_pulses_.empty() && !w.electrified_cells_)return;
        auto fade=[&](Cell p){auto s=state(p);if(!s.electrified_ticks)return;--s.electrified_ticks;store(p,material(p),s);};
        // Raw grid cells also fade when temporarily hidden by a physical body.
        for(const auto p:active)fade(Cell{p});
        for(std::size_t i=0;i<w.bodies_.size();++i)for(std::size_t j=0;j<w.bodies_[i].pixels.size();++j)if(w.bodies_[i].pixels[j].material)fade(Cell{{},i,j});
        if(w.electric_pulses_.empty())return;
        const auto initial=w.electric_energy_;std::uint64_t spent=0;std::vector<std::uint8_t> claimed(reacted.size(),0);
        auto conducting=[&](Cell p){const auto* d=definition(p);return d && d->electrical_conductivity;};
        struct Work {ElectricPulse pulse;std::uint16_t steps;};std::vector<Work> work;
        auto original=std::move(w.electric_pulses_);w.electric_pulses_.clear();std::uint64_t source_initial=0;
        for(const auto& pulse:original){if(pulse.source_rules){source_initial+=pulse.energy;source_electricity(pulse);}else{work.push_back({pulse,pulse.speed});if(const auto p=electric_cell(pulse.position,pulse.body))claimed[key(*p)]=1;}}
        std::uint64_t source_remaining=0;for(const auto& pulse:w.electric_pulses_)source_remaining+=pulse.energy;
        if(source_remaining>source_initial)throw std::logic_error("Source electrical branching created energy.");spent+=source_initial-source_remaining;
        for(std::size_t cursor=0;cursor<work.size();++cursor){
            auto item=work[cursor];auto& pulse=item.pulse;const auto resolved=electric_cell(pulse.position,pulse.body);
            if(!resolved || !conducting(*resolved)){spent+=pulse.energy;continue;}const auto p=*resolved;const auto m=material(p);const auto* d=definition(p);
            auto s=state(p);s.electrified_ticks=std::max(s.electrified_ticks,pulse.glow_ticks);store(p,m,s);
            const auto cost=std::min(pulse.energy,static_cast<std::uint32_t>(d->electric_cost));pulse.energy-=cost;spent+=cost;
            const auto adjacent=neighbors(p);
            auto heat=[&](){
                const auto temperature=std::min<std::uint32_t>(pulse.heat,pulse.energy*8);if(!temperature)return;
                const auto energy=(temperature+7)/8;pulse.energy-=energy;spent+=energy;
                auto warm=[&](Cell at){const auto* target=definition(at);if(!target || (at!=p && !target->fuel))return;auto st=state(at);st.temperature=std::max(st.temperature,static_cast<int>(temperature));store(at,material(at),st);};
                warm(p);for(const auto at:adjacent)warm(at);
            };
            if(noise(w.seed_,w.tick_,position(p))%pulse.heat_interval==0)heat();
            const auto previous=electric_cell(pulse.previous,pulse.previous_body);std::vector<Cell> exits;
            for(const auto next:adjacent){if((previous && next==*previous) || !conducting(next) || claimed[key(next)] || state(next).electrified_ticks)continue;exits.push_back(next);}
            if(exits.empty()){heat();spent+=pulse.energy;continue;}
            const auto remainder=pulse.energy%static_cast<std::uint32_t>(exits.size()),share=pulse.energy/static_cast<std::uint32_t>(exits.size());
            for(std::size_t i=0;i<exits.size();++i){
                auto child=pulse;const auto from=electric_address(p),to=electric_address(exits[i]);child.previous=from.position;child.previous_body=from.body;child.position=to.position;child.body=to.body;
                child.energy=share+(i<remainder?1u:0u);if(!child.energy)continue;claimed[key(exits[i])]=1;w.wake(position(exits[i]));
                if(item.steps>1)work.push_back({child,static_cast<std::uint16_t>(item.steps-1)});else w.electric_pulses_.push_back(child);
            }
        }
        w.electric_energy_=0;for(const auto& pulse:w.electric_pulses_)w.electric_energy_+=pulse.energy;
        if(w.electric_energy_+spent!=initial)throw std::logic_error("Electrical pulse energy invariant failed.");
        w.simulation_stats_.electric_spent+=spent;
    }
    bool ignition_conversion(Cell p,CellState& s){
        const auto* d=definition(p);if(!s.ignition_pending)return false;
        s.ignition_pending=false;
        const auto& target=!d->ignition_output.empty()?d->ignition_output:
            (!p.rigid() && d->cell_type=="liquid" && !d->fire_hp?d->flame_output:std::string{});
        if(target.empty() || (target!="air" && w.material_index(target)==material(p))){store(p,material(p),s);return false;}
        if(commit({output(p,target)})){++w.simulation_stats_.reactions;return true;}
        s.ignition_pending=true;store(p,material(p),s);return true;
    }
    void spread_fire(Cell p,const CellState& source,CombustionRandom& random){
        const int intensity=combustion_intensity(source);if(static_cast<int>(random.percent())>intensity)return;
        const auto direction=directions[random.neighbor()];
        // Propagation chooses one world-space cardinal neighbor, including for
        // rotated bodies. The existing resolver owns grid/body addressing.
        const auto at=position(p);const auto q=resolve({at.x+direction.x,at.y+direction.y});
        if(!q || *q==p || material(*q)==Material::air)return;
        const auto* d=definition(*q);if(!d->source_burning)return;auto target=state(*q);
        // Original target-type predicates remain unnamed. Restrict this visual
        // approximation to non-gas/non-fire materials instead of guessing them.
        if(d->cell_type!="gas" && d->cell_type!="fire")target.fire_color=char_color(target.fire_color?target.fire_color:d->color,intensity,random);
        if(d->burnable && !target.burning && (!d->explosion.radius || target.fuel) && random.percent()<static_cast<std::uint32_t>(100-d->ignition)){
            target.burning=true;target.fire_intensity=source.fire_intensity;target.ignition_pending=true;
        }
        store(*q,material(*q),target);static_cast<void>(ignition_conversion(*q,target));
    }
    bool emit_fire(Cell p,const std::string& id,int value,CombustionRandom& random){
        const auto at=position(p);const auto q=resolve({at.x+static_cast<int>(random.next()%3)-1,at.y-1});
        if(id=="air" || !q || q->rigid() || material(*q)!=Material::air)return false;
        auto write=output(*q,id);
        if(defs[static_cast<std::uint16_t>(write.material)]->source_burning)write.state.fire_intensity=static_cast<std::uint8_t>(value);
        return commit({write});
    }
    // Returns true after conversion/removal or a blocked atomic conversion.
    bool source_combustion(Cell p,CellState& s,int temperature,bool burning_before){
        const auto* d=definition(p);
        const auto at=position(p);auto seed=noise(w.seed_,w.tick_,at);
        if(p.rigid())seed^=noise(w.bodies_[p.body].id,0,w.bodies_[p.body].pixels[p.pixel].local);
        CombustionRandom random(static_cast<std::uint32_t>(seed^(seed>>32)));
        // External heat -> ordinary ignition is a project bridge, separate
        // from the source difficulty parameter. Explicit heat tool is forced.
        if(!s.burning && d->burnable && (!d->explosion.radius || s.fuel) && temperature>=100 && random.percent()<static_cast<std::uint32_t>(100-d->ignition)){
            s.burning=true;s.fire_intensity=static_cast<std::uint8_t>(d->burn_heat);s.ignition_pending=true;
        }
        if(ignition_conversion(p,s))return true;
        if(d->cell_type=="fire"){
            // Fire lifetime/movement stay the bounded project model; only the
            // recovered two propagation attempts are adopted here.
            spread_fire(p,s,random);spread_fire(p,s,random);return false;
        }
        if(!s.burning || !burning_before)return false;
        spread_fire(p,s,random);
        bool flame=false;
        if(d->generates_flames>0 && random.percent()<static_cast<std::uint32_t>(d->generates_flames)){
            const int value=static_cast<int>(combustion_intensity(s)*(0.75+0.54*random.next()/32767.0));
            flame=emit_fire(p,d->flame_output,value,random);
        }
        if(d->requires_oxygen){
            s.fire_intensity=flame?static_cast<std::uint8_t>(d->burn_heat):static_cast<std::uint8_t>(s.fire_intensity-1);
            if(combustion_intensity(s)<=0)extinguish_combustion(s);
        }
        if(d->generates_smoke>0 && random.percent()<static_cast<std::uint32_t>(d->generates_smoke)){
            const int value=static_cast<int>(combustion_intensity(s)*(0.75+0.54*random.next()/32767.0));
            static_cast<void>(emit_fire(p,d->smoke_output,value,random));
        }
        s.temperature=std::max(s.temperature,std::max(0,combustion_intensity(s)));
        if(d->explosion.radius){if(s.burning && s.fuel && !s.detonation_ticks)s.detonation_ticks=static_cast<std::uint16_t>(d->explosion.delay_ticks);return false;}
        std::string target;
        if(p.rigid()){
            s.burning_ticks=static_cast<std::uint16_t>(s.burning_ticks+1);
            if(!(s.burning_ticks&7))s.fire_color=fade_burning_color(s.fire_color?s.fire_color:d->color);
            // Mapping the unknown global source counter to fixed tick is a
            // project scheduling choice, not a recovered frame duration.
            if(!(w.tick_&31) && d->fire_hp!=-1 && s.burning_ticks>d->fire_hp)target="air";
        }else if(d->cell_type=="liquid"){
            if(d->fire_hp>=0 && random.next()%(static_cast<std::uint32_t>(d->fire_hp)+1)==0)target=d->flame_output;
        }else if(d->cell_type=="gas" && s.fuel){
            // Gas countdown semantics are not recovered; retain the existing
            // finite project budget instead of claiming the liquid algorithm.
            if(!--s.fuel){s.burning=false;target=d->burn_output;}
        }
        if(target.empty())return false;
        if(target!="air" && w.material_index(target)==material(p)){s.burning=false;return false;}
        if(commit({output(p,target)})){++w.simulation_stats_.reactions;return true;}
        // Keep the material/counter and retry next eligible step; no products
        // or shape changes were partially committed.
        if(d->cell_type=="gas" && !p.rigid() && !s.fuel){s.fuel=1;s.burning=true;}
        store(p,material(p),s);return true;
    }
    void thermal(){
        std::vector<std::uint8_t> burning_before(reacted.size(),0);
        for(const auto p:cells)burning_before[key(p)]=state(p).burning?1:0;
        std::vector<std::int32_t> heat(reacted.size(),0);
        if(w.bodies_.empty() && cells.size()>=grid_size/4){
            // A dense grid needs a snapshot of temperatures, not four repeated
            // spatial queries per cell. Include sleeping chunks for the halo.
            for(const auto& chunk:w.chunks_)for(int y=0;y<chunk_size;++y)for(int x=0;x<chunk_size;++x){
                const CellPos at{chunk.position.x*chunk_size+x,chunk.position.y*chunk_size+y};
                if(w.bounds_.contains(at))heat[key(Cell{at})]=chunk.states[static_cast<std::size_t>(y)*chunk_size+x].temperature;
            }
        }else for(const auto p:cells){if(material(p)==Material::air)continue;heat[key(p)]=state(p).temperature;for(const auto q:neighbors(p))heat[key(q)]=state(q).temperature;}
        for(const auto p:cells){
            const auto m=material(p);if(m==Material::air || thermal_newborn[key(p)])continue;const auto* d=definition(p);auto s=state(p);
            int temperature=heat[key(p)]>0?heat[key(p)]-1:std::min(0,heat[key(p)]+1),cooling=0;bool oxygen=false;
            const auto adjacent=neighbors(p);
            // At ambient temperature, non-burning material without fuel cannot
            // ignite and cooling leaves zero unchanged. Lifetime and phase
            // transitions still execute below; only irrelevant tag queries skip.
            const bool ambient=!d->fuel && !s.burning && !d->heat && !heat[key(p)] &&
                std::all_of(adjacent.begin(),adjacent.end(),[&](Cell q){return heat[key(q)]==0;});
            if(!ambient)for(const auto q:adjacent){
                const auto* other=definition(q);if(!other || other->has_tag("oxidizer"))oxygen=true;if(!other)continue;
                if(heat[key(q)]<temperature)temperature-=std::max(0,(temperature-heat[key(q)])/16);else temperature+=std::max(0,(heat[key(q)]-temperature)/8);
                cooling+=other->cooling;
            }
            if(d->heat)temperature=std::max(temperature,d->heat);if(cooling)temperature=std::min(temperature,std::max(0,temperature-cooling));s.temperature=temperature;
            if(d->source_burning){if(source_combustion(p,s,temperature,burning_before[key(p)]!=0))continue;}
            else {
            if(d->fuel && s.fuel && temperature>=d->ignition && (!d->requires_oxygen || oxygen))s.burning=true;
            if(d->requires_oxygen && !oxygen)s.burning=false;if(cooling>0 && temperature<d->ignition)s.burning=false;
            if(s.burning && d->explosion.radius && s.fuel && !s.detonation_ticks)s.detonation_ticks=static_cast<std::uint16_t>(d->explosion.delay_ticks);
            if(s.burning && !d->explosion.radius){
                --s.fuel;s.temperature=std::max(temperature,d->burn_heat-cooling);
                if(!s.fuel){
                    s.burning=false;const auto product=output(p,d->burn_output);
                    if(product.material==m)store(p,m,s);
                    else if(!commit({product})){s.fuel=state(p).fuel;s.burning=true;store(p,m,s);continue;}
                    ++w.simulation_stats_.reactions;continue;
                }
                const auto at=position(p);
                if(d->smoke_output!="air" && noise(w.seed_,w.tick_+43,at)%12==0){const auto q=resolve({at.x,at.y-1});if(q && !q->rigid() && material(*q)==Material::air)static_cast<void>(commit({output(*q,d->smoke_output)}));}
                if(d->flame_output!="air" && noise(w.seed_,w.tick_,at)%3==0){for(const auto q:adjacent)if(!q.rigid() && material(q)==Material::air){static_cast<void>(commit({output(q,d->flame_output)}));break;}}
            }
            }
            if(!d->source_phase && !d->cold_output.empty() && s.temperature<=d->cold_threshold){if(commit({output(p,d->cold_output)})){++w.simulation_stats_.reactions;continue;}}
            if(!d->source_phase && !d->warmth_output.empty() && s.temperature>=d->warmth_threshold){if(commit({output(p,d->warmth_output)})){++w.simulation_stats_.reactions;continue;}}
            if(d->lifetime){if(!d->source_gas && s.life>0)--s.life;if(d->source_gas?s.gas_counter>=d->lifetime:!s.life){if(commit({output(p,"air")})){++w.simulation_stats_.expired;continue;}s.life=1;}}
            store(p,m,s);if((s.life && !d->source_gas) || s.burning || s.temperature || d->heat || s.detonation_ticks || s.electrified_ticks)w.wake(position(p));
        }
    }
public:
    MaterialInteractionStage(WorldState& world,const ContentRegistry& registry,std::span<const CellPos> active):w(world),content(registry),grid_size(static_cast<std::size_t>(w.bounds_.width)*w.bounds_.height),fragment_total(w.bodies_.size()),before(RigidSimulation::shape_centers(w,content)){
        applicable.emplace_back();
        for(const auto& id:w.material_ids_){defs.push_back(content.find_material(id));applicable.push_back(content.reaction_candidates(id));}
        source_gates.resize(defs.size(),{-1,-1,-1});legacy_contacts.resize(defs.size(),0);
        for(std::size_t m=1;m<defs.size();++m)for(const auto ri:applicable[m]){const auto& r=content.reactions()[ri];if(r.probability_gate<0)legacy_contacts[m]=1;else{auto& gate=source_gates[m][r.required_neighbor?2:r.fast?1:0];gate=std::max(gate,r.probability_gate);}}
        auto size=grid_size;for(const auto& b:w.bodies_){starts.push_back(size);size+=b.pixels.size();parts.push_back(1);local_pixels.emplace_back();local_pixels.back().fill(-1);for(std::size_t j=0;j<b.pixels.size();++j){const auto p=b.pixels[j].local;local_pixels.back()[(p.y+32)*65+p.x+32]=static_cast<int>(j);}}reacted.resize(size,0);known.resize(size,0);thermal_newborn.resize(size,0);
        cells.reserve(active.size()+size-grid_size);
        for(const auto p:active){const auto ref=resolve(p);if(ref && !known[key(*ref)]){known[key(*ref)]=1;cells.push_back(*ref);}}
        for(std::size_t i=0;i<w.bodies_.size();++i)for(std::size_t j=0;j<w.bodies_[i].pixels.size();++j){Cell p{{},i,j};if(!known[key(p)]){known[key(p)]=1;cells.push_back(p);}}
    }
    CommandResult temperature_area(CellPos center,int radius,int strength){
        std::uint64_t changed=0;std::vector<Cell> seen;
        for(int y=-radius;y<=radius;++y)for(int x=-radius;x<=radius;++x){if(x*x+y*y>radius*radius)continue;
            const CellPos at{center.x+x,center.y+y};const auto p=resolve(at);if(!p || material(*p)==Material::air || std::find(seen.begin(),seen.end(),*p)!=seen.end())continue;seen.push_back(*p);
            const auto* d=definition(*p);const auto& target=strength>0?d->warmth_output:d->cold_output;if(target.empty())continue;
            const auto value=noise(w.seed_,w.tick_+211,at);CombustionRandom random(static_cast<std::uint32_t>(value^(value>>32)));
            if(source_phase_gate(random,strength>0?d->warmth_chance_rev:d->cold_chance_rev,std::abs(strength)) && commit({output(*p,target)}))++changed;
        }
        RigidSimulation::finish_material_changes(w,content,before);
        return {strength>0?CommandKind::source_warm:CommandKind::source_cold,true,changed,"Source temperature T="+std::to_string(strength)+" converted "+std::to_string(changed)+" cells (no stored heat)."};
    }
    void run(std::span<const CellPos> active){
        contacts();cells.insert(cells.end(),pending_grid.begin(),pending_grid.end());pending_grid.clear();
        electricity(active);in_thermal=true;thermal();RigidSimulation::finish_material_changes(w,content,before);
    }
    BodyConversionResult consume(std::uint32_t id,std::span<const CellPos> pixels,const std::string& product){
        const auto found=std::find_if(w.bodies_.begin(),w.bodies_.end(),[&](const auto& body){return body.id==id;});
        if(found==w.bodies_.end() || pixels.empty())return {false,"Explosive body/cluster no longer exists.",{}};
        const auto body=static_cast<std::size_t>(found-w.bodies_.begin());std::vector<Write> writes;std::vector<Cell> spent;
        for(const auto local:pixels){
            const auto at=std::find_if(found->pixels.begin(),found->pixels.end(),[&](const auto& p){return p.local==local;});
            if(at==found->pixels.end())return {false,"Explosive local pixel no longer exists.",{}};
            const Cell p{{},body,static_cast<std::size_t>(at-found->pixels.begin())};
            if(std::any_of(writes.begin(),writes.end(),[&](const auto& v){return v.at==p;}))return {false,"Duplicate explosive local pixel.",{}};
            auto write=output(p,product);if(write.material==material(p))spent.push_back(p);writes.push_back(write);
        }
        if(!commit(writes))return {false,"Explosion residue needs free grid space or fragment capacity.",{}};
        for(const auto p:spent){auto s=state(p);s.fuel=0;s.burning=false;s.detonation_ticks=0;store(p,material(p),s);}
        return {true,{},RigidSimulation::finish_material_changes(w,content,before)};
    }
};
CommandResult MaterialInteractions::temperature(WorldState& w,const ContentRegistry& content,CellPos center,int radius,int strength){MaterialInteractionStage stage(w,content,{});return stage.temperature_area(center,radius,strength);}
void MaterialInteractions::step(WorldState& w,const ContentRegistry& content,std::span<const CellPos> active){MaterialInteractionStage stage(w,content,active);stage.run(active);}
BodyConversionResult MaterialInteractions::consume_body_fuel(WorldState& w,const ContentRegistry& content,std::uint32_t body,std::span<const CellPos> pixels,const std::string& output){MaterialInteractionStage stage(w,content,{});return stage.consume(body,pixels,output);}
}
