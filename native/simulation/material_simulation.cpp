#include "simulation/material_simulation.hpp"
#include "simulation/explosion_simulation.hpp"
#include "simulation/electrical_simulation.hpp"
#include "simulation/material_interactions.hpp"
#include "simulation/liquid_simulation.hpp"
#include "simulation/source_liquid_simulation.hpp"
#include "simulation/combustion.hpp"
#include "simulation/source_phase.hpp"
#include "simulation/source_adhesion.hpp"
#include <cmath>
#include <algorithm>
#include <array>

namespace pixel {
namespace {
constexpr CellPos neighbors[]{{0,-1},{-1,0},{1,0},{0,1}};
std::uint64_t noise(std::uint64_t seed, std::uint64_t tick, CellPos p) {
    auto n=seed ^ (tick*0x9e3779b97f4a7c15ULL) ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x))<<32) ^ static_cast<std::uint32_t>(p.y);
    n=(n^(n>>30))*0xbf58476d1ce4e5b9ULL; n=(n^(n>>27))*0x94d049bb133111ebULL;return n^(n>>31);
}
}
CellState MaterialSimulation::initial_state(const MaterialDefinition& d,std::uint32_t random_seed) {
    CellState s{d.initially_burning?std::max(d.heat,d.burn_heat):d.heat,d.lifetime,d.fuel,d.initially_burning};
    if(d.source_burning)s.fire_intensity=static_cast<std::uint8_t>(d.burn_heat);
    if(d.source_gas){CombustionRandom random(random_seed);s.gas_counter=source_gas_initial_counter(d.lifetime,random);}
    return s;
}
void MaterialSimulation::transform(WorldState& w, CellPos p, Material m, CellState state) {
    const auto old=*w.material_at(p);
    if(old==m)return; // Identity outputs preserve lifetime, age and mechanical state.
    // Lab sources/sinks alter the stone budget explicitly; movement and gameplay never do.
    if (old!=m) {
        if (old==Material::stone) --w.initial_stone_;
        if (m==Material::stone) ++w.initial_stone_;
    }
    if(m!=Material::air)state.created_tick=w.tick_;
    w.set_cell(p,m,state);
}
WorldState MaterialSimulation::make_lab(std::uint64_t seed, const ContentRegistry& content, bool liquids_only) {
    WorldState w(seed,{{-64,0},256,128});w.material_lab_=true;
    for (const auto& d:content.materials()) if(d.id!="material.stone") w.material_ids_.push_back(d.id);
    w.counts_.resize(w.material_ids_.size()+1,0);
    auto rect=[&](int x,int y,int width,int height,std::string_view id) {
        const auto m=w.material_index(id);const auto* d=content.find_material(id);
        if (!d) return;
        for(int yy=y;yy<y+height;++yy) for(int xx=x;xx<x+width;++xx) w.set_cell({xx,yy},m,initial_state(*d,static_cast<std::uint32_t>(noise(seed,0,{xx,yy}))));
    };
    // Authored fixtures, entirely outside fixed-step logic. Closed outer boundary.
    rect(-64,0,256,2,"material.stone");rect(-64,126,256,2,"material.stone");
    rect(-64,0,2,128,"material.stone");rect(190,0,2,128,"material.stone");
    if(liquids_only){
        // Three independent containers: falling water, inverted oil/water layers,
        // and an empty basin for painting. No unrelated reaction fixtures.
        auto basin=[&](int x,int width,int top,int bottom){
            rect(x,bottom,width,2,"material.stone");
            rect(x,top,2,bottom-top,"material.stone");
            rect(x+width-2,top,2,bottom-top,"material.stone");
        };
        basin(-56,72,50,116);rect(-46,12,20,18,"material.water");
        basin(24,72,50,116);rect(26,99,68,17,"material.oil");rect(26,87,68,12,"material.water");
        basin(104,76,50,116);
        w.initial_stone_=w.world_stone_;return w;
    }
    // Left: sand shelf and water trough across x=0 / y=64.
    rect(-57,48,54,2,"material.stone");rect(-47,12,30,16,"material.sand");
    rect(-57,78,93,3,"material.stone");rect(-57,54,2,24,"material.stone");rect(34,54,2,24,"material.stone");
    rect(-48,58,27,13,"material.water");rect(0,58,24,6,"material.oil");
    // Centre: burnable bridge over a basin; ignition is an explicit user action.
    rect(46,99,64,3,"material.stone");rect(46,74,2,25,"material.stone");rect(108,74,2,25,"material.stone");
    rect(52,67,50,4,"material.wood");rect(55,84,20,8,"material.oil");
    // Right: ceiling pocket for gases, ice and a static-water sample.
    rect(124,24,52,3,"material.stone");rect(124,24,3,31,"material.stone");rect(173,24,3,31,"material.stone");
    rect(136,49,20,7,"material.smoke");rect(135,95,20,10,"material.ice");
    rect(160,84,20,7,"material.water_static");
    // Additional chemistry fixtures only use definitions present in this catalog.
    if(content.find_material("material.salt")){
        rect(-57,123,34,2,"material.stone");rect(-57,101,2,22,"material.stone");rect(-25,101,2,22,"material.stone");
        rect(-53,112,12,7,"material.water");rect(-39,111,8,4,"material.salt");
    }
    if(content.find_material("material.sludge")){
        rect(-15,123,33,2,"material.stone");rect(-15,103,2,20,"material.stone");rect(16,103,2,20,"material.stone");
        rect(-12,112,10,7,"material.water");rect(2,112,10,7,"material.sludge");
    }
    if(content.find_material("material.cement")){
        rect(24,123,18,2,"material.stone");rect(24,108,2,15,"material.stone");rect(40,108,2,15,"material.stone");
        rect(27,115,5,5,"material.cement");rect(34,114,5,5,"material.water");
    }
    if(content.find_material("material.salt_water")){
        rect(48,123,30,2,"material.stone");rect(48,108,2,15,"material.stone");rect(76,108,2,15,"material.stone");
        rect(53,111,19,6,"material.salt_water");
        // Separate flame pockets supply distinct cardinal brine neighbors;
        // adjacent flames must not occupy the sampled third-input positions.
        for(int x=58;x<=70;x+=4)rect(x,115,1,1,"material.fire");
    }
    if(const auto* rock=content.find_material("material.stone");rock && rock->can_loosen){
        rect(88,123,30,2,"material.stone");rect(92,108,20,3,"material.stone");
    }
    if(content.find_material("material.glue")){
        rect(126,62,50,2,"material.stone");rect(130,64,4,1,"material.glue");
    }
    if(content.find_material("material.slime"))rect(145,64,5,4,"material.slime");
    if(content.find_material("material.ice_slime"))rect(180,95,5,6,"material.ice_slime");
    if(content.find_material("material.explosive_powder")){
        rect(6,30,28,3,"material.stone");rect(6,12,2,18,"material.stone");rect(32,12,2,18,"material.stone");
        rect(16,25,3,2,"material.explosive_powder");rect(25,26,3,2,"material.gunpowder");
    }
    if(content.find_material("material.steel")){
        rect(48,15,72,3,"material.stone");rect(48,14,72,1,"material.steel");
        rect(82,13,1,1,"material.stone");rect(82,14,1,1,"material.oil");
        rect(64,15,1,3,"material.water_static");
    }
    // Empty isolated trough for deliberate liquid/contact experiments.
    rect(130,22,40,2,"material.stone");rect(130,8,2,14,"material.stone");rect(168,8,2,14,"material.stone");
    // Separate empty crucible, below the wire and above the wooden bridge.
    rect(80,58,32,2,"material.stone");rect(80,38,2,20,"material.stone");rect(110,38,2,20,"material.stone");
    rect(114,88,13,2,"material.stone");rect(114,72,2,16,"material.stone");rect(125,72,2,16,"material.stone");
    rect(42,58,32,2,"material.stone"); // Empty rigid-body landing platform.
    w.initial_stone_=w.world_stone_;return w;
}
CommandResult MaterialSimulation::command(WorldState& w,const ContentRegistry& content,const WorldCommand& c) {
    if(!w.material_lab_)return {c.kind,false,0,"Material tools require the material laboratory."};
    if(c.kind<CommandKind::paint || c.kind>CommandKind::environment_hot)return {c.kind,false,0,"Unknown material command."};
    if(c.radius<1 || c.radius>8 || !w.bounds_.contains(c.target))return {c.kind,false,0,"Brush radius must be 1..8 and target inside world."};
    if(c.kind>=CommandKind::environment_normal){
        w.freeze_chance_=c.kind==CommandKind::environment_cold?5:0;w.unfreeze_chance_=c.kind==CommandKind::environment_hot?12:0;
        for(auto& chunk:w.chunks_)chunk.awake=true;
        return {c.kind,true,0,"Environment freeze="+std::to_string(w.freeze_chance_)+" / unfreeze="+std::to_string(w.unfreeze_chance_)};
    }
    if(c.kind==CommandKind::source_warm || c.kind==CommandKind::source_cold)return MaterialInteractions::temperature(w,content,c.target,c.radius,c.kind==CommandKind::source_warm?10:-100);
    if(c.kind==CommandKind::electrify)return ElectricalSimulation::inject(w,content,c);
    if(c.kind==CommandKind::blast){
        const auto* profile=content.find_material(c.material_id);
        if(!c.material_id.empty() && !profile)return {c.kind,false,0,"Unknown explosion profile: "+c.material_id};
        auto blast=profile && profile->explosion.radius?profile->explosion:ExplosionDefinition{};blast.radius=c.radius*4;if(profile)blast.source_rules=profile->explosion.source_rules;
        const auto result=ExplosionSimulation::apply(w,content,c.target,blast);
        return {c.kind,result.ok,result.affected,result.ok?"Explosion affected "+std::to_string(result.affected)+" cells; no inventory reward.":result.error};
    }
    const auto* definition=content.find_material(c.material_id);
    const auto selected=w.material_index(c.material_id);
    if(c.kind==CommandKind::paint && (!definition || selected==Material::air))return {c.kind,false,0,"Unknown brush material: "+c.material_id};
    if(c.kind==CommandKind::paint && definition->cell_type=="solid")return {c.kind,false,0,"Solid materials must be created through the session's rigid solver."};
    std::uint64_t changed=0;
    for(int y=-c.radius;y<=c.radius;++y)for(int x=-c.radius;x<=c.radius;++x){
        if(x*x+y*y>c.radius*c.radius)continue;
        const CellPos p{c.target.x+x,c.target.y+y};const auto m=w.material_at(p);if(!m)continue;
        if(w.body_pixel_at(p))continue;
        auto state=*w.cell_state(p);
        if(c.kind==CommandKind::paint && *m==Material::air){transform(w,p,selected,initial_state(*definition,static_cast<std::uint32_t>(noise(w.seed_,w.tick_,p))));++changed;}
        else if(c.kind==CommandKind::erase && *m!=Material::air){transform(w,p,Material::air,{});++changed;}
        else if((c.kind==CommandKind::heat || c.kind==CommandKind::cool) && *m!=Material::air){
            state.temperature=c.kind==CommandKind::heat?120:-30;
            if(c.kind==CommandKind::cool)extinguish_combustion(state);else force_combustion(*content.find_material(w.material_ids_[static_cast<std::uint16_t>(*m)-1]),state);
            w.set_cell(p,*m,state);++changed;
        }else if(c.kind==CommandKind::push && *m!=Material::air){
            const auto* d=content.find_material(w.material_ids_[static_cast<std::uint16_t>(*m)-1]);
            if(d->motion(state.mobilized)!=Motion::fixed || d->can_loosen){
                const int length=std::max(1,std::max(std::abs(x),std::abs(y)));
                if(d->can_loosen){state.mobilized=true;state.quiet_ticks=0;}
                state.velocity_x=x*1536/length;state.velocity_y=y*1536/length;
                state.source_attachment=static_cast<std::uint8_t>(d->source_adhesion && d->liquid_sticks_to_ceiling>0);
                w.set_cell(p,*m,state);++changed;
            }
        }else if(c.kind==CommandKind::loosen){
            const auto* d=*m==Material::air?nullptr:content.find_material(w.material_ids_[static_cast<std::uint16_t>(*m)-1]);
            if(d && d->can_loosen){
                state.mobilized=true;state.quiet_ticks=0;
                w.set_cell(p,*m,state);++changed;
            }
        }
    }
    return {c.kind,true,changed,"Lab command changed "+std::to_string(changed)+" cells."};
}
void MaterialSimulation::step(WorldState& w,const ContentRegistry& content){
    if(w.material_ids_.size()==1 && w.electric_pulses_.empty() && !w.electrified_cells_ && w.bodies_.empty())return;
    auto& stats=w.simulation_stats_;
    ExplosionSimulation::advance(w,content);
    for(const auto& body:w.bodies_)for(const auto& pixel:body.pixels)
        if(!body.sleeping || pixel.state.temperature || pixel.state.burning)
            w.wake({static_cast<int>(std::floor(body.x+std::cos(body.angle)*pixel.local.x-std::sin(body.angle)*pixel.local.y)),static_cast<int>(std::floor(body.y+std::sin(body.angle)*pixel.local.x+std::cos(body.angle)*pixel.local.y))});
    std::vector<const MaterialDefinition*> defs{nullptr};
    for(const auto& id:w.material_ids_)defs.push_back(content.find_material(id));
    // Material categories are immutable; resolve their string-based definition
    // once per step instead of at every transport/flow-search neighbor query.
    std::vector<std::array<Motion,2>> motions{{Motion::fixed,Motion::fixed}};
    for(std::size_t i=1;i<defs.size();++i)motions.push_back({defs[i]->motion(false),defs[i]->motion(true)});
    auto motion=[&](Material m,bool mobilized=false){return motions[static_cast<std::uint16_t>(m)][mobilized?1:0];};
    const auto& b=w.bounds_;
    const auto total=static_cast<std::size_t>(b.width)*b.height;
    std::vector<std::uint8_t> updated(total,0),transported(total,0),attached(total,0);
    auto index=[&](CellPos p){return static_cast<std::size_t>(p.y-b.origin.y)*b.width+static_cast<std::size_t>(p.x-b.origin.x);};
    auto def=[&](Material m){return defs[static_cast<std::uint16_t>(m)];};
    std::vector<CellPos> candidates;candidates.reserve(total);
    std::vector<bool> awake;
    for(auto& chunk:w.chunks_){awake.push_back(chunk.awake);chunk.awake=false;}
    const auto first=cell_to_chunk(b.origin),last=cell_to_chunk({b.origin.x+b.width-1,b.origin.y+b.height-1});
    // A remote outlet can affect a liquid anywhere along this finite row.
    // Expand only the input mask: no recursive or permanent wake propagation.
    bool has_liquid=false;
    for(std::size_t m=1;m<defs.size();++m)if(w.counts_[m] && motions[m][0]==Motion::liquid && !defs[m]->source_liquid){has_liquid=true;break;}
    if(has_liquid){
        const auto changed=awake;
        for(int cy=first.y;cy<=last.y;++cy){
            bool row_changed=false;for(int cx=first.x;cx<=last.x;++cx)row_changed=row_changed || changed[w.chunk_index({cx,cy})];
            if(row_changed)for(int cx=first.x;cx<=last.x;++cx)awake[w.chunk_index({cx,cy})]=true;
        }
    }
    for(const bool active:awake)if(active)++stats.active_chunks;
    // Construct global traversal directly; no per-tick sorting or whole-world material copy.
    for(int cy=last.y;cy>=first.y;--cy)for(int ly=chunk_size-1;ly>=0;--ly)
        for(int cx=first.x;cx<=last.x;++cx){
            const int xx=(w.tick_&1)?last.x-(cx-first.x):cx;
            if(!awake[w.chunk_index({xx,cy})])continue;
            for(int lx=0;lx<chunk_size;++lx){
                const int local_x=(w.tick_&1)?chunk_size-1-lx:lx;
                CellPos p{xx*chunk_size+local_x,cy*chunk_size+ly};if(b.contains(p))candidates.push_back(p);
            }
        }
    stats.visited_cells=static_cast<std::uint32_t>(candidates.size());
    auto ceiling_support=[&](CellPos p,Material material,const MaterialDefinition& d){
        const int span=d.slime?d.adhesion_span:1;
        for(int height=1;height<=span;++height){
            const CellPos above{p.x,p.y-height};const auto m=w.material_at(above);
            if(!m || *m==Material::air)return false;
            const auto* support=def(*m);const auto& s=*w.cell_state(above);
            if(motion(*m,s.mobilized)==Motion::fixed && support->sand)return true;
            if(*m!=material || s.adhesion_cooldown || std::abs(s.velocity_x)+std::abs(s.velocity_y)>d.adhesion_strength_q8)return false;
        }
        return false;
    };
    // Adhesion timing uses fixed ticks, independent of viscosity/movement sampling.
    // Support is bounded and must terminate at a fixed material: floating chains cannot support themselves.
    for(const auto p:candidates){
        const auto m=*w.material_at(p);if(m==Material::air)continue;
        const auto* d=def(m);
        if(d->source_adhesion && !d->source_liquid){
            if(d->is_static || d->liquid_sticks_to_ceiling<=0)continue;
            auto state=*w.cell_state(p);
            state.adhesion_ticks=state.adhesion_cooldown=0; // Superseded project timers in older saves.
            // Source acquisition requires a sand-class support in state 1;
            // static cells and attached sand are the currently represented cases.
            auto support=[&](CellPos q,bool acquisition){
                const auto qm=w.surface_material_at(q);if(!qm || *qm==Material::air)return false;
                const auto* qd=def(*qm);const auto* qs=w.surface_cell_state(q);
                return qd->cell_type=="liquid" && (!acquisition || qd->sand) &&
                    ((qd->is_static && !qs->mobilized) || qs->source_attachment>=5);
            };
            // State 1 is a project handoff: continuous transport may need several
            // ticks to leave this pixel. Reacquiring now would reset that motion.
            if(state.source_attachment>=5){
                const auto below=w.surface_material_at({p.x,p.y+1});
                const auto value=noise(w.seed_^0x4144484553494f4eULL,w.tick_,p);
                CombustionRandom random(static_cast<std::uint32_t>(value^(value>>32)));
                if(source_attachment_releases(random,d->liquid_sticks_to_ceiling,
                       support(source_attachment_support(p,state.source_attachment),false),below && *below==Material::air))state.source_attachment=1;
            }else if(!state.source_attachment){
                const auto direction=source_attachment_direction(p);
                if(support(source_attachment_support(p,direction),true))state.source_attachment=direction;
            }
            if(state.source_attachment>=5){
                attached[index(p)]=1;state.velocity_x=state.velocity_y=0;
                state.vertical_fraction=state.horizontal_fraction=0;
                state.momentum_remainder_x=state.momentum_remainder_y=0;
                state.liquid_fraction_x=state.liquid_fraction_y=0;
                w.wake(p); // Random release remains eligible while held.
            }
            w.set_cell(p,m,state);continue;
        }
        if(!d->adhesion_strength_q8)continue;
        auto state=*w.cell_state(p);
        if(state.adhesion_cooldown){--state.adhesion_cooldown;state.adhesion_ticks=0;}
        else if(std::abs(state.velocity_x)+std::abs(state.velocity_y)<=d->adhesion_strength_q8 && ceiling_support(p,m,*d)){
            if(d->adhesion_hold_ticks && state.adhesion_ticks+1>=d->adhesion_hold_ticks){
                state.adhesion_ticks=0;state.adhesion_cooldown=16;
            }else{
                if(d->adhesion_hold_ticks)++state.adhesion_ticks;
                state.velocity_x=state.velocity_y=0;state.vertical_fraction=state.horizontal_fraction=0;attached[index(p)]=1;
                state.momentum_remainder_x=state.momentum_remainder_y=0;state.liquid_fraction_x=state.liquid_fraction_y=0;
            }
        }else state.adhesion_ticks=0;
        w.set_cell(p,m,state);
    }
    SourceLiquidSimulation::step(w,content,candidates,attached,updated,transported);
    LiquidSimulation::step(w,content,candidates,attached,updated,transported);
    // Canonical global traversal, independent of chunk wake/list order. Alternate x to avoid a persistent bias.
    auto can_enter = [&](Material m, CellPos q, bool upward, bool horizontal) {
        const auto other = w.surface_material_at(q);
        if (!other) return false;
        if (*other == Material::air) return true;
        const auto* destination = def(*other);
        const auto destination_motion=motion(*other,w.surface_cell_state(q)->mobilized);
        if (destination_motion == Motion::fixed || destination_motion == Motion::rigid || destination_motion == Motion::powder || attached[index(q)]) return false;
        const auto* source = def(m);
        if (horizontal) return !upward && (destination_motion == Motion::gas || destination_motion == Motion::fire);
        if (upward) return (destination_motion == Motion::gas || destination_motion == Motion::fire) && destination->density > source->density;
        return destination_motion == Motion::gas || destination_motion == Motion::fire || destination->density < source->density;
    };
    auto flow_passable=[&](Material m,CellPos p){
        if(can_enter(m,p,false,true))return true;
        const auto at=w.surface_material_at(p);
        return at && *at==m && !attached[index(p)];
    };
    std::vector<std::uint64_t> flow_revisions(b.height,0);
    struct FlowRow {
        Material material=Material::air;int y=0;
        std::uint64_t current=0,below=0,used=0;
        std::vector<int> left,right;std::vector<std::uint8_t> flags;
    };
    // Bounded derived metadata, not another material world. Entries are lazy
    // and live only for this step; local probes handle the common short paths.
    std::vector<FlowRow> flow_rows;flow_rows.reserve(16);std::uint64_t flow_clock=0;
    auto row_flow_direction=[&](Material m,CellPos p,int preferred){
        FlowRow* row=nullptr;
        for(auto& entry:flow_rows)if(entry.material==m && entry.y==p.y){row=&entry;break;}
        if(!row){
            if(flow_rows.size()<16){flow_rows.emplace_back();row=&flow_rows.back();}
            else row=&*std::min_element(flow_rows.begin(),flow_rows.end(),[](const auto& a,const auto& c){return a.used<c.used;});
        }
        const int y=p.y-b.origin.y,infinity=b.width+1;
        const auto below=y+1<b.height?flow_revisions[y+1]:0;
        if(row->material!=m || row->y!=p.y || row->current!=flow_revisions[y] || row->below!=below){
            row->material=m;row->y=p.y;row->current=flow_revisions[y];row->below=below;
            row->left.resize(b.width);row->right.resize(b.width);row->flags.resize(b.width);
            for(int x=0;x<b.width;++x){
                const CellPos at{b.origin.x+x,p.y};
                row->flags[x]=flow_passable(m,at)?(can_enter(m,{at.x,at.y+1},false,false)?2:1):0;
                row->left[x]=!row->flags[x]?infinity:row->flags[x]==2?0:(x>0 && row->left[x-1]<infinity?row->left[x-1]+1:infinity);
            }
            for(int x=b.width-1;x>=0;--x)
                row->right[x]=!row->flags[x]?infinity:row->flags[x]==2?0:(x+1<b.width && row->right[x+1]<infinity?row->right[x+1]+1:infinity);
        }
        row->used=++flow_clock;
        const int x=p.x-b.origin.x;
        const int left=x>0 && row->left[x-1]<infinity?row->left[x-1]+1:infinity;
        const int right=x+1<b.width && row->right[x+1]<infinity?row->right[x+1]+1:infinity;
        if(left==infinity && right==infinity)return 0;
        return left==right?preferred:left<right?-1:1;
    };
    auto move = [&](CellPos p, CellPos q, CellState& travelling, bool upward, bool horizontal) {
        const auto material = *w.material_at(p);
        if (!can_enter(material,q,upward,horizontal) || updated[index(q)]) return false;
        const auto other = *w.material_at(q);
        auto displaced = *w.cell_state(q);
        travelling.quiet_ticks=0;travelling.adhesion_ticks=0;displaced.quiet_ticks=0;displaced.adhesion_ticks=0;
        travelling.source_attachment=displaced.source_attachment=0;
        if(def(material)->source_gas && def(material)->lifetime)++travelling.gas_counter;
        if(other!=Material::air && def(other)->source_gas && def(other)->lifetime)++displaced.gas_counter;
        w.set_cell(p,other,displaced);
        w.set_cell(q,material,travelling);
        ++flow_revisions[p.y-b.origin.y];++flow_revisions[q.y-b.origin.y];
        updated[index(p)] = other == Material::air ? 0 : 1;
        updated[index(q)] = 1;
        transported[index(p)]=other==Material::air?0:1;transported[index(q)]=1;
        ++stats.moved_cells;
        return true;
    };
    // Each particle is integrated once. Substeps check every intermediate cell,
    // so accelerated particles cannot tunnel through a one-cell wall.
    for (int pass=0; pass<2; ++pass) {
        for (std::size_t k=0; k<candidates.size(); ++k) {
            auto p = candidates[pass==0 ? k : candidates.size()-1-k];
            if (updated[index(p)]) continue;
            const auto material = *w.material_at(p);
            if (material == Material::air) continue;
            const auto* d = def(material);
            const auto category = motion(material,w.cell_state(p)->mobilized);
            const bool up = category==Motion::gas || category==Motion::fire;
            if (d->source_liquid || category==Motion::fixed || category==Motion::rigid || attached[index(p)] || up!=(pass==1)) continue;
            if (w.tick_%static_cast<std::uint64_t>(d->flow_interval)!=0) { w.wake(p); continue; }
            auto state = *w.cell_state(p);
            const int dy = up ? -1 : 1;
            const int preferred = (noise(w.seed_,w.tick_,p)&1) ? 1 : -1;
            if(category==Motion::liquid){
                const auto y=std::clamp(static_cast<std::int32_t>(std::llround(state.velocity_y*256+state.momentum_remainder_y+liquid_gravity(*d)*65536)),-d->terminal_speed_q8*256,d->terminal_speed_q8*256);
                state.velocity_y=y/256;state.momentum_remainder_y=static_cast<std::int16_t>(y%256);
            }else{
                state.velocity_x = state.velocity_x*d->damping_milli/1000;
                if (std::abs(state.velocity_x)<8) state.velocity_x=0;
                state.velocity_y = up
                ? std::clamp(state.velocity_y-d->gravity_q8,-d->gas_upward_speed_q8,d->gas_downward_speed_q8)
                : std::clamp(state.velocity_y+d->gravity_q8,-d->terminal_speed_q8,d->terminal_speed_q8);
            }
            const int credit = std::abs(state.velocity_y)+state.vertical_fraction;
            int vertical_steps = credit/256;
            state.vertical_fraction = static_cast<std::uint16_t>(credit%256);
            if(category==Motion::liquid){const auto fine_credit=static_cast<std::uint64_t>(std::abs(state.velocity_y*256+state.momentum_remainder_y))*256+state.liquid_fraction_y;
                vertical_steps=static_cast<int>(fine_credit/(1u<<24));state.liquid_fraction_y=static_cast<std::uint32_t>(fine_credit%(1u<<24));state.vertical_fraction=0;}
            // Fractional gravity must also settle. Otherwise a supported liquid
            // alternates between a fractional velocity and zero forever.
            bool blocked = (up && d->gas_upward_speed_q8==0 && state.velocity_y==0) || (vertical_steps==0 && state.velocity_y*dy>=0 &&
                !can_enter(material,{p.x,p.y+dy},up,false) &&
                !can_enter(material,{p.x-1,p.y+dy},up,false) &&
                !can_enter(material,{p.x+1,p.y+dy},up,false));
            for (int n=0; n<vertical_steps; ++n) {
                const CellPos next{p.x,p.y+(state.velocity_y<0 ? -1 : 1)};
                const bool travelling_up = state.velocity_y<0;
                if (!move(p,next,state,travelling_up,false)) { blocked=true; break; }
                p=next;
            }
            if (blocked) {
                state.velocity_y=0; state.vertical_fraction=0;
                if(category==Motion::liquid){state.momentum_remainder_y=0;state.liquid_fraction_y=0;}
                bool rolled=false;
                for (const int dx : {preferred,-preferred}) {
                    if(!vertical_steps)break;
                    const CellPos next{p.x+dx,p.y+dy};
                    if (move(p,next,state,up,false)) { p=next; rolled=true; break; }
                }
                if (rolled && category==Motion::powder) { w.set_cell(p,material,state); continue; }
            }
            int horizontal_direction=0;
            int horizontal_speed=0;
            if (state.velocity_x || (category==Motion::liquid && state.momentum_remainder_x)) {
                horizontal_direction=state.velocity_x>0 ? 1 : -1;
                if(!state.velocity_x)horizontal_direction=state.momentum_remainder_x>0?1:-1;
                horizontal_speed=std::abs(state.velocity_x);
            } else if (blocked && (category==Motion::gas || category==Motion::fire)) {
                horizontal_direction=preferred; horizontal_speed=d->gas_horizontal_speed_q8;
            } else if (blocked && category==Motion::liquid) {
                bool joined=false;
                for(const int dx:{-1,1}){
                    const CellPos next{p.x+dx,p.y};const auto at=w.surface_material_at(next);
                    if(at && *at==material && !attached[index(next)])joined=true;
                }
                // A parcel may follow a lower outlet through its own liquid
                // column. Actual moves still require an enterable destination;
                // this never swaps identical parcels or teleports material.
                if(joined)horizontal_direction=row_flow_direction(material,p,preferred);
                else{
                    int best=d->flow_search+1;bool beyond_local=false;
                    for (const int direction : {preferred,-preferred}) {
                        for (int distance=1; distance<=d->flow_search; ++distance) {
                            const CellPos next{p.x+direction*distance,p.y};
                            if (!flow_passable(material,next)) break;
                            if (can_enter(material,{next.x,next.y+1},false,false)) {
                                if (distance<best) { best=distance; horizontal_direction=direction; }
                                break;
                            }
                            if(distance==d->flow_search)beyond_local=true;
                        }
                    }
                    if(!horizontal_direction && beyond_local)horizontal_direction=row_flow_direction(material,p,preferred);
                }
                if (horizontal_direction) horizontal_speed=d->lateral_speed_q8;
            }
            if (category!=Motion::powder || state.velocity_x) {
                int side_credit=horizontal_speed+state.horizontal_fraction;
                state.horizontal_fraction=horizontal_speed ? static_cast<std::uint16_t>(side_credit%256) : 0;
                if(category==Motion::liquid){const auto speed=state.velocity_x || state.momentum_remainder_x?std::abs(state.velocity_x*256+state.momentum_remainder_x):horizontal_speed*256;
                    const auto fine_credit=static_cast<std::uint64_t>(speed)*256+state.liquid_fraction_x;side_credit=static_cast<int>(fine_credit/(1u<<24))*256;state.liquid_fraction_x=horizontal_direction?static_cast<std::uint32_t>(fine_credit%(1u<<24)):0;state.horizontal_fraction=0;}
                for (int n=0; n<side_credit/256; ++n) {
                    const CellPos next{p.x+horizontal_direction,p.y};
                    if (!horizontal_direction || !move(p,next,state,up,true)) {
                        state.velocity_x=0; state.horizontal_fraction=0;
                        if(category==Motion::liquid){state.momentum_remainder_x=0;state.liquid_fraction_x=0;}
                        break;
                    }
                    p=next;
                }
            }
            w.set_cell(p,material,state);
        }
    }
    // Settling changes mechanical phase, never material identity or resources.
    for(const auto p:candidates){
        const auto m=*w.material_at(p);if(m==Material::air)continue;
        auto state=*w.cell_state(p);if(!state.mobilized)continue;
        const auto* d=def(m);
        const bool quiet=!transported[index(p)] && !state.velocity_x && !state.velocity_y &&
            !can_enter(m,{p.x,p.y+1},false,false) && !can_enter(m,{p.x-1,p.y+1},false,false) && !can_enter(m,{p.x+1,p.y+1},false,false);
        state.quiet_ticks=quiet?static_cast<std::uint16_t>(state.quiet_ticks+1):0;
        if(state.quiet_ticks>=d->settle_ticks){state.mobilized=false;state.quiet_ticks=0;state.vertical_fraction=state.horizontal_fraction=0;}
        w.set_cell(p,m,state);
    }
    MaterialInteractions::step(w,content,candidates);
}
}
