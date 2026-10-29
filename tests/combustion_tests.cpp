#include "session/game_session.hpp"
#include "simulation/material_simulation.hpp"
#include "simulation/combustion.hpp"
#include "physics/box2d_solver.hpp"
#include "persistence/world_file.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include "benchmark_material_catalog.hpp"

using namespace pixel;
namespace {
void need(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
std::shared_ptr<const ContentRegistry> catalog(const std::function<void(std::vector<MaterialDefinition>&)>& edit={}){
    MaterialDefinition stone;stone.id="material.stone";stone.display_name="Stone";
    auto wood=stone;wood.id="material.wood";wood.source_burning=true;wood.burnable=true;wood.fire_hp=-1;wood.fuel=600;
    wood.requires_oxygen=false;wood.generates_flames=0;wood.generates_smoke=0;wood.burn_heat=50;wood.ignition=0;wood.color=0xff808080;
    auto body=wood;body.id="material.wood_prop";body.cell_type="solid";body.sand=false;body.is_static=false;
    auto fire=wood;fire.id="material.fire";fire.cell_type="fire";fire.sand=false;fire.is_static=false;fire.burnable=false;
    fire.initially_burning=true;fire.fuel=0;fire.burn_heat=100;fire.lifetime=90;fire.tags={"fire"};
    auto smoke=fire;smoke.id="material.smoke";smoke.cell_type="gas";smoke.initially_burning=false;smoke.lifetime=80;smoke.tags.clear();
    auto ash=stone;ash.id="material.char";ash.source_burning=true;
    std::vector<MaterialDefinition> defs{stone,wood,body,fire,smoke,ash};if(edit)edit(defs);
    const auto result=ContentRegistry::build(std::move(defs));need(bool(result),result.error);return result.registry;
}
WorldSnapshot blank(const ContentRegistry& c){
    auto s=MaterialSimulation::make_lab(12345,c).snapshot();s.bounds={{-8,56},16,24};s.cells.assign(16*24,0);s.states.assign(s.cells.size(),{});s.initial_stone=0;return s;
}
std::size_t index(const WorldSnapshot& s,CellPos p){return static_cast<std::size_t>(p.y-s.bounds.origin.y)*s.bounds.width+p.x-s.bounds.origin.x;}
void put(WorldSnapshot& s,const ContentRegistry& c,CellPos p,const std::string& id){
    auto& m=s.cells[index(s,p)];if(m==1)--s.initial_stone;
    m=static_cast<std::uint16_t>(1+std::find(s.material_ids.begin(),s.material_ids.end(),id)-s.material_ids.begin());
    if(m==1)++s.initial_stone;s.states[index(s,p)]=MaterialSimulation::initial_state(*c.find_material(id));
}
void restore(GameSession& s,const WorldSnapshot& snap){const auto r=s.restore_world(snap);need(r.ok,r.error);need(bool(s.resume()),"resume");}
void ticks(GameSession& s,int n=1){for(int i=0;i<n;++i)need(s.advance_fixed_step(),"fixed step");}
std::uint64_t count(const GameSession& s,const char* id){return s.world()->material_count(s.world()->material_index(id));}
void heat(GameSession& s,CellPos at){need(bool(s.submit({CommandKind::heat,at,1})),"queue forced ignition");ticks(s);}
void ignition(){
    CombustionRandom random(0);need(random.next()==38 && random.next()==7719,"recovered LCG has known 15-bit output sequence");
    int lit=0;
    for(int difficulty:{0,100}){
        const auto c=catalog([&](auto& d){d[1].ignition=difficulty;});GameSession session(c);auto s=blank(*c);
        for(int y=56;y<80;++y)for(int x=-8;x<8;++x)put(s,*c,{x,y},"material.stone");
        put(s,*c,{0,64},"material.fire");for(const CellPos p:std::array<CellPos,4>{{{0,63},{-1,64},{1,64},{0,65}}})put(s,*c,p,"material.wood");
        restore(session,s);ticks(session);int burning=0;
        for(const CellPos p:std::array<CellPos,4>{{{0,63},{-1,64},{1,64},{0,65}}})burning+=session.world()->cell_state(p)->burning;
        if(difficulty==100)need(!burning,"difficulty 100 has zero ordinary ignition gate even beside actual fire");
        else{
            need(burning>0 && burning<=2,"fire chooses at most two single cardinal neighbors per update");lit=burning;
            need(bool(session.submit({CommandKind::cool,{0,64},1})),"cool the real fire-adjacent targets");ticks(session);
            int relit=0;for(const CellPos p:std::array<CellPos,4>{{{0,63},{-1,64},{1,64},{0,65}}}){
                need(session.world()->cell_state(p)->temperature<50,"cooling lowers temperature even when a fire attempt follows");relit+=session.world()->cell_state(p)->burning;
            }
            need(relit>0 && relit<=2,"ordinary ignition is not a temperature threshold or an implicit post-cooling immunity");
        }
    }
    auto c=catalog([](auto& d){d[1].ignition=100;});GameSession forced(c);auto s=blank(*c);put(s,*c,{0,64},"material.wood");restore(forced,s);heat(forced,{0,64});
    need(forced.world()->cell_state({0,64})->burning,"explicit heat tool uses forced entry, bypassing probability");
    c=catalog([](auto& d){d[1].ignition_output="material.char";});GameSession converted(c);s=blank(*c);put(s,*c,{0,64},"material.wood");restore(converted,s);heat(converted,{0,64});
    need(!count(converted,"material.wood") && count(converted,"material.char")==1,"ignition conversion occurs on entry, before waiting for burnout");
    c=catalog([](auto& d){d[1].fire_hp=0;});GameSession zero(c);s=blank(*c);put(s,*c,{0,64},"material.wood");restore(zero,s);heat(zero,{0,64});
    need(!count(zero,"material.wood") && count(zero,"material.fire")==1,"zero-HP liquid-class material converts immediately to its flame material");
    const auto original=ContentRegistry::build(benchmark_materials());need(bool(original),original.error);
    need(original.registry->find_material("material.wood")->source_burning && original.registry->find_material("material.wood")->fire_hp==600 &&
         original.registry->find_material("material.wood_prop")->fire_hp==420 && original.registry->find_material("material.wood")->generates_smoke==0 &&
         original.registry->find_material("material.fire")->initially_burning && !original.registry->find_material("material.fire")->burnable,"actual generated project catalog uses source fields independently of fuel and burnable");
    auto defs=c->materials();defs[1].fire_hp=-2;need(!ContentRegistry::build(defs),"reject invalid signed fire_hp");defs=c->materials();defs[1].generates_flames=102;need(!ContentRegistry::build(defs),"reject invalid emission gate");
    defs=c->materials();defs[1].ignition_output="missing";need(!ContentRegistry::build(defs),"reject missing ignition conversion output");
    std::cout<<"ordinary lit="<<lit<<"; forced/entry conversion; actual catalog="<<original.registry->material_count()<<'\n';
}
void emission(){
    auto c=catalog([](auto& d){d[1].initially_burning=true;d[1].requires_oxygen=true;d[1].generates_flames=101;d[1].burn_heat=3;});
    GameSession sealed(c);auto s=blank(*c);put(s,*c,{0,64},"material.wood");for(int x=-1;x<=1;++x)put(s,*c,{x,63},"material.stone");restore(sealed,s);
    ticks(sealed);need(sealed.world()->cell_state({0,64})->burning && sealed.world()->cell_state({0,64})->fire_intensity==2,"failed upper emission decays intensity despite cardinal air beside fuel");
    ticks(sealed,2);need(!sealed.world()->cell_state({0,64})->burning && sealed.world()->cell_state({0,64})->fuel==600 && !count(sealed,"material.fire"),"oxygen path extinguishes at signed zero without burning down a uniform fuel budget");
    int success=0,failed=0;
    for(int seed=0;seed<32;++seed){s=blank(*c);s.seed=seed;put(s,*c,{0,64},"material.wood");put(s,*c,{-1,63},"material.stone");put(s,*c,{1,63},"material.stone");restore(sealed,s);ticks(sealed);
        if(count(sealed,"material.fire")){++success;need(sealed.world()->cell_state({0,64})->fire_intensity==3,"successful flame placement restores material intensity");}
        else{++failed;need(sealed.world()->material_at({0,63})==Material::air && sealed.world()->cell_state({0,64})->fire_intensity==2,"one sampled blocked slot does not retry the remaining vacancy");}
    }
    need(success && failed,"single-slot emission exercises both creation success and blocked sample");
    c=catalog([](auto& d){d[1].initially_burning=true;d[1].requires_oxygen=true;d[1].burn_heat=1;d[1].generates_smoke=101;});
    GameSession smoke(c);s=blank(*c);put(s,*c,{0,64},"material.wood");restore(smoke,s);ticks(smoke);
    need(!smoke.world()->cell_state({0,64})->burning && count(smoke,"material.smoke")==1 && !count(smoke,"material.fire"),"independent smoke generation runs after oxygen decay, even when flames are disabled");
    const auto snap=*smoke.capture_snapshot();for(std::size_t i=0;i<snap.cells.size();++i)if(snap.cells[i]==static_cast<std::uint16_t>(smoke.world()->material_index("material.smoke")))need(snap.states[i].fire_intensity==0,"smoke receives the post-decay intensity byte");
    std::cout<<"single upper slot success/blocked="<<success<<'/'<<failed<<'\n';
}
void burnout(){
    const auto c=catalog([](auto& d){d[1].initially_burning=true;d[1].fire_hp=3;});int early=0,survived=0;
    for(int seed=0;seed<32;++seed){GameSession session(c);auto s=blank(*c);s.seed=seed;put(s,*c,{0,64},"material.wood");restore(session,s);ticks(session);
        if(!count(session,"material.wood"))++early;
        else need(session.world()->cell_state({0,64})->fuel==600,"liquid fire_hp is sampled from definition, not decremented per-instance fuel");
        ticks(session,3);if(count(session,"material.wood"))++survived;
    }
    need(early && survived,"sampled burnout can finish early or survive beyond the definition threshold");
    GameSession finite(c);auto s=blank(*c);put(s,*c,{0,64},"material.wood");restore(finite,s);ticks(finite,512);
    need(!count(finite,"material.wood") && !count(finite,"material.fire") && !finite.published_world()->active_chunks,"finite isolated combustion finishes and sleeps");
    auto before=*finite.capture_snapshot();static_cast<void>(finite.take_changes());ticks(finite,100);auto after=*finite.capture_snapshot();after.tick=before.tick;
    need(before==after && finite.take_changes().dirty_chunks.empty(),"finished isolated fire has no hidden state or texture churn");
    const auto immortal=catalog([](auto& d){d[1].initially_burning=true;d[1].fire_hp=-1;d[1].requires_oxygen=false;});GameSession alive(immortal);s=blank(*immortal);put(s,*immortal,{0,64},"material.wood");restore(alive,s);ticks(alive,100);
    need(count(alive,"material.wood")==1 && alive.world()->cell_state({0,64})->burning && alive.world()->cell_state({0,64})->fuel==600,"negative fire_hp and oxygen-free burning do not consume a fictitious common fuel countdown");
    CombustionRandom a(5),b(5);for(int i=0;i<1000;++i)need(a.next()%90001==b.next()%1000001,"15-bit source sampling is not replaced with a uniform million-value random generator");
    std::cout<<"sampled early/survived threshold="<<early<<'/'<<survived<<'\n';
}
WorldSnapshot body_fixture(const ContentRegistry& c){
    auto s=blank(c);for(int x=-8;x<8;++x)put(s,c,{x,78},"material.stone");
    RigidBodyState body;body.id=1;body.x=0;body.y=64;s.next_body_id=2;
    const auto material=static_cast<std::uint16_t>(1+std::find(s.material_ids.begin(),s.material_ids.end(),"material.wood_prop")-s.material_ids.begin());
    for(int x=-1;x<=1;++x)body.pixels.push_back({{x,0},material,MaterialSimulation::initial_state(*c.find_material("material.wood_prop"))});
    s.bodies={body};return s;
}
void rigid(){
    auto c=catalog([](auto& d){d[2].initially_burning=true;d[2].fire_hp=9;d[2].burn_heat=0;});
    GameSession body(c,make_box2d_solver());auto s=body_fixture(*c);restore(body,s);ticks(body,8);
    for(const auto& p:body.world()->bodies()[0].pixels)need(p.state.burning_ticks==8 && p.state.fire_color==0xff7f7f7f && p.state.fuel==600,"body increments 16-bit counter and fades RGB every eight updates without decrementing source fire_hp");
    const auto running=*body.capture_snapshot();ticks(body,24);need(body.world()->bodies().size()==1,"body removal waits for the project-mapped global check, not a fixed fuel countdown");ticks(body);
    need(body.world()->bodies().empty() && !count(body,"material.smoke") && !count(body,"material.fire"),"body burn-through removes actual pixels, without fabricated universal ash/smoke products");
    restore(body,running);const auto& pose=body.world()->bodies()[0];const CellPos target{static_cast<int>(std::floor(pose.x)),static_cast<int>(std::floor(pose.y))};
    need(bool(body.submit({CommandKind::cool,target,2})),"queue body extinguish");ticks(body);
    for(const auto& p:body.world()->bodies()[0].pixels)need(!p.state.burning && p.state.burning_ticks==8 && p.state.fire_color==0xff7f7f7f,"direct extinguishing preserves prior body count and scorch");
    c=catalog([](auto& d){d[2].initially_burning=true;d[2].fire_hp=-1;d[2].burn_heat=0;});GameSession wrapped(c,make_box2d_solver());s=body_fixture(*c);for(auto& p:s.bodies[0].pixels)p.state.burning_ticks=65535;restore(wrapped,s);ticks(wrapped);
    need(wrapped.world()->bodies()[0].pixels[0].state.burning_ticks==0,"body counter retains recovered uint16 wrap semantics");
}
void saved(){
    const auto c=catalog([](auto& d){d[2].initially_burning=true;d[2].requires_oxygen=true;d[2].generates_flames=101;d[2].burn_heat=50;});
    GameSession a(c,make_box2d_solver()),b(c,make_box2d_solver());restore(a,body_fixture(*c));ticks(a,11);const auto snap=*a.capture_snapshot();
    need(std::any_of(snap.bodies[0].pixels.begin(),snap.bodies[0].pixels.end(),[](const auto& p){return p.state.burning_ticks && p.state.fire_color && p.state.fire_intensity;}),"file fixture contains actual intensity, counter and scorched color");
    const auto path=std::filesystem::temp_directory_path()/("PixelSandbox-combustion-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".psw");
    const auto write=persistence::save_world_file(path,snap);need(write.ok,write.error);const auto read=persistence::load_world_file(path);std::error_code error;std::filesystem::remove(path,error);
    need(read.snapshot && *read.snapshot==snap,"format 12 preserves all active combustion state");restore(b,*read.snapshot);
    for(int t=0;t<120;++t){ticks(a);ticks(b);need(a.capture_snapshot()==b.capture_snapshot(),"restored random-stream/body combustion future matches each fixed step");}
    const auto before=a.capture_snapshot();auto bad=*before;bad.states[index(bad,{0,64})].burning=false;bad.states[index(bad,{0,64})].ignition_pending=true;
    need(!a.restore_world(bad) && before==a.capture_snapshot(),"invalid pending ignition cannot replace the current world");
}
}
int main(int argc,char** argv){try{
    const std::string name=argc==2?argv[1]:"";
    if(name=="combustion_ignition")ignition();else if(name=="combustion_emission")emission();else if(name=="combustion_burnout")burnout();else if(name=="rigid_combustion")rigid();else if(name=="combustion_save")saved();else throw std::runtime_error("Unknown combustion test");
    std::cout<<"PASS "<<name<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
