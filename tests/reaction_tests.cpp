#include "content/reaction_table.hpp"
#include "session/game_session.hpp"
#include "simulation/material_simulation.hpp"
#include "simulation/reaction_geometry.hpp"
#include "simulation/source_phase.hpp"
#include "simulation/source_blast.hpp"
#include "simulation/source_adhesion.hpp"
#include "persistence/world_file.hpp"
#include <filesystem>
#include <cmath>
#include <limits>
#include <array>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include "benchmark_material_catalog.hpp"
using namespace pixel;
namespace {
void need(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
std::vector<MaterialDefinition> definitions(){
    std::vector<MaterialDefinition> result;
    for(const auto* name:{"stone","a","b","c","d","e","product_a","product_b","solid"}){
        MaterialDefinition d;d.id=std::string("material.")+name;d.display_name=name;d.source_id=name;result.push_back(d);
    }
    result[1].tags={"same","first"};result[2].tags={"same","second"};result[6].source_id="a_molten";result[7].source_id="b_molten";
    result[8].cell_type="solid";result[8].is_static=false;result[8].sand=false;return result;
}
std::shared_ptr<const ContentRegistry> catalog(std::string_view text){auto parsed=parse_reaction_table(text);need(bool(parsed),parsed.error);auto built=ContentRegistry::build(definitions(),parsed.reactions);need(bool(built),built.error);return built.registry;}
WorldSnapshot blank(const ContentRegistry& c,std::uint64_t seed=26){
    auto s=MaterialSimulation::make_lab(seed,c).snapshot();s.bounds={{-16,48},40,40};s.cells.assign(1600,1);s.states.assign(1600,{});s.initial_stone=1600;return s;
}
std::size_t index(const WorldSnapshot& s,CellPos p){return static_cast<std::size_t>(p.y-s.bounds.origin.y)*s.bounds.width+p.x-s.bounds.origin.x;}
void put(WorldSnapshot& s,CellPos p,std::uint16_t value){auto& old=s.cells[index(s,p)];if(old==1)--s.initial_stone;if(value==1)++s.initial_stone;old=value;s.states[index(s,p)]={};}
void restore(GameSession& s,const WorldSnapshot& snapshot){const auto result=s.restore_world(snapshot);need(result.ok,result.error);need(bool(s.resume()),"resume");}
void tick(GameSession& s,int n=1){for(int i=0;i<n;++i)need(s.advance_fixed_step(),"fixed step");}
int at(const GameSession& s,CellPos p){return static_cast<int>(*s.world()->material_at(p));}
WorldSnapshot pair(const ContentRegistry& c,std::uint64_t seed=26){auto s=blank(c,seed);put(s,{-1,63},2);put(s,{0,63},3);return s;}
void labels(const std::string& game){
    auto c=catalog("[same];[same];[same]_molten;keep;1;probability=20.129;fast=1");
    need(c->reactions().size()==2,"one source tag binds all occurrences together instead of a Cartesian product of identical labels");
    for(const auto& r:c->reactions())need(r.input_a==r.input_b && r.probability_gate==2012 && r.fast && (r.output_a=="material.product_a" || r.output_a=="material.product_b"),"global label substitution resolves existing source-name suffixes into project IDs");
    c=catalog("[first];[second];[first];[second];1;probability=100");need(c->reactions().size()==1 && c->reactions()[0].output_a=="material.a" && c->reactions()[0].output_b=="material.b","different labels recursively bind their own members across slots");
    need(catalog("[absent];material.b;air;keep;1;probability=100")->reactions().empty(),"empty source tag list registers no material pair");
    auto bad=parse_reaction_table("[same];material.b;[same]_unavailable;keep;1;probability=100");need(!ContentRegistry::build(definitions(),bad.reactions),"unknown suffixed output is rejected instead of inventing a material");
    for(const auto* text:{"a;b;air;air;1;probability=nan","a;b;air;air;1;probability=100.01","a;b;air;air;1;probability=2;probability=3","a;b;air;air;1;probability=2;direction=diagonal","a;b;air;air;1;fast=1"})need(!parse_reaction_table(text),"parser rejects invalid source options");
    std::ifstream file(game+"/content/reactions.txt");std::ostringstream source;source<<file.rdbuf();auto parsed=parse_reaction_table(source.str());need(bool(parsed),parsed.error);
    auto actual=ContentRegistry::build(benchmark_materials(),parsed.reactions);need(bool(actual),actual.error);
    const auto count=std::count_if(actual.registry->reactions().begin(),actual.registry->reactions().end(),[](const auto& r){return r.probability_gate>=0;});
    need(count>46,"actual curated source records expand into concrete runtime pairs");std::cout<<"actual records="<<actual.registry->reactions().size()<<" source records="<<count<<'\n';
    GameSession lab(actual.registry);need(bool(lab.create_material_lab(20261027)),"create actual source laboratory");
    int contacts=0;std::uint64_t peak_steam=0;
    for(int t=0;t<20;++t){
        for(int y=108;y<123;++y)for(int x=50;x<76;++x)if(lab.world()->material_at({x,y})==lab.world()->material_index("material.fire")){
            int brine=0;for(const auto delta:std::array<CellPos,4>{{{0,-1},{0,1},{-1,0},{1,0}}})brine+=lab.world()->material_at({x+delta.x,y+delta.y})==lab.world()->material_index("material.salt_water");if(brine>=2)++contacts;
        }
        tick(lab);peak_steam=std::max(peak_steam,lab.world()->material_count(lab.world()->material_index("material.steam")));
    }
    int salt=0,steam=0;for(int y=88;y<123;++y)for(int x=50;x<76;++x){salt+=lab.world()->material_at({x,y})==lab.world()->material_index("material.salt");steam+=lab.world()->material_at({x,y})==lab.world()->material_index("material.steam");}
    std::cout<<"authored brine fixture eligible contacts="<<contacts<<" salt="<<salt<<" steam="<<steam<<" peak_global="<<peak_steam<<'\n';
    need(salt>0 && peak_steam>0,"authored brine scene produces salt and steam; moving steam may later condense or leave the sample region");
}
void sampling(){
    const auto c=catalog("material.a;material.b;material.c;keep;1;probability=0\nmaterial.a;material.b;material.d;keep;1;probability=100");
    // Fixed project seeds at (-1,63), tick 0: seed26 -> p1746/right,
    // seed9 -> p5871/right, seed21649 -> p0/right. Both candidates share p.
    GameSession s(c);restore(s,pair(*c,26));tick(s);need(at(s,{-1,63})==2,"even sample selects gate-zero candidate and must not fall through to the high-gate candidate");
    restore(s,pair(*c,9));tick(s);need(at(s,{-1,63})==5,"odd sample selects the other candidate using the same probability value");
    restore(s,pair(*c,21649));tick(s);need(at(s,{-1,63})==4,"probability zero retains the recovered inclusive p==0 case");
    auto directional=catalog("material.a;material.b;material.c;keep;1;probability=100;direction=top");GameSession d(directional);restore(d,pair(*directional));tick(d);need(at(d,{-1,63})==2,"direction constraint rejects the sampled right neighbor without searching elsewhere");
    auto triple=catalog("material.a;material.b;material.c;material.d;1;0;-1000;10000;material.e;keep;probability=100");GameSession t(triple);auto snap=pair(*triple);put(snap,{-2,63},6);restore(t,snap);tick(t);
    need(at(t,{-1,63})==4 && at(t,{0,63})==5 && at(t,{-2,63})==6,"third sample equal to B reflects to the opposite neighbor of A across the chunk edge");
    snap=pair(*triple);put(snap,{1,63},6);restore(t,snap);tick(t);need(at(t,{-1,63})==2,"third input beside B alone is not an arbitrary connected triple");
    auto fast=catalog("material.a;material.b;material.c;keep;1;probability=100;fast=1");GameSession f(fast);snap=blank(*fast);put(snap,{-1,63},2);for(const auto delta:std::array<CellPos,4>{{{0,-1},{0,1},{-1,0},{1,0}}})put(snap,{-1+delta.x,63+delta.y},3);restore(f,snap);tick(f);need(at(f,{-1,63})==4,"fast records have their own dispatch path when no ordinary candidate exists");
    auto lonely=catalog("material.a;material.b;material.c;keep;1;probability=100;destroy_lonely=1");GameSession l(lonely);snap=blank(*lonely,0);put(snap,{-1,63},2);put(snap,{-1,62},3);put(snap,{-2,63},0);put(snap,{0,63},0);restore(l,snap);tick(l);need(at(l,{-1,63})==0,"horizontally unsupported first output becomes air");
}
void required(){
    const std::string one="material.a;air;material.c;air;1;probability=100;required=1";
    auto c=catalog(one+"\n"+one);GameSession s(c);auto snap=blank(*c,0);put(snap,{-1,63},2);put(snap,{-1,62},0);restore(s,snap);tick(s);
    need(at(s,{-1,63})==4 && at(s,{-1,62})==0,"one air neighbor removes only one requirement and is never consumed");
    snap=blank(*c,0);put(snap,{-1,63},2);put(snap,{-1,62},0);put(snap,{-1,64},0);restore(s,snap);tick(s);need(at(s,{-1,63})==2,"two air neighbors satisfy both records");
    const auto before=*s.capture_snapshot();static_cast<void>(s.take_changes());tick(s,100);auto after=*s.capture_snapshot();after.tick=before.tick;
    need(before==after && !s.published_world()->active_chunks && s.take_changes().dirty_chunks.empty(),"satisfied static requirements sleep until neighbor changes");
    c=catalog("material.a;air;material.c;air;1;probability=100;required=1;direction=top\nmaterial.a;air;material.d;air;1;probability=100;required=1;direction=left\nmaterial.a;air;material.e;air;1;probability=100;required=1;direction=right");
    GameSession order(c);snap=blank(*c,0);put(snap,{-1,63},2);for(const auto delta:std::array<CellPos,4>{{{0,-1},{0,1},{-1,0},{1,0}}})put(snap,{-1+delta.x,63+delta.y},0);restore(order,snap);tick(order);
    need(at(order,{-1,63})==5,"swap-pop and direction-mismatch early break retain the left requirement even with four air cells");
    auto zero=catalog("material.a;air;material.c;air;1;probability=0;required=1");GameSession z(zero);restore(z,pair(*zero,21649));tick(z);need(at(z,{-1,63})==2,"required dispatch uses strict p<material maximum, unlike ordinary record comparison");
}
void geometry(){
    std::map<std::pair<int,int>,int> grid;
    auto fill=[&](int value){grid.clear();for(int y=-20;y<=20;++y)for(int x=-20;x<=20;++x)grid[{x,y}]=value;};
    auto read=[&](CellPos p)->std::optional<int>{const auto it=grid.find({p.x,p.y});return it==grid.end()?std::nullopt:std::optional{it->second};};
    auto write=[&](CellPos p){grid[{p.x,p.y}]=2;};
    fill(1);source_reaction_blob({0,0},1,false,1,2,read,write);need(grid[{0,0}]==2 && grid[{1,0}]==1,"unrestricted radius one excludes the circumference");
    fill(1);source_reaction_blob({0,0},0,false,1,2,read,write);need(grid[{0,0}]==1,"unrestricted radius zero is empty");
    source_reaction_blob({0,0},0,true,1,2,read,write);need(grid[{0,0}]==2,"restricted radius zero converts the center");
    fill(3);grid[{0,0}]=1;grid[{1,0}]=2;grid[{2,0}]=2;grid[{3,0}]=1;source_reaction_blob({0,0},4,true,1,2,read,write);need(grid[{3,0}]==2,"restricted rays cross existing output material");
    fill(3);for(int x=0;x<=4;++x)grid[{x,0}]=1;grid[{2,0}]=3;source_reaction_blob({0,0},4,true,1,2,read,write);need(grid[{1,0}]==2 && grid[{3,0}]==1,"other material stops only the current ray");
    fill(3);for(int x=8;x<=12;++x)grid[{x,8}]=1;for(int y=4;y<=8;++y)grid[{12,y}]=1;source_reaction_blob({8,8},8,true,1,2,read,write);
    need(grid[{11,8}]==2 && grid[{12,4}]==1,"radial sampling does not flood around an L-shaped channel");
    fill(1);source_reaction_blob({-8,-8},1,true,1,2,read,write);need(grid[{-7,-7}]==2,"negative float coordinates truncate toward zero rather than floor");
    fill(3);source_reaction_blob({0,0},3,false,1,2,read,write);need(grid[{2,2}]==2 && grid[{3,0}]==3,"unrestricted strict disk replaces other materials but excludes equal radius");
}
void regions(){
    auto c=catalog("material.a;material.b;material.c;material.d;1;probability=100;blob_radius_a=2;restrict_a=0");GameSession s(c);restore(s,pair(*c));tick(s);
    need(at(s,{-1,63})==4 && at(s,{0,63})==4 && at(s,{-2,62})==4,"later unrestricted A region overrides staged B output and includes unrelated material");
    const auto steady=*s.capture_snapshot();static_cast<void>(s.take_changes());tick(s,100);auto after=*s.capture_snapshot();after.tick=steady.tick;
    need(after==steady && !s.published_world()->active_chunks && s.take_changes().dirty_chunks.empty(),"completed source region leaves no perpetual activity or display churn");
    c=catalog("material.a;material.a;material.c;material.d;1;probability=100;blob_radius_a=2;blob_radius_b=2");GameSession overlap(c);auto snap=pair(*c);put(snap,{0,63},2);restore(overlap,snap);tick(overlap);
    need(at(overlap,{-1,63})==5 && at(overlap,{0,63})==5,"B radial output can replace A before A's restricted center check; no nearest-anchor flood arbitration");
    c=catalog("material.a;material.b;material.solid;material.d;1;probability=100");GameSession rejected(c);restore(rejected,pair(*c));tick(rejected);
    need(at(rejected,{-1,63})==2 && at(rejected,{0,63})==3 && rejected.published_world()->deferred_reactions>0,"unsupported grid-to-body product rejects the entire staged batch, including B");
}
std::shared_ptr<const ContentRegistry> built(std::vector<MaterialDefinition> defs,std::string_view rules=""){
    auto parsed=parse_reaction_table(rules);need(bool(parsed),parsed.error);auto result=ContentRegistry::build(std::move(defs),parsed.reactions);need(bool(result),result.error);return result.registry;
}
void environment_phase(){
    auto defs=definitions();auto& a=defs[1];a.source_phase=true;a.cold_output="material.c";a.warmth_output="material.d";a.cold_chance_rev=a.warmth_chance_rev=0;
    defs[2].sand=false;auto c=built(defs,"material.a;material.b;material.e;keep;1;probability=100");
    auto snap=blank(*c);put(snap,{-1,63},2);for(const auto delta:std::array<CellPos,4>{{{0,-1},{0,1},{-1,0},{1,0}}})put(snap,{-1+delta.x,63+delta.y},3);
    snap.freeze_chance=snap.unfreeze_chance=255;GameSession blocked(c);restore(blocked,snap);tick(blocked);
    need(at(blocked,{-1,63})==2,"freeze candidate blocked by a non-sand liquid suppresses melt and ordinary contact fallback");
    snap=blank(*c);put(snap,{-1,63},2);snap.freeze_chance=snap.unfreeze_chance=255;restore(blocked,snap);tick(blocked);need(at(blocked,{-1,63})==4,"freeze has priority when both phase candidates pass");
    snap=blank(*c);put(snap,{-1,63},2);snap.unfreeze_chance=255;restore(blocked,snap);tick(blocked);need(at(blocked,{-1,63})==5,"warm environment converts without stored heat");
    CombustionRandom both(73),expected(73);static_cast<void>(source_phase_candidates(a,255,255,false,both));expected.next();expected.next();need(both.next()==expected.next(),"warm candidate consumes its own random sample even when freezing already passed");
    auto invalid=defs;invalid[1].cold_chance_rev=-1;need(!ContentRegistry::build(invalid),"reject invalid signed reverse gate denominator");
}
void temperature_phase(){
    auto defs=definitions();defs[1].source_phase=true;defs[1].cold_output="material.c";defs[1].warmth_output="material.d";defs[1].cold_chance_rev=defs[1].warmth_chance_rev=0;
    auto c=built(defs);GameSession session(c);auto snap=blank(*c);put(snap,{-1,63},2);restore(session,snap);
    need(bool(session.submit({CommandKind::source_warm,{-1,63},1})),"queue source warm tool");need(at(session,{-1,63})==2,"temperature UI command cannot mutate before fixed-step boundary");tick(session);
    need(at(session,{-1,63})==5 && session.world()->cell_state({-1,63})->temperature==0,"signed T selects warm target without accumulating temperature");
    restore(session,snap);need(bool(session.submit({CommandKind::source_cold,{-1,63},1})),"queue source cold tool");tick(session);need(at(session,{-1,63})==4,"negative T selects cold target without a neighbor prerequisite");
    CombustionRandom r1(15),r2(15);int easy=0,hard=0;for(int i=0;i<1000;++i){easy+=source_phase_gate(r1,100,10);hard+=source_phase_gate(r2,500,10);}need(easy>hard*3 && hard>0,"water reverse gate 500 is substantially harder than generic 100 for T10");
    need(bool(session.submit({CommandKind::environment_cold,{-1,63},1})),"queue cold environment");tick(session);need(session.published_world()->freeze_chance==5 && !session.published_world()->unfreeze_chance,"environment command publishes actual cold byte");
}
void source_electric(){
    auto defs=definitions();defs[1].electrical_conductivity=true;auto c=built(defs);auto snap=blank(*c);put(snap,{-2,63},2);put(snap,{-1,63},0);put(snap,{0,63},2);
    ElectricPulse pulse;pulse.position=pulse.previous={-2,63};pulse.energy=20;pulse.speed=1;pulse.source_rules=true;pulse.avg_x=1;pulse.avg_y=0;pulse.glow_ticks=10;snap.electric_pulses={pulse};
    GameSession session(c);restore(session,snap);tick(session);auto result=*session.capture_snapshot();need(result.electric_pulses.size()==1 && result.electric_pulses[0].position==CellPos{0,63} && result.electric_pulses[0].energy==19,"solid conductor searches across a one-cell gap and spends one energy per successful internal step");
    need(result.states[index(result,{-2,63})].electric_stamp==0 && result.states[index(result,{0,63})].electric_stamp==UINT64_MAX,"nearby search marks old 3x3 instead of marking the selected remote target");
    pulse.splittings_left=2;pulse.splitting_energy=19;snap.electric_pulses={pulse};restore(session,snap);tick(session);result=*session.capture_snapshot();need(result.electric_pulses.size()==2 && result.electric_pulses[0].energy==9 && result.electric_pulses[1].energy==9,"exact energy threshold replaces parent by two rounded-down children");
    for(const auto& child:result.electric_pulses)need(child.splittings_left==1 && child.splitting_energy==9 && std::abs(std::hypot(child.avg_x,child.avg_y)-30)<0.0001,"children preserve source split counters and length-30 directions");
    need(session.published_world()->electric_energy+session.published_world()->electric_spent==20,"split rounding loses energy without creating any");need(validate_snapshot(result).empty(),"two source heads at one position form a valid snapshot");
    defs[1].sand=false;c=built(defs);snap=blank(*c);put(snap,{-2,63},2);pulse.splittings_left=0;snap.electric_pulses={pulse};GameSession fluid(c);restore(fluid,snap);tick(fluid);result=*fluid.capture_snapshot();need(result.electric_pulses.size()==1 && at(fluid,result.electric_pulses[0].position)==1,"early fluid random candidates can accept existing nonconductive material, unlike the final fallback");
    need(result.electric_pulses[0].position!=CellPos{-2,63},"fluid branch uses a nonzero truncated rotated step");
    tick(fluid,30);need(fluid.published_world()->electric_energy==0,"finite source arc terminates");tick(fluid,20);const auto stable=*fluid.capture_snapshot();static_cast<void>(fluid.take_changes());tick(fluid,100);auto after=*fluid.capture_snapshot();after.tick=stable.tick;need(after==stable && fluid.take_changes().dirty_chunks.empty(),"source electrical afterglow leaves no perpetual state churn");
}
void source_blast(){
    CombustionRandom random(4);std::map<std::pair<int,int>,SourceBlastObstacle> grid;grid[{0,0}]={1000,99,true};grid[{1,0}]={7,2,true};grid[{2,0}]={12,20,true};
    const auto probe=source_blast_probe(0,0,4,0,10,10,random,[&](CellPos p)->std::optional<SourceBlastObstacle>{const auto it=grid.find({p.x,p.y});return it==grid.end()?std::nullopt:std::optional{it->second};});
    need(probe.max_spent==7 && probe.distance_squared>=4 && probe.distance_squared<=5,"probe skips center, spends current HP before hardness blocking and records max single-cell expenditure");need(grid[{1,0}].hp==7,"probing never writes HP");
    auto defs=definitions();defs[1].fuel=1;defs[1].flame_output=defs[1].smoke_output=defs[1].burn_output="air";auto& blast=defs[1].explosion;blast.source_rules=true;blast.radius=1;blast.ray_energy=5;blast.create_probability=0;blast.heat=blast.impulse_q8=0;blast.fire_material=blast.output="air";defs[2].hit_points=12;
    auto c=built(defs);auto snap=blank(*c);snap.cells.assign(snap.cells.size(),0);snap.initial_stone=0;put(snap,{0,63},3);put(snap,{3,63},3);
    GameSession session(c);restore(session,snap);
    auto fire=[&](){need(bool(session.submit({CommandKind::blast,{-1,63},1,"material.a"})),"queue blast");tick(session);};
    fire();need(at(session,{0,63})==3 && session.world()->cell_state({0,63})->damage_taken==5,"sublethal blast retains cell and accumulated HP damage across chunk boundary");
    fire();need(session.world()->cell_state({0,63})->damage_taken==10,"later probe reads remaining HP and damage persists");fire();need(at(session,{0,63})==0,"repeated blast finally exhausts HP");need(at(session,{3,63})==3 && !session.world()->cell_state({3,63})->damage_taken,"strict radius excludes the circumference");
}
void corrosion(){
    auto defs=definitions();defs[1].tags={"acid"};defs[2].tags={"corrodible"};defs[2].hit_points=99999999;defs[2].durability=9999;defs[3].tags={"corrodible"};defs[4].tags={"meltable_metal"};defs[5].cell_type="gas";defs[5].sand=false;
    auto c=built(defs,"[acid];[corrodible];keep;material.e;1;probability=100;fast=1;blob_radius_a=2;blob_radius_b=2");
    auto snap=pair(*c);put(snap,{1,63},3);put(snap,{2,63},4);GameSession acid(c);restore(acid,snap);tick(acid,10);
    need(at(acid,{-1,63})==2 && acid.world()->material_count(static_cast<Material>(3))==0,"acid ignores HP/hardness and does not consume its same-ID source blob");need(at(acid,{2,63})==4,"restricted wood reaction stops at another corrodible material ID");
    snap=blank(*c);put(snap,{-1,63},2);put(snap,{0,63},5);restore(acid,snap);tick(acid,10);need(at(acid,{0,63})==5,"glass-like tag set without corrodible resists this acid recipe");
    c=built(defs,"material.a;[meltable_metal];keep;material.e;1;probability=100");GameSession mana(c);restore(mana,snap);tick(mana,10);need(at(mana,{0,63})==6,"same material can dissolve through mana metal tag despite acid resistance");
}
void gas_counter(){
    auto defs=definitions();auto& gas=defs[1];gas.cell_type="gas";gas.sand=false;gas.is_static=false;gas.source_gas=true;gas.lifetime=450;auto c=built(defs);
    int minimum=10000,maximum=-10000;for(unsigned seed=0;seed<10000;++seed){const auto state=MaterialSimulation::initial_state(gas,seed);minimum=std::min(minimum,state.gas_counter);maximum=std::max(maximum,state.gas_counter);}
    need(minimum==-135 && maximum==135,"gas constructor uses source +/-30% signed offset for L450");
    auto snap=blank(*c);put(snap,{-1,63},2);snap.states[index(snap,{-1,63})]=MaterialSimulation::initial_state(gas,33);const int initial=snap.states[index(snap,{-1,63})].gas_counter;
    GameSession trapped(c);restore(trapped,snap);tick(trapped,200);need(at(trapped,{-1,63})==2 && trapped.world()->cell_state({-1,63})->gas_counter==initial,"blocked gas does not accrue an unconditional fixed-tick lifetime counter");
    auto age_defs=defs;age_defs[1].is_static=true;auto aged=built(age_defs,"material.a;material.stone;material.c;keep;1;1;probability=100");auto age=blank(*aged);age.tick=1000;put(age,{-1,63},2);age.states[index(age,{-1,63})]=MaterialSimulation::initial_state(age_defs[1]);age.states[index(age,{-1,63})].gas_counter=-1;GameSession lifetime(aged);restore(lifetime,age);tick(lifetime,10);need(at(lifetime,{-1,63})==2,"source gas req_lifetime reads its signed counter instead of world tick age");age.states[index(age,{-1,63})].gas_counter=1;restore(lifetime,age);tick(lifetime);need(at(lifetime,{-1,63})==4,"positive gas lifetime counter satisfies recovered reaction age gate");
    put(snap,{-1,62},0);snap.states[index(snap,{-1,63})].gas_counter=449;snap.states[index(snap,{-1,63})].velocity_y=-256;restore(trapped,snap);tick(trapped);need(!trapped.world()->material_count(static_cast<Material>(2)),"actual gas move increments counter and expiry removes source at L");
}
std::shared_ptr<const ContentRegistry> adhesive_catalog(bool sand=false){
    auto defs=definitions();auto& a=defs[1];a.is_static=false;a.sand=sand;
    a.source_adhesion=true;a.liquid_sticks_to_ceiling=100;
    return built(defs);
}
void source_adhesion(){
    // Coordinate cases come from the recovered x86 direction routine, including
    // arithmetic right shifts and negative y parity. No world seed participates.
    need(source_attachment_direction({-1,64})==7 && source_attachment_direction({-1,63})==5 &&
         source_attachment_direction({1,63})==6 && source_attachment_direction({-1,-2})==7 &&
         source_attachment_direction({-4,-1})==5,"coordinate-selected side/top directions preserve signed-coordinate behavior");
    const auto c=adhesive_catalog();
    for(const auto p:std::array<CellPos,3>{{{-1,64},{-1,63},{1,63}}}){
        auto snap=blank(*c);put(snap,p,2);
        for(const auto delta:std::array<CellPos,3>{{{-1,0},{1,0},{0,-1}}}){
            const auto selected=p==CellPos{-1,64}?CellPos{-1,0}:p==CellPos{-1,63}?CellPos{1,0}:CellPos{0,-1};
            if(delta!=selected)put(snap,{p.x+delta.x,p.y+delta.y},0);
        }
        // Occupied below suppresses the random drip even at threshold 100.
        GameSession s(c);restore(s,snap);tick(s,300);
        need(at(s,p)==2 && s.world()->cell_state(p)->source_attachment==(p.y==64?7:p.x==-1?5:6),"attachment uses the selected side or ceiling and occupied-below hold has no fixed expiry");
        const auto held=*s.capture_snapshot();static_cast<void>(s.take_changes());tick(s,100);auto after=*s.capture_snapshot();after.tick=held.tick;
        need(after==held && s.take_changes().dirty_chunks.empty(),"held source material has no timer churn or repeated texture changes");
    }
    auto snap=blank(*c);const CellPos p{-1,64};put(snap,p,2);put(snap,{-2,64},0);put(snap,{-1,65},0);put(snap,{0,64},0);
    GameSession wrong_side(c);restore(wrong_side,snap);tick(wrong_side,8);
    need(at(wrong_side,p)!=2,"missing selected left support cannot fall back to an available ceiling");
    int releases=0,retained=0,free_flags=0,moved_later=0;
    for(std::uint64_t seed=0;seed<32;++seed){
        snap=blank(*c,seed);put(snap,p,2);put(snap,{-1,65},0);
        GameSession drip(c);restore(drip,snap);tick(drip);need(drip.world()->cell_state(p)->source_attachment==7,"acquisition is independent of drip threshold sampling");
        tick(drip);if(at(drip,p)==2)++retained;else ++releases;
        if(drip.world()->cell_state(p)->source_attachment<5)++free_flags;
        tick(drip,120);need(drip.world()->material_count(static_cast<Material>(2))==1,"source dripping transfers the material without creating/deleting it");
        moved_later+=at(drip,p)!=2;
    }
    std::cout<<"drip next-position="<<releases<<" retained="<<retained<<" free-flags="<<free_flags<<" moved-after120="<<moved_later<<'\n';
    need(free_flags>0 && free_flags<32 && retained>0 && moved_later==32,"source maintenance gate produces both holds and releases, then every sampled droplet actually leaves the source position");
    snap=blank(*c);put(snap,{2,62},2);snap.states[index(snap,{2,62})].source_attachment=6;put(snap,{2,63},2);put(snap,{2,64},0);
    GameSession chain(c);restore(chain,snap);tick(chain);
    need(!chain.world()->cell_state({2,63})->source_attachment && chain.world()->cell_state({2,62})->source_attachment==6,"unsanded attached liquid is not a new sand-class acquisition anchor for an artificial vertical chain");
    const auto sand=adhesive_catalog(true);snap=blank(*sand);put(snap,p,2);GameSession glue(sand);restore(glue,snap);tick(glue,100);
    need(glue.world()->cell_state(p)->source_attachment==7,"sand-class glue uses the same recovered attachment path");
    snap=blank(*c);put(snap,p,2);for(int x=0;x<8;++x)put(snap,{x,64},0);
    GameSession pushed(c);restore(pushed,snap);tick(pushed);need(bool(pushed.submit({CommandKind::push,{-2,64},1})),"queue lab impulse beside held source liquid");tick(pushed,16);
    need(at(pushed,p)!=2 && pushed.world()->material_count(static_cast<Material>(2))==1,"explicit lab impulse transfers a released adhesive parcel instead of reacquiring and erasing its momentum");
}
void source_adhesion_save(){
    const auto c=adhesive_catalog();auto snap=blank(*c);put(snap,{-1,64},2);
    GameSession original(c);restore(original,snap);tick(original);snap=*original.capture_snapshot();
    need(snap.states[index(snap,{-1,64})].source_attachment==7,"save fixture has actual acquired side attachment");
    const auto path=std::filesystem::temp_directory_path()/"pixelsandbox-adhesion-v14.psw";
    auto saved=persistence::save_world_file(path,snap);need(saved.ok,saved.error);auto loaded=persistence::load_world_file(path);need(loaded.snapshot && *loaded.snapshot==snap,"real format14 preserves acquired source direction");
    GameSession resumed(c);restore(resumed,*loaded.snapshot);
    need(bool(original.submit({CommandKind::erase,{-3,64},1})) && bool(resumed.submit({CommandKind::erase,{-3,64},1})),"queue support removal on both worlds");
    for(int t=0;t<100;++t){tick(original);tick(resumed);need(original.capture_snapshot()==resumed.capture_snapshot(),"restored held material uses the same subsequent release and transport trajectory");}
    auto bad=snap;bad.states[index(bad,{-1,64})].source_attachment=4;const auto before=resumed.capture_snapshot();
    need(!resumed.restore_world(bad) && resumed.capture_snapshot()==before,"invalid direction is rejected atomically");
    bad=snap;bad.states[0].source_attachment=7;need(!resumed.restore_world(bad),"non-adhesive material cannot carry source attachment state");
    std::filesystem::remove(path);
}
void interaction_save(){
    auto defs=definitions();defs[1].source_gas=true;defs[1].cell_type="gas";defs[1].sand=false;defs[1].lifetime=450;auto c=built(defs);
    auto snap=blank(*c);snap.tick=40;snap.freeze_chance=5;snap.unfreeze_chance=12;put(snap,{-1,63},2);snap.states[index(snap,{-1,63})]=MaterialSimulation::initial_state(defs[1],22);snap.states[index(snap,{-1,63})].gas_counter=-135;snap.states[index(snap,{-1,63})].electric_stamp=39;snap.states[0].damage_taken=31;
    ElectricPulse pulse;pulse.position={0,63};pulse.previous={-4,63};pulse.source_rules=true;pulse.avg_x=3.25f;pulse.avg_y=-1.5f;pulse.energy=777;pulse.splittings_left=2;pulse.splitting_energy=500;pulse.probability_to_heat=0.2f;snap.electric_pulses={pulse};
    const auto path=std::filesystem::temp_directory_path()/"pixelsandbox-interactions.psw";auto saved=persistence::save_world_file(path,snap);need(saved.ok,saved.error);auto loaded=persistence::load_world_file(path);need(loaded.snapshot.has_value(),loaded.error);need(*loaded.snapshot==snap,"current file roundtrip retains HP/gas/electric direction, split state and environment");
    GameSession a(c),b(c);restore(a,snap);restore(b,*loaded.snapshot);tick(a,40);tick(b,40);need(a.capture_snapshot()==b.capture_snapshot(),"restored interaction state continues deterministically");
    auto invalid=snap;invalid.electric_pulses[0].avg_x=std::numeric_limits<float>::quiet_NaN();const auto before=b.capture_snapshot();need(!b.restore_world(invalid) && b.capture_snapshot()==before,"invalid new electrical state is rejected before replacing current world");
    auto old=blank(*c);old.bounds={{-1,63},1,1};old.cells={1};old.material_ids={"material.stone"};old.states={{}};old.initial_stone=1;old.states[0].temperature=17;
    saved=persistence::save_world_file(path,old);need(saved.ok,saved.error);
    std::ifstream stream(path,std::ios::binary);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)),{});stream.close();
    // One material.stone mapping: payload state begins at byte 93; v12 had
    // 54 common bytes then 8 burning bytes. Remove exactly v13's 16-byte addition.
    constexpr std::size_t state_begin=28+93;bytes.erase(bytes.begin()+state_begin+79,bytes.begin()+state_begin+88); // v15 accumulators/direction.
    auto write_int=[&](std::size_t offset,std::uint64_t value,int size){for(int i=0;i<size;++i)bytes[offset+i]=static_cast<unsigned char>(value>>(8*i));};
    auto previous_file=[&](int version){
        write_int(8,version,4);write_int(28+89,version-1,4);write_int(12,bytes.size()-28,8);std::uint64_t hash=14695981039346656037ULL;
        for(std::size_t i=28;i<bytes.size();++i){hash^=bytes[i];hash*=1099511628211ULL;}write_int(20,hash,8);
        std::ofstream previous(path,std::ios::binary|std::ios::trunc);previous.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    };
    previous_file(14);loaded=persistence::load_world_file(path);need(loaded.snapshot && loaded.snapshot->states[0].temperature==17 && !loaded.snapshot->states[0].liquid_fall && !loaded.snapshot->states[0].liquid_lateral && !loaded.snapshot->states[0].liquid_direction,"real v14 bytes preserve old state and initialize source accumulators");
    bytes.erase(bytes.begin()+state_begin+78); // v14 attachment byte.
    previous_file(13);loaded=persistence::load_world_file(path);need(loaded.snapshot && loaded.snapshot->states[0].temperature==17 && !loaded.snapshot->states[0].source_attachment,"real v13 bytes retain old state and initialize free source attachment");
    bytes.erase(bytes.begin()+state_begin+54,bytes.begin()+state_begin+70);bytes.resize(bytes.size()-2);previous_file(12);
    loaded=persistence::load_world_file(path);need(loaded.snapshot.has_value(),loaded.error);need(loaded.snapshot->states[0].temperature==17 && !loaded.snapshot->states[0].damage_taken && !loaded.snapshot->states[0].gas_counter && loaded.snapshot->states[0].electric_stamp==UINT64_MAX && !loaded.snapshot->freeze_chance,"real v12 bytes migrate existing state and initialize new interaction fields");
    std::filesystem::remove(path);
}
}
int main(int argc,char** argv){try{need(argc==3,"Usage: pixel_reaction_tests CASE GAME_PATH");const std::string name=argv[1];
    if(name=="reaction_labels")labels(argv[2]);else if(name=="reaction_sampling")sampling();else if(name=="reaction_required")required();else if(name=="reaction_geometry")geometry();else if(name=="reaction_regions")regions();else if(name=="source_phase_environment")environment_phase();else if(name=="source_phase_temperature")temperature_phase();else if(name=="source_electricity")source_electric();else if(name=="source_blast")source_blast();else if(name=="source_corrosion")corrosion();else if(name=="source_gas")gas_counter();else if(name=="source_adhesion")source_adhesion();else if(name=="source_adhesion_save")source_adhesion_save();else if(name=="interaction_save")interaction_save();else throw std::runtime_error("unknown reaction test");
    std::cout<<"PASS "<<name<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}


