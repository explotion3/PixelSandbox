#include "session/game_session.hpp"
#include "physics/box2d_solver.hpp"
#include "persistence/world_file.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace pixel;
namespace {
void need(bool b,const std::string& message){if(!b)throw std::runtime_error(message);}
std::shared_ptr<const ContentRegistry> content(){
    std::vector<MaterialDefinition> defs;
    MaterialDefinition stone;stone.id="material.stone";stone.display_name="Stone";defs.push_back(stone);
    auto wood=stone;wood.id="material.wood";wood.display_name="Wood";wood.source_id="wood_static";wood.density=2;wood.fuel=90000;wood.flame_output=wood.smoke_output=wood.burn_output="air";defs.push_back(wood);
    wood.id="material.wood_prop";wood.source_id="wood_prop";wood.cell_type="solid";wood.is_static=false;wood.sand=false;defs.push_back(wood);
    auto sand=stone;sand.id="material.sand";sand.display_name="Sand";sand.is_static=false;sand.density=6;defs.push_back(sand);
    auto result=ContentRegistry::build(std::move(defs));need(bool(result),result.error);return result.registry;
}
WorldSnapshot blank(){
    WorldSnapshot s;s.bounds={{-32,32},96,96};s.material_lab=true;s.material_ids={"material.stone","material.wood","material.wood_prop","material.sand"};s.cells.assign(96*96,0);s.states.resize(s.cells.size());return s;
}
void put(WorldSnapshot& s,CellPos p,std::uint16_t m){
    const auto i=static_cast<std::size_t>(p.y-s.bounds.origin.y)*s.bounds.width+static_cast<std::size_t>(p.x-s.bounds.origin.x);
    if(s.cells[i]==1)--s.initial_stone;s.cells[i]=m;if(m==1)++s.initial_stone;s.states[i]={};if(m==2 || m==3)s.states[i].fuel=90000;
}
void floor(WorldSnapshot& s,int y){for(int x=-32;x<64;++x)for(int yy=y;yy<y+2;++yy)put(s,{x,yy},1);}
void restore(GameSession& s,const WorldSnapshot& snap){const auto result=s.restore_world(snap);need(result.ok,result.error);need(bool(s.resume()),"resume fixture");}
void ticks(GameSession& s,int n){for(int i=0;i<n;++i)need(s.advance_fixed_step(),"advance rigid step");}
CommandResult command(GameSession& s,WorldCommand c){need(bool(s.submit(std::move(c))),"queue rigid command");ticks(s,1);return s.command_results().back();}
void rigid_motion(){
    auto c=content();GameSession s(c,make_box2d_solver());auto snap=blank();floor(snap,110);restore(s,snap);
    auto result=command(s,{CommandKind::paint,{0,64},3,"material.wood_prop"});need(result.ok && result.amount==29,"painting a real solid definition creates 29 body-owned pixels");
    need(s.world()->bodies().size()==1 && s.world()->material_count(s.world()->material_index("material.wood_prop"))==29,"one native owner accounts for the rigid material");
    need(s.world()->material_at({0,64})==Material::air && s.world()->surface_material_at({0,64})==s.world()->material_index("material.wood_prop"),"surface query includes the body without copying its pixels into terrain");
    const auto blocked=command(s,{CommandKind::repair,{0,64}});need(!blocked.ok && blocked.message.find("rigid body")!=std::string::npos && s.published_world()->rigid_pixels==29,"grid repair cannot overwrite a rigid shape or consume material");
    const auto first=s.world()->bodies()[0];need(first.y>64.5,"real Box2D gravity advances on the command step");
    need(bool(s.pause()),"pause body");const auto paused=s.capture_snapshot();need(!s.advance_fixed_step() && s.capture_snapshot()==paused,"paused body and solver do not advance");need(bool(s.resume()),"resume body");
    ticks(s,240);const auto landed=s.world()->bodies()[0];need(landed.y>100 && landed.y<108 && std::abs(landed.vy)<0.2 && landed.sleeping,"pixel shape lands on actual terrain and sleeps");
    static_cast<void>(s.take_changes());ticks(s,12);need(!s.take_changes().bodies_changed,"sleeping body does not publish redundant motion batches");
    result=command(s,{CommandKind::push,{static_cast<int>(std::floor(landed.x)),static_cast<int>(std::floor(landed.y))},2});need(result.ok && result.amount>0,"existing push command reaches a rigid body");ticks(s,8);
    const auto pushed=s.world()->bodies()[0];need(pushed.x>landed.x+5 && std::abs(pushed.angle-landed.angle)>0.05,"the real solver translates and rotates after an impulse");
    need(s.world()->material_count(s.world()->material_index("material.wood_prop"))==29,"movement preserves all logical pixels despite raster rotation");
    s.end();need(bool(s.create_empty_world(44)) && s.world()->bodies().empty() && s.published_world()->rigid_pixels==0,"end/recreate releases the old physical world and material owner");
    GameSession no_solver(c);restore(no_solver,blank());need(!command(no_solver,{CommandKind::spawn_body,{0,64},2,"material.stone"}).ok,"runtime-only session explicitly rejects unavailable rigid dynamics");
}
void rigid_geometry(){
    const auto c=content();GameSession s(c,make_box2d_solver());auto snap=blank();floor(snap,110);
    for(int x=-6;x<=6;++x)put(snap,{x,63},2);restore(s,snap);
    need(command(s,{CommandKind::detach_body,{0,63},8}).ok,"detach actual connected terrain");
    need(s.world()->bodies().size()==1 && s.published_world()->rigid_pixels==13 && s.world()->material_at({0,63})==Material::air && s.world()->material_count(s.world()->material_index("material.wood"))==13,"detach transfers, rather than duplicates, the source material");
    const auto id=s.world()->bodies()[0].id;need(command(s,{CommandKind::erase,{0,63},1}).ok,"cut middle of flying bar");
    need(s.world()->bodies().size()==2 && s.published_world()->rigid_pixels==12,"one erased bridge pixel splits the remaining twelve into two moving shapes");
    need(s.world()->bodies()[0].id==id && s.world()->bodies()[1].id!=id,"fragments have unique persistent identities");
    need(s.world()->material_count(s.world()->material_index("material.wood"))==12 && !s.published_world()->held_stone && !s.published_world()->dropped_stone,"cutting has no duplicated material or inventory awards");
    ticks(s,240);for(const auto& b:s.world()->bodies())need(b.y<112 && b.y>100,"each rebuilt fragment collides with the real floor");
    // Cross-chunk support removal reaches the solver through collision revisions.
    snap=blank();floor(snap,110);for(int x=-5;x<=5;++x)put(snap,{x,70},1);restore(s,snap);
    need(command(s,{CommandKind::spawn_body,{0,58},2,"material.wood_prop"}).ok,"create support-removal body");ticks(s,180);const auto supported=s.world()->bodies()[0].y;
    need(supported<70 && supported>65,"body rests on the upper platform");need(command(s,{CommandKind::dig,{0,70},8}).ok,"dig actual grid support");ticks(s,30);
    need(s.world()->bodies()[0].y>supported+8 && s.published_world()->rigid_pixels==13,"changing terrain rebuilds collision and wakes the formerly resting body");
    // A real flat body blocks grid particles; the material remains in the grid layer.
    snap=blank();floor(snap,110);for(int y=90;y<93;++y)for(int x=-4;x<=4;++x)put(snap,{x,y},2);restore(s,snap);
    need(command(s,{CommandKind::detach_body,{0,91},5}).ok,"detach flat particle barrier");ticks(s,180);
    snap=*s.capture_snapshot();put(snap,{0,100},4);restore(s,snap);ticks(s,60);
    bool sand_on_body=false;for(int y=100;y<110;++y)if(s.world()->material_at({0,y})==s.world()->material_index("material.sand"))sand_on_body=y<s.world()->bodies()[0].y && !s.world()->body_pixel_at({0,y});
    need(sand_on_body && s.world()->material_count(s.world()->material_index("material.sand"))==1,"surface query blocks falling sand without duplicating or deleting it");
    // Continuous collision protects a one-cell wall at the highest supported test speed.
    snap=blank();floor(snap,110);for(int y=32;y<110;++y)put(snap,{20,y},1);restore(s,snap);
    need(command(s,{CommandKind::spawn_body,{0,64},2,"material.wood_prop"}).ok,"create continuous-collision body");snap=*s.capture_snapshot();snap.bodies[0].vx=500;snap.bodies[0].sleeping=false;restore(s,snap);
    for(int i=0;i<20;++i){ticks(s,1);need(s.world()->bodies()[0].x<20,"fast pixel body cannot tunnel through one-cell terrain");}
    // A capacity failure leaves local shape data intact; the physical step may still advance.
    snap=blank();floor(snap,110);snap.next_body_id=65;
    for(std::uint32_t i=0;i<64;++i){RigidBodyState b;b.id=i+1;b.x=-23.5+static_cast<int>(i%8)*10;b.y=40.5+static_cast<int>(i/8)*8;RigidPixel p;p.material=2;p.state.fuel=90000;b.pixels={p};if(!i){b.pixels.clear();for(int x=-2;x<=2;++x){p.local={x,0};b.pixels.push_back(p);}}snap.bodies.push_back(b);}
    restore(s,snap);const auto failed=command(s,{CommandKind::erase,{-24,40},1});
    need(!failed.ok && s.world()->bodies().size()==64 && s.world()->bodies()[0].pixels.size()==5 && s.published_world()->rigid_pixels==68,"fragment capacity failure has no partial deletion");
}
struct Temp {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("PixelSandbox-rigid-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){std::filesystem::create_directories(path);}
    ~Temp(){std::error_code e;const auto parent=std::filesystem::weakly_canonical(path.parent_path(),e);if(!e && parent==std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),e) && path.filename().string().starts_with("PixelSandbox-rigid-tests-"))std::filesystem::remove_all(path,e);}
};
void rigid_persistence(){
    const auto c=content();GameSession s(c,make_box2d_solver());auto snap=blank();floor(snap,110);restore(s,snap);need(command(s,{CommandKind::spawn_body,{0,60},3,"material.wood_prop"}).ok,"create saved body");ticks(s,12);need(bool(s.pause()),"pause airborne body");const auto saved=*s.capture_snapshot();
    Temp temp;const auto file=temp.path/"rigid.psw";need(persistence::save_world_file(file,saved).ok,"save rigid format 8");const auto loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==saved,"explicit rigid pose, velocity, material, fuel and shape survive real file roundtrip");
    auto defs=c->materials();std::reverse(defs.begin(),defs.end());const auto built=ContentRegistry::build(defs);need(bool(built),built.error);GameSession other(built.registry,make_box2d_solver());need(other.restore_world(*loaded.snapshot).ok && other.capture_snapshot()==saved,"registry reorder restores exact saved body state before resuming");
    need(bool(s.resume()) && bool(other.resume()),"resume both physical worlds");ticks(s,10);ticks(other,10);need(std::abs(s.world()->bodies()[0].y-other.world()->bodies()[0].y)<0.01,"reconstructed solver follows saved airborne velocity");ticks(s,180);ticks(other,180);
    need(std::abs(s.world()->bodies()[0].y-other.world()->bodies()[0].y)<0.3 && other.published_world()->rigid_pixels==29,"restored body lands with the same material quantity (contact caches are not serialized)");
    const auto before=s.capture_snapshot();auto bad=saved;bad.bodies[0].x=std::numeric_limits<double>::quiet_NaN();need(!s.restore_world(bad) && s.capture_snapshot()==before,"NaN pose rejected without replacing current world");
    bad=saved;bad.bodies[0].x=bad.bounds.origin.x-2;need(!s.restore_world(bad) && s.capture_snapshot()==before,"out-of-world material rejected even when the body origin passes broad pose bounds");
    bad=saved;bad.bodies.push_back(bad.bodies[0]);need(!s.restore_world(bad) && s.capture_snapshot()==before,"duplicate body identities rejected atomically");bad=saved;bad.bodies[0].pixels.push_back(bad.bodies[0].pixels[0]);need(!s.restore_world(bad),"duplicate local material pixels rejected");
    GameSession no_solver(c);restore(no_solver,blank());const auto empty=no_solver.capture_snapshot();need(!no_solver.restore_world(saved) && no_solver.capture_snapshot()==empty,"missing solver rejects rigid save and retains old world");
    // Historical version 7, with wide fuel but no rigid trailer.
    auto legacy=blank();put(legacy,{0,64},2);legacy.states[static_cast<std::size_t>(64-32)*96+32].fuel=80000;
    std::vector<unsigned char> bytes;auto put_bytes=[&](std::uint64_t value,int count){for(int i=0;i<count;++i)bytes.push_back(static_cast<unsigned char>(value>>(i*8)));};
    put_bytes(legacy.seed,8);put_bytes(legacy.tick,8);put_bytes(static_cast<std::uint32_t>(legacy.bounds.origin.x),4);put_bytes(legacy.bounds.origin.y,4);put_bytes(96,4);put_bytes(96,4);put_bytes(64,4);put_bytes(8,4);put_bytes(legacy.initial_stone,8);put_bytes(legacy.material_ids.size(),4);
    for(const auto& id2:legacy.material_ids){put_bytes(id2.size(),4);bytes.insert(bytes.end(),id2.begin(),id2.end());}put_bytes(legacy.cells.size(),4);for(const auto m:legacy.cells)put_bytes(m,2);put_bytes(0,4);put_bytes(0,8);put_bytes(1,1);put_bytes(6,4);
    for(const auto& state:legacy.states){put_bytes(state.temperature,4);put_bytes(state.life,2);put_bytes(state.fuel,4);put_bytes(state.burning,1);put_bytes(state.created_tick,8);put_bytes(state.velocity_x,4);put_bytes(state.velocity_y,4);put_bytes(state.vertical_fraction,2);put_bytes(state.horizontal_fraction,2);put_bytes(state.mobilized,1);put_bytes(state.quiet_ticks,2);put_bytes(state.adhesion_ticks,2);put_bytes(state.adhesion_cooldown,2);put_bytes(state.detonation_ticks,2);put_bytes(state.electrified_ticks,2);}put_bytes(0,4);
    const auto payload=bytes;std::uint64_t hash=14695981039346656037ULL;for(const auto value:payload){hash^=value;hash*=1099511628211ULL;}bytes={'P','S','B','W','A','L','L',0};put_bytes(7,4);put_bytes(payload.size(),8);put_bytes(hash,8);bytes.insert(bytes.end(),payload.begin(),payload.end());
    std::ofstream historical(file,std::ios::binary|std::ios::trunc);historical.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));historical.close();const auto migrated=persistence::load_world_file(file);need(migrated.snapshot && *migrated.snapshot==legacy,"real format 7 preserves wide fuel and starts with no bodies: "+migrated.error);
}
std::shared_ptr<const ContentRegistry> reactive_content(){
    auto defs=content()->materials();
    for(auto& d:defs)if(d.id=="material.wood_prop" || d.id=="material.wood"){
        d.fuel=5;d.requires_oxygen=true;d.ignition=60;d.burn_heat=100;d.burn_output="material.smoke";d.tags={"corrodible"};
    }
    MaterialDefinition smoke;smoke.id="material.smoke";smoke.display_name="Smoke";smoke.cell_type="gas";smoke.sand=false;smoke.is_static=false;smoke.density=0.1f;smoke.lifetime=40;defs.push_back(smoke);
    auto water=defs[0];water.id="material.water";water.display_name="Water";water.sand=false;water.cooling=100;defs.push_back(water);
    auto ice=defs[0];ice.id="material.ice";ice.display_name="Ice";ice.warmth_threshold=50;ice.warmth_output="material.water";defs.push_back(ice);
    auto acid=water;acid.id="material.acid";acid.display_name="Acid";acid.cooling=0;defs.push_back(acid);
    auto result=ContentRegistry::build(std::move(defs),{{"material.acid","[corrodible]","keep","material.smoke",1}});need(bool(result),result.error);return result.registry;
}
WorldSnapshot reactive_blank(const ContentRegistry& c){auto s=blank();s.material_ids.clear();s.material_ids.push_back("material.stone");for(const auto& d:c.materials())if(d.id!="material.stone")s.material_ids.push_back(d.id);return s;}
std::uint16_t slot(const WorldSnapshot& s,const std::string& id){return static_cast<std::uint16_t>(std::find(s.material_ids.begin(),s.material_ids.end(),id)-s.material_ids.begin()+1);}
RigidBodyState& add_body(WorldSnapshot& s,const std::string& id,std::vector<CellPos> shape,CellPos at){
    RigidBodyState b;b.id=s.next_body_id++;b.x=at.x+0.5;b.y=at.y+0.5;const auto m=slot(s,id);
    for(const auto p:shape){RigidPixel pixel;pixel.local=p;pixel.material=m;if(id=="material.wood_prop")pixel.state.fuel=5;b.pixels.push_back(pixel);}
    s.bodies.push_back(std::move(b));return s.bodies.back();
}
void rigid_thermal(){
    const auto c=reactive_content();GameSession s(c,make_box2d_solver());auto snap=reactive_blank(*c);floor(snap,110);restore(s,snap);
    need(command(s,{CommandKind::spawn_body,{0,64},1,"material.wood_prop"}).ok,"spawn reactive wood");
    need(command(s,{CommandKind::heat,{0,64},2}).ok,"heat reaches actual body pixels");
    std::uint32_t fuel=0,burning=0;for(const auto& p:s.world()->bodies()[0].pixels){fuel+=p.state.fuel;burning+=p.state.burning;}
    need(fuel==21 && burning==4,"only four oxygen-exposed pixels burn once; center cannot breathe through grid air underneath a body");
    ticks(s,20);need(s.published_world()->rigid_pixels==0 && s.world()->material_count(s.world()->material_index("material.smoke"))==5,"each of five body pixels burns into exactly one grid product, without duplicate rewards");
    // A burning center that loses oxygen is extinguished, not allowed to burn internally.
    snap=reactive_blank(*c);floor(snap,110);std::vector<CellPos> square;for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)square.push_back({x,y});
    auto& sealed=add_body(snap,"material.wood_prop",square,{0,64});sealed.pixels[4].state.burning=true;sealed.pixels[4].state.temperature=100;restore(s,snap);ticks(s,1);
    need(!s.world()->bodies()[0].pixels[4].state.burning && s.world()->bodies()[0].pixels[4].state.fuel==5,"sealed center stops burning without consuming fuel");
    // Water cooling and explicit cool both use the same native thermal state.
    snap=reactive_blank(*c);floor(snap,110);auto& wet=add_body(snap,"material.wood_prop",{{0,0}},{0,64});wet.pixels[0].state.burning=true;wet.pixels[0].state.temperature=100;
    put(snap,{-1,64},slot(snap,"material.water"));restore(s,snap);ticks(s,1);
    need(!s.world()->bodies()[0].pixels[0].state.burning && s.world()->bodies()[0].pixels[0].state.fuel==5,"real grid water cools and extinguishes an adjacent body");
    need(command(s,{CommandKind::cool,{0,64},1}).ok && !s.world()->bodies()[0].pixels[0].state.burning,"cool tool reaches rigid pixels");
    // Ice releases into the grid, including the body's point velocity.
    snap=reactive_blank(*c);floor(snap,110);auto& ice=add_body(snap,"material.ice",{{0,0},{1,0},{2,0}},{0,64});ice.vx=60;restore(s,snap);
    need(command(s,{CommandKind::heat,{1,64},3}).ok,"heat a whole ice body");
    const auto melted=s.world()->material_count(s.world()->material_index("material.water"));
    need(melted>0 && s.published_world()->rigid_pixels+melted==3,"melting preserves all three pixels across body and grid; new cooling water can quench adjacent ice");
    for(int x=0;x<3;++x)if(s.world()->material_at({x,64})==s.world()->material_index("material.water"))need(s.world()->cell_state({x,64})->velocity_x==256,"phase release inherits body speed in grid Q8 per tick");
    snap=reactive_blank(*c);floor(snap,110);put(snap,{0,64},slot(snap,"material.wood"));
    auto& hot_state=snap.states[static_cast<std::size_t>(64-32)*96+32];hot_state.fuel=4;hot_state.temperature=100;hot_state.burning=true;restore(s,snap);
    need(command(s,{CommandKind::detach_body,{0,64},1}).ok && s.published_world()->rigid_pixels==1 && s.world()->material_at({0,64})==Material::air &&
        s.world()->bodies()[0].pixels[0].state.fuel==3 && s.world()->bodies()[0].pixels[0].state.burning,"detaching burning terrain transfers existing heat and fuel, then burns once without reinitializing or duplicating it");
    // The old stone-only fast path must not skip hot body state.
    WorldSnapshot only_stone;only_stone.bounds={{-32,32},96,96};only_stone.cells.resize(96*96,0);only_stone.states.resize(96*96);
    auto& hot_stone=add_body(only_stone,"material.stone",{{0,0}},{0,64});hot_stone.pixels[0].state.temperature=50;only_stone.initial_stone=1;restore(s,only_stone);ticks(s,1);
    need(s.world()->bodies()[0].pixels[0].state.temperature<50,"stone-only mapping still advances thermal state of an actual body");
}
void rigid_reactions(){
    const auto c=reactive_content();GameSession s(c,make_box2d_solver());auto snap=reactive_blank(*c);floor(snap,110);
    add_body(snap,"material.wood_prop",{{-1,0},{0,0},{1,0}},{0,63});put(snap,{0,64},slot(snap,"material.acid"));restore(s,snap);ticks(s,1);
    need(s.published_world()->rigid_bodies==2 && s.published_world()->rigid_pixels==2,"acid removes the actual cross-chunk bridge pixel and splits the two surviving pieces");
    need(s.world()->material_count(s.world()->material_index("material.smoke"))==1 && s.world()->material_count(s.world()->material_index("material.acid"))==1,"contact commits both configured outputs once and keeps the catalyst");
    need(s.world()->cell_state({0,63})->life==39,"a body contact product enters the following thermal phase once, just like a grid contact product");
    ticks(s,1);need(s.published_world()->rigid_pixels==2 && s.world()->material_count(s.world()->material_index("material.smoke"))==1,"repeated contact cannot corrode a pixel already removed");
    // All capacity checks precede the two-reagent commit.
    snap=reactive_blank(*c);floor(snap,110);add_body(snap,"material.wood_prop",{{-1,0},{0,0},{1,0}},{0,63});put(snap,{0,64},slot(snap,"material.acid"));
    for(int i=0;i<63;++i)add_body(snap,"material.wood_prop",{{0,0}},{-24+(i%8)*10,40+(i/8)*9});restore(s,snap);ticks(s,1);
    need(s.published_world()->rigid_bodies==64 && s.published_world()->rigid_pixels==66 && s.world()->bodies()[0].pixels.size()==3 &&
        s.world()->material_count(s.world()->material_index("material.smoke"))==0 && s.published_world()->deferred_reactions>0,"fragment capacity defers the entire acid reaction without material loss or a partial reagent change");
    // A product cannot overwrite occupied grid data or disappear when release is blocked.
    snap=reactive_blank(*c);floor(snap,110);add_body(snap,"material.ice",{{0,0}},{0,64});put(snap,{0,64},slot(snap,"material.water"));restore(s,snap);
    need(command(s,{CommandKind::heat,{0,64},2}).ok && s.published_world()->rigid_pixels==1 && s.world()->material_count(s.world()->material_index("material.water"))==1 && s.published_world()->deferred_reactions>0,"occupied release cell preserves both material owners and reports deferral");
    // The shared matcher also handles three reagents across the two owners.
    ReactionDefinition triple{"material.acid","material.wood_prop","keep","material.smoke",1};triple.input_c="material.wood_prop";triple.output_c="material.stone";
    auto built=ContentRegistry::build(c->materials(),{triple});need(bool(built),built.error);GameSession three(built.registry,make_box2d_solver());
    snap=reactive_blank(*built.registry);floor(snap,110);const auto stone_before=snap.initial_stone;
    add_body(snap,"material.wood_prop",{{-1,0},{0,0},{1,0}},{0,63});put(snap,{0,64},slot(snap,"material.acid"));restore(three,snap);ticks(three,1);
    need(three.published_world()->rigid_pixels==2 && three.world()->material_count(three.world()->material_index("material.smoke"))==1 &&
        three.world()->material_count(three.world()->material_index("material.wood_prop"))==1 && three.published_world()->initial_stone==stone_before+1,
        "three distinct body/grid reagents commit together; a solid product remains body-owned and updates the stone ledger");
    // Restricted blobs gather the actual body-local connected shape before releasing.
    ReactionDefinition blob{"material.acid","material.wood_prop","keep","material.smoke",1};blob.blob_radius_b=2;
    built=ContentRegistry::build(c->materials(),{blob});need(bool(built),built.error);GameSession region(built.registry,make_box2d_solver());
    snap=reactive_blank(*built.registry);floor(snap,110);add_body(snap,"material.wood_prop",{{-2,0},{-1,0},{0,0},{1,0},{2,0}},{0,63});put(snap,{0,64},slot(snap,"material.acid"));restore(region,snap);ticks(region,1);
    need(region.published_world()->rigid_pixels==0 && region.world()->material_count(region.world()->material_index("material.smoke"))==5,"one restricted blob transfers five actual body pixels exactly once");
}
void rigid_thermal_persistence(){
    const auto c=reactive_content();GameSession s(c,make_box2d_solver());auto snap=reactive_blank(*c);floor(snap,110);restore(s,snap);
    need(command(s,{CommandKind::spawn_body,{0,64},1,"material.wood_prop"}).ok && command(s,{CommandKind::heat,{0,64},2}).ok,"create live burning rigid save");
    need(bool(s.pause()),"pause burning body");const auto saved=*s.capture_snapshot();need(!s.advance_fixed_step() && s.capture_snapshot()==saved,"paused body thermal state does not advance");
    Temp temp;const auto file=temp.path/"burning.psw";need(persistence::save_world_file(file,saved).ok,"write body thermal state");auto loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==saved,"format 8 retains full burning body fuel, temperature, shape and tick");
    auto defs=c->materials();std::reverse(defs.begin(),defs.end());auto reordered=ContentRegistry::build(std::move(defs),c->reactions());need(bool(reordered),reordered.error);
    GameSession other(reordered.registry,make_box2d_solver());need(other.restore_world(*loaded.snapshot).ok && other.capture_snapshot()==saved,"reordered registry restores live rigid fire without resetting fuel");
    need(bool(s.resume()) && bool(other.resume()),"resume both burning worlds");ticks(s,20);ticks(other,20);
    need(!s.published_world()->rigid_pixels && !other.published_world()->rigid_pixels && s.world()->material_count(s.world()->material_index("material.smoke"))==5 && other.world()->material_count(other.world()->material_index("material.smoke"))==5,"restored burning bodies finish into the same five grid products");
    const auto current=s.capture_snapshot();auto bad=saved;bad.bodies[0].pixels[0].state.fuel=0;bad.bodies[0].pixels[0].state.burning=true;
    need(!s.restore_world(bad) && s.capture_snapshot()==current,"invalid live-body fuel fails atomically");
    bad=saved;bad.bodies[0].pixels[0].state.fuel=6;need(!s.restore_world(bad) && s.capture_snapshot()==current,"over-definition rigid fuel fails atomically");
    bad=saved;auto& pixels=bad.bodies[0].pixels;pixels.erase(std::remove_if(pixels.begin(),pixels.end(),[](const auto& p){return p.local==CellPos{0,0};}),pixels.end());need(!s.restore_world(bad) && s.capture_snapshot()==current,"disconnected body save requires separate fragment identities");
}
std::shared_ptr<const ContentRegistry> explosion_content(ExplosionDefinition blast={}){
    auto defs=content()->materials();for(auto& d:defs){d.hit_points=d.id=="material.stone"?100000:5000;d.durability=d.id=="material.stone"?10:8;}
    MaterialDefinition smoke;smoke.id="material.smoke";smoke.display_name="Smoke";smoke.cell_type="gas";smoke.sand=false;smoke.is_static=false;smoke.density=0.1f;smoke.lifetime=40;defs.push_back(smoke);
    auto shield=defs[0];shield.id="material.shield";shield.durability=100;shield.hit_points=1000000;defs.push_back(shield);
    auto charge=defs[2];charge.id="material.charge";charge.source_id="charge";charge.fuel=12;charge.ignition=20;charge.explosion.radius=2;charge.explosion.ray_energy=1000000;charge.explosion.heat=0;charge.explosion.impulse_q8=0;charge.explosion.delay_ticks=3;charge.explosion.trigger_energy=10;charge.explosion.output="material.smoke";charge.explosion.fire_material="air";defs.push_back(charge);
    auto grid=charge;grid.id="material.grid_charge";grid.source_id="grid_charge";grid.cell_type="liquid";grid.sand=true;grid.is_static=true;defs.push_back(grid);
    auto profile=charge;profile.id="material.blast_profile";profile.source_id="blast_profile";profile.explosion=blast;profile.explosion.radius=4;defs.push_back(profile);
    auto result=ContentRegistry::build(std::move(defs));need(bool(result),result.error);return result.registry;
}
void rigid_blast(){
    ExplosionDefinition profile;profile.heat=0;profile.fire_material="air";profile.ray_energy=1000000;
    auto c=explosion_content(profile);GameSession s(c,make_box2d_solver());auto snap=reactive_blank(*c);floor(snap,110);
    std::vector<CellPos> bar;for(int x=-6;x<=6;++x)bar.push_back({x,0});add_body(snap,"material.wood_prop",bar,{0,64});restore(s,snap);
    need(command(s,{CommandKind::blast,{0,64},1,"material.blast_profile"}).ok,"blast reaches a real body through the normal session command");
    need(s.published_world()->rigid_pixels==4 && s.published_world()->rigid_bodies==2 && s.published_world()->blast_destroyed==9,"nine actual solid pixels are removed across the chunk boundary despite sand=false and destroy_liquids=false");
    for(const auto& body:s.world()->bodies()){double x=0;for(const auto& p:body.pixels)x+=p.local.x;need(x<0?body.vx<0:body.vx>0,"fractured pieces receive their own outward impulse, rather than cancelling on the former parent");}
    need(!s.published_world()->held_stone && !s.published_world()->dropped_stone,"explosion destruction creates no duplicate inventory rewards");
    const auto saved=*s.capture_snapshot();Temp temp;const auto file=temp.path/"blast.psw";need(persistence::save_world_file(file,saved).ok,"save actual blast fragments");const auto loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==saved,"blast geometry and fragment velocities survive explicit file roundtrip");
    GameSession other(c,make_box2d_solver());need(other.restore_world(*loaded.snapshot).ok && other.capture_snapshot()==saved,"restored blast has exact public material and motion state");
    // Non-excavating pressure translates/rotates a real compound shape.
    profile.hole_enabled=false;c=explosion_content(profile);GameSession pressure(c,make_box2d_solver());snap=reactive_blank(*c);floor(snap,110);
    add_body(snap,"material.wood_prop",{{0,-2},{0,-1},{0,0},{0,1},{0,2}},{0,64});restore(pressure,snap);need(command(pressure,{CommandKind::blast,{-4,62},2,"material.blast_profile"}).ok,"apply nondestructive pressure");
    need(pressure.published_world()->rigid_pixels==5 && pressure.published_world()->blast_destroyed==0 && pressure.world()->bodies()[0].vx>0 && std::abs(pressure.world()->bodies()[0].angular_velocity)>0.001,"off-center exposed pressure conserves material and produces both linear and angular velocity");
    const auto x=pressure.world()->bodies()[0].x;ticks(pressure,5);need(pressure.world()->bodies()[0].x>x,"the real Box2D solver continues blast motion");
    // Same exposed area, tenfold density: tenfold resistance to impulse.
    snap=reactive_blank(*c);add_body(snap,"material.wood_prop",{{0,0}},{0,64});GameSession light(c,make_box2d_solver());restore(light,snap);command(light,{CommandKind::blast,{-4,64},2,"material.blast_profile"});
    auto heavy_defs=c->materials();for(auto& d:heavy_defs)if(d.id=="material.wood_prop")d.density*=10;auto heavy_registry=ContentRegistry::build(std::move(heavy_defs));need(bool(heavy_registry),heavy_registry.error);
    GameSession heavy(heavy_registry.registry,make_box2d_solver());restore(heavy,snap);command(heavy,{CommandKind::blast,{-4,64},2,"material.blast_profile"});
    need(light.world()->bodies()[0].vx>heavy.world()->bodies()[0].vx*9 && light.world()->bodies()[0].vx<heavy.world()->bodies()[0].vx*11,"pressure uses actual density/mass rather than a fixed body velocity increment");
    profile.heat=240;profile.impulse_q8=0;c=explosion_content(profile);GameSession heat(c,make_box2d_solver());snap=reactive_blank(*c);add_body(snap,"material.wood_prop",{{0,0}},{0,64}).pixels[0].state.fuel=90000;restore(heat,snap);command(heat,{CommandKind::blast,{-2,64},1,"material.blast_profile"});
    need(heat.world()->bodies()[0].pixels[0].state.burning && heat.world()->bodies()[0].pixels[0].state.fuel==89999,"blast heat enters the shared body combustion phase exactly once");
}
void rigid_blast_occlusion(){
    ExplosionDefinition p;p.heat=240;p.fire_material="air";p.ray_energy=1000000;auto c=explosion_content(p);GameSession s(c,make_box2d_solver());auto snap=reactive_blank(*c);
    for(int y=32;y<128;++y)put(snap,{0,y},slot(snap,"material.shield"));add_body(snap,"material.wood_prop",{{0,0}},{2,64});restore(s,snap);command(s,{CommandKind::blast,{-3,64},2,"material.blast_profile"});
    need(s.published_world()->rigid_pixels==1 && s.world()->bodies()[0].pixels[0].state.temperature==0 && s.world()->bodies()[0].vx==0,"a high-durability grid wall blocks body damage, heat and impulse");
    // A rotated physical shield blocks both owners on its far side.
    snap=reactive_blank(*c);std::vector<CellPos> vertical;for(int y=-30;y<=30;++y)vertical.push_back({0,y});auto& shield=add_body(snap,"material.shield",vertical,{0,64});shield.angle=0.3;
    put(snap,{2,64},slot(snap,"material.wood"));add_body(snap,"material.wood_prop",{{0,0}},{3,64});restore(s,snap);command(s,{CommandKind::blast,{-3,64},2,"material.blast_profile"});
    need(s.world()->material_at({2,64})==s.world()->material_index("material.wood") && s.world()->cell_state({2,64})->temperature==0 && s.world()->bodies().back().pixels[0].state.temperature==0 && s.world()->bodies().back().vx==0,"oriented body pixels shield grid and body materials using the same pre-blast rays");
    need(s.published_world()->rigid_pixels==62 && s.world()->bodies()[0].vx>0,"an indestructible physical shield can receive pressure without being destroyed");
    // Two physical pixels can share a raster projection; both must be hit once.
    p.heat=0;p.impulse_q8=0;c=explosion_content(p);GameSession alias(c,make_box2d_solver());snap=reactive_blank(*c);auto& rotated=add_body(snap,"material.wood_prop",{{0,0},{1,0}},{0,64});rotated.x=0.1;rotated.y=64.1;rotated.angle=0.7853981633974483;restore(alias,snap);command(alias,{CommandKind::blast,{0,64},1,"material.blast_profile"});
    need(alias.published_world()->rigid_pixels==0 && alias.published_world()->blast_destroyed==2,"oriented ray intersections remove both actual pixels once, without relying on one raster center query");
    p.ray_energy=8000;p.heat=240;c=explosion_content(p);GameSession weak(c,make_box2d_solver());snap=reactive_blank(*c);add_body(snap,"material.wood_prop",{{0,0},{1,0},{2,0},{3,0}},{0,64});put(snap,{5,64},slot(snap,"material.wood"));restore(weak,snap);command(weak,{CommandKind::blast,{-3,64},2,"material.blast_profile"});
    need(weak.published_world()->rigid_pixels==3 && weak.world()->cell_state({5,64})->temperature==0,"body HP consumes ray energy and removal cannot open a shortcut for other rays in the same blast");
    // Capacity failure leaves terrain, fuel and body shape intact.
    p.ray_energy=1000000;p.heat=0;c=explosion_content(p);GameSession full(c,make_box2d_solver());snap=reactive_blank(*c);std::vector<CellPos> bar;for(int x=-6;x<=6;++x)bar.push_back({x,0});add_body(snap,"material.wood_prop",bar,{0,64});
    for(int i=0;i<63;++i)add_body(snap,"material.wood_prop",{{0,0}},{-24+(i%8)*10,38+(i/8)*10});put(snap,{0,65},1);restore(full,snap);const auto failed=command(full,{CommandKind::blast,{0,64},1,"material.blast_profile"});
    need(!failed.ok && failed.message.find("capacity")!=std::string::npos && full.published_world()->rigid_pixels==76 && full.world()->bodies()[0].pixels.size()==13 && full.world()->material_at({0,65})==Material::stone && full.published_world()->explosions==0,"fragment overflow rejects the whole blast before terrain or body writes");
    put(snap,{0,64},slot(snap,"material.grid_charge"));auto& armed=snap.states[static_cast<std::size_t>(64-32)*96+32];armed.fuel=12;armed.detonation_ticks=1;restore(full,snap);ticks(full,1);
    need(full.published_world()->rigid_pixels==76 && full.world()->bodies()[0].pixels.size()==13 && full.world()->material_at({0,64})==full.world()->material_index("material.grid_charge") && full.world()->cell_state({0,64})->detonation_ticks==1 && full.published_world()->explosions==0 && full.published_world()->deferred_reactions>0,"failed automatic blast retains grid fuel and its ready fuse instead of consuming residue before the capacity check");
}
void set_charge(WorldSnapshot& s,CellPos p,bool ready=false){put(s,p,slot(s,"material.grid_charge"));auto& state=s.states[static_cast<std::size_t>(p.y-s.bounds.origin.y)*s.bounds.width+static_cast<std::size_t>(p.x-s.bounds.origin.x)];state.fuel=12;if(ready)state.detonation_ticks=1;}
RigidBodyState& charge_body(WorldSnapshot& s,CellPos at,std::vector<CellPos> pixels,bool ready=false){auto& body=add_body(s,"material.charge",std::move(pixels),at);for(auto& p:body.pixels){p.state.fuel=12;if(ready)p.state.detonation_ticks=1;}return body;}
void rigid_fuses(){
    ExplosionDefinition p;p.heat=0;p.fire_material="air";auto c=explosion_content(p);GameSession s(c,make_box2d_solver());auto snap=reactive_blank(*c);charge_body(snap,{0,64},{{-1,0},{0,0},{1,0}});restore(s,snap);
    command(s,{CommandKind::heat,{0,64},3});for(const auto& pixel:s.world()->bodies()[0].pixels)need(pixel.state.detonation_ticks==3,"shared body heat latches a finite material fuse");
    need(bool(s.pause()),"pause body fuse");const auto armed=*s.capture_snapshot();need(!s.advance_fixed_step() && s.capture_snapshot()==armed,"paused body fuse cannot count down");
    Temp temp;const auto file=temp.path/"body-fuse.psw";need(persistence::save_world_file(file,armed).ok,"save live body fuse in existing format 8");const auto loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==armed,"actual file roundtrip retains every local fuse and fuel");
    auto reversed=c->materials();std::reverse(reversed.begin(),reversed.end());auto registry=ContentRegistry::build(std::move(reversed));need(bool(registry),registry.error);GameSession other(registry.registry,make_box2d_solver());need(other.restore_world(*loaded.snapshot).ok && other.capture_snapshot()==armed,"content reorder restores a live body fuse exactly");
    need(bool(s.resume()) && bool(other.resume()),"resume material fuses");ticks(s,2);ticks(other,2);need(s.published_world()->rigid_pixels==3 && other.published_world()->rigid_pixels==3,"body fuse does not detonate early");ticks(s,1);ticks(other,1);
    need(s.published_world()->rigid_pixels==0 && other.published_world()->rigid_pixels==0 && s.published_world()->explosions==1 && other.published_world()->explosions==1 && s.world()->material_count(s.world()->material_index("material.smoke"))==3 && other.world()->material_count(other.world()->material_index("material.smoke"))==3,"one connected body fuel cluster becomes three residues and one explosion after restore");
    snap=armed;restore(s,snap);command(s,{CommandKind::cool,{0,64},3});need(s.world()->bodies()[0].pixels[0].state.detonation_ticks==2 && !s.world()->bodies()[0].pixels[0].state.burning,"cooling extinguishes heat without cancelling a latched fuse");ticks(s,2);need(!s.published_world()->rigid_pixels && s.published_world()->explosions==1,"cold latched body fuse still detonates");
    snap=reactive_blank(*c);set_charge(snap,{0,64});snap.states[static_cast<std::size_t>(64-32)*96+32].detonation_ticks=2;restore(s,snap);
    need(command(s,{CommandKind::detach_body,{0,64},1}).ok && s.published_world()->rigid_pixels==1 && s.world()->material_at({0,64})==Material::air && s.world()->bodies()[0].pixels[0].state.detonation_ticks==1,"detaching armed terrain transfers the existing fuse and advances it once without resetting it");ticks(s,1);need(!s.published_world()->rigid_pixels && s.published_world()->explosions==1,"transferred fuse detonates through the same body phase");
    // A grid blast primes a body, and a body blast primes grid fuel, without recursion.
    snap=reactive_blank(*c);set_charge(snap,{-3,64},true);charge_body(snap,{0,64},{{0,0}});restore(s,snap);ticks(s,1);
    need(s.published_world()->rigid_pixels==1 && s.published_world()->explosions==1 && s.world()->bodies()[0].pixels[0].state.detonation_ticks==3,"grid pressure primes the body but cannot recursively detonate it");ticks(s,3);need(!s.published_world()->rigid_pixels && s.published_world()->explosions==1,"body waits for its own three subsequent steps");
    snap=reactive_blank(*c);charge_body(snap,{-3,64},{{0,0}},true);set_charge(snap,{0,64});restore(s,snap);ticks(s,1);
    need(s.world()->cell_state({0,64})->detonation_ticks==3 && s.world()->material_count(s.world()->material_index("material.grid_charge"))==1 && s.published_world()->explosions==1,"body pressure primes grid fuel without same-phase recursion");ticks(s,3);need(s.world()->material_count(s.world()->material_index("material.grid_charge"))==0 && s.published_world()->explosions==1,"grid fuel completes the reverse chain");
    // Fracture during an earlier grid explosion must not orphan already-ready body pixels.
    snap=reactive_blank(*c);auto& mixed=add_body(snap,"material.wood_prop",{{-3,0},{-2,0},{-1,0},{0,0},{1,0},{2,0},{3,0}},{0,64});
    for(auto& pixel:mixed.pixels)if(std::abs(pixel.local.x)==3){pixel.material=slot(snap,"material.charge");pixel.state.fuel=12;pixel.state.detonation_ticks=1;}
    set_charge(snap,{0,64},true);restore(s,snap);ticks(s,1);
    need(!s.published_world()->rigid_pixels && s.published_world()->explosions==3,"ready material identity follows child fragments through a preceding blast");
    // No space for residue: preserve both the ready fuel and the complete blast.
    snap=reactive_blank(*c);charge_body(snap,{0,64},{{0,0}},true);put(snap,{0,64},1);restore(s,snap);ticks(s,1);
    need(s.published_world()->rigid_pixels==1 && s.published_world()->explosions==0 && s.world()->bodies()[0].pixels[0].state.detonation_ticks==1 && s.world()->material_at({0,64})==Material::stone && s.published_world()->deferred_reactions>0,"blocked body residue keeps the ready fuel and old terrain without a partial explosion");
    // Grid and body events share one bounded batch; the deferred body can be saved.
    snap=reactive_blank(*c);for(int i=0;i<64;++i)set_charge(snap,{-24+(i%8)*10,40+(i/8)*10},true);charge_body(snap,{58,120},{{0,0}},true);restore(s,snap);ticks(s,1);
    need(s.published_world()->explosions==64 && s.published_world()->rigid_pixels==1 && s.world()->bodies()[0].pixels[0].state.detonation_ticks==1,"64 grid detonations leave the excess body event ready for the next step");
    const auto deferred=*s.capture_snapshot();need(persistence::save_world_file(file,deferred).ok,"save deferred body event");auto waiting=persistence::load_world_file(file);need(waiting.snapshot.has_value(),waiting.error);GameSession resume(c,make_box2d_solver());restore(resume,*waiting.snapshot);ticks(s,1);ticks(resume,1);need(!s.published_world()->rigid_pixels && !resume.published_world()->rigid_pixels && s.published_world()->explosions==1 && resume.published_world()->explosions==1,"deferred body fuse survives file reload and executes once");
    const auto current=s.capture_snapshot();auto bad=armed;bad.bodies[0].pixels[0].state.detonation_ticks=4;need(!s.restore_world(bad) && s.capture_snapshot()==current,"over-definition body fuse is rejected atomically");bad=armed;bad.bodies[0].pixels[0].material=slot(bad,"material.wood_prop");bad.bodies[0].pixels[0].state.fuel=12;need(!s.restore_world(bad) && s.capture_snapshot()==current,"non-explosive body fuse is rejected atomically");
}
std::shared_ptr<const ContentRegistry> rigid_electric_content(bool melting=false){
    ExplosionDefinition profile;profile.heat=0;profile.fire_material="air";auto defs=explosion_content(profile)->materials();MaterialDefinition metal;metal.id="material.metal";metal.source_id="electric_metal";metal.display_name="Metal";metal.cell_type="solid";metal.sand=false;metal.is_static=false;metal.density=8;metal.electrical_conductivity=true;metal.electric_cost=1;
    if(melting){metal.warmth_threshold=50;metal.warmth_output="material.molten";}defs.push_back(metal);
    auto wire=metal;wire.id="material.wire";wire.source_id="wire";wire.cell_type="liquid";wire.sand=true;wire.is_static=true;wire.warmth_output="";defs.push_back(wire);
    auto liquid=metal;liquid.id="material.molten";liquid.source_id="electric_molten";liquid.cell_type="liquid";liquid.gravity_q8=0;liquid.warmth_output="";defs.push_back(liquid);
    auto result=ContentRegistry::build(std::move(defs));need(bool(result),result.error);return result.registry;
}
ElectricPulse body_pulse(std::uint32_t id,CellPos p,std::uint16_t speed=1,std::uint16_t heat=0){ElectricPulse pulse;pulse.body=pulse.previous_body=id;pulse.position=pulse.previous=p;pulse.speed=speed;pulse.heat=heat;return pulse;}
std::vector<CellPos> horizontal(int from,int to){std::vector<CellPos> result;for(int x=from;x<=to;++x)result.push_back({x,0});return result;}
void rigid_electricity(){
    const auto c=rigid_electric_content();GameSession s(c,make_box2d_solver());auto snap=reactive_blank(*c);auto& body=add_body(snap,"material.metal",horizontal(-6,6),{0,64});body.vx=60;body.angular_velocity=0.5;snap.electric_pulses={body_pulse(body.id,{-6,0},2)};snap.electric_pulses[0].energy=2000;restore(s,snap);ticks(s,1);
    auto state=*s.capture_snapshot();need(state.electric_pulses.size()==1 && state.electric_pulses[0].body==body.id && state.electric_pulses[0].position==CellPos{-4,0} && s.published_world()->electric_energy==1998 && s.published_world()->electric_spent==2,"body-local pulse obeys speed/cost while the real body moves and rotates");
    need(s.world()->bodies()[0].x>1 && s.published_world()->electrified_cells==2 && s.world()->bodies()[0].shape_revision==1,"body afterglow is counted without changing physical shape");
    std::uint64_t spent=2;need(bool(s.pause()),"pause moving electricity");const auto paused=s.capture_snapshot();need(!s.advance_fixed_step() && s.capture_snapshot()==paused,"pause freezes body pose, pulse and glow");need(bool(s.resume()),"resume moving electricity");
    for(int i=0;i<35;++i){ticks(s,1);spent+=s.published_world()->electric_spent;}need(spent==2000 && !s.published_world()->electric_energy && !s.published_world()->electrified_cells && s.published_world()->rigid_pixels==13,"all body pulse energy is spent once and glow expires without material loss");
    // A real body closes the gap between two pieces of fixed grid wire.
    snap=reactive_blank(*c);add_body(snap,"material.metal",horizontal(-1,1),{-1,64});for(const int x:{-4,-3,1,2,3,4})put(snap,{x,64},slot(snap,"material.wire"));auto pulse=body_pulse(0,{-4,64},32);snap.electric_pulses={pulse};restore(s,snap);ticks(s,1);
    need(s.world()->cell_state({4,64})->electrified_ticks && s.published_world()->electrified_cells==9 && s.published_world()->electric_spent==1000,"one finite pulse traverses grid, body and grid using actual contacts");
    // Same connection with a ninety-degree body, then an insulating middle pixel.
    snap=reactive_blank(*c);add_body(snap,"material.metal",horizontal(-6,6),{0,64}).angle=1.5707963267948966;for(const int y:{56,57,71,72,73,74})put(snap,{0,y},slot(snap,"material.wire"));snap.electric_pulses={body_pulse(0,{0,56},32)};restore(s,snap);ticks(s,1);
    need(s.world()->cell_state({0,74})->electrified_ticks && s.published_world()->electrified_cells==19,"rotation preserves the physical conductor's local topology and its two exposed ports");
    snap=reactive_blank(*c);auto& insulated=add_body(snap,"material.metal",horizontal(-6,6),{0,64});for(auto& p:insulated.pixels)if(p.local.x==0){p.material=slot(snap,"material.wood_prop");p.state.fuel=5;}snap.electric_pulses={body_pulse(insulated.id,{-6,0},32)};restore(s,snap);ticks(s,1);
    need(std::none_of(s.world()->bodies()[0].pixels.begin(),s.world()->bodies()[0].pixels.end(),[](const auto& p){return p.local.x>0 && p.state.electrified_ticks;}) && !s.published_world()->electric_energy,"an insulating pixel splits a body's electrical path without splitting its physical shape");
    snap=reactive_blank(*c);auto& alias=add_body(snap,"material.metal",{{0,0},{1,0}},{0,64});alias.x=0.1;alias.y=64.1;alias.angle=0.7853981633974483;snap.electric_pulses={body_pulse(alias.id,{0,0},32)};restore(s,snap);ticks(s,1);
    need(s.published_world()->electrified_cells==2 && s.published_world()->electric_spent==1000,"two actual conductor pixels sharing a raster projection are visited separately once");
    snap=reactive_blank(*c);add_body(snap,"material.wood_prop",{{0,0}},{0,64});restore(s,snap);need(!command(s,{CommandKind::electrify,{0,64},1}).ok && !s.published_world()->electric_energy,"electrical tool explicitly rejects insulating body material");
}
void rigid_electric_changes(){
    auto c=rigid_electric_content();GameSession s(c,make_box2d_solver());auto snap=reactive_blank(*c);auto& body=add_body(snap,"material.metal",horizontal(-6,6),{0,64});const auto parent=body.id;snap.electric_pulses={body_pulse(parent,{5,0})};snap.electric_pulses[0].previous={4,0};restore(s,snap);command(s,{CommandKind::erase,{0,64},1});
    auto current=*s.capture_snapshot();need(current.bodies.size()==2 && current.electric_pulses.size()==1 && current.electric_pulses[0].body!=parent && current.electric_pulses[0].position==CellPos{6,0} && current.electric_pulses[0].previous_body==current.electric_pulses[0].body && s.published_world()->electric_energy==999,"in-flight pulse and its previous address migrate to the correct child fragment");ticks(s,1);need(!s.published_world()->electric_energy && s.published_world()->electric_spent==999,"fracture does not duplicate or lose the remaining electric energy");
    snap=reactive_blank(*c);add_body(snap,"material.metal",horizontal(-2,2),{0,64});snap.electric_pulses={body_pulse(1,{0,0})};snap.bodies[0].pixels[2].state.electrified_ticks=6;restore(s,snap);command(s,{CommandKind::erase,{0,64},1});
    need(!s.published_world()->electric_energy && s.published_world()->electric_spent==1000 && !s.published_world()->electrified_cells,"erasing a charged pulse pixel dissipates its energy once and removes glow counts");
    restore(s,snap);command(s,{CommandKind::blast,{0,64},2,"material.blast_profile"});need(!s.published_world()->rigid_pixels && !s.published_world()->electric_energy && !s.published_world()->electrified_cells && s.published_world()->electric_spent==1000,"explosion destruction cannot leave an orphan body pulse or electric counter");
    snap=reactive_blank(*c);for(int x=-4;x<=4;++x)put(snap,{x,64},slot(snap,"material.wire"));snap.electric_pulses={body_pulse(0,{2,64})};snap.electric_pulses[0].previous={1,64};snap.states[static_cast<std::size_t>(64-32)*96+34].electrified_ticks=6;restore(s,snap);command(s,{CommandKind::detach_body,{0,64},8});
    current=*s.capture_snapshot();need(current.electric_pulses.size()==1 && current.electric_pulses[0].body==current.bodies[0].id && current.electric_pulses[0].position==CellPos{3,0} && s.published_world()->electrified_cells==1 && s.published_world()->electric_energy==999 && s.world()->material_at({2,64})==Material::air,"detaching charged terrain transfers pulse, glow and ownership without a second material copy");
    // A pending pulse follows a conducting phase product back into the grid.
    c=rigid_electric_content(true);GameSession melt(c,make_box2d_solver());snap=reactive_blank(*c);auto& hot=add_body(snap,"material.metal",horizontal(-1,1),{0,64});for(auto& p:hot.pixels)if(p.local.x==0)p.state.temperature=120;snap.electric_pulses={body_pulse(hot.id,{-1,0})};restore(melt,snap);ticks(melt,1);
    current=*melt.capture_snapshot();need(current.electric_pulses.size()==1 && !current.electric_pulses[0].body && current.electric_pulses[0].position==CellPos{0,64} && melt.world()->material_count(melt.world()->material_index("material.molten"))==1 && melt.published_world()->electric_energy==999,"conducting melt product receives the pending pulse's actual new grid address");ticks(melt,1);
    need(melt.capture_snapshot()->electric_pulses.size()==1 && melt.capture_snapshot()->electric_pulses[0].body && melt.published_world()->electric_energy==998,"pulse can enter a surviving conducting fragment from the liquid product");
    c=rigid_electric_content();GameSession ignition(c,make_box2d_solver());snap=reactive_blank(*c);add_body(snap,"material.metal",{{0,0}},{0,64});charge_body(snap,{1,64},{{0,0}});auto p=body_pulse(1,{0,0},32,120);p.heat_interval=1;snap.electric_pulses={p};restore(ignition,snap);ticks(ignition,1);
    need(ignition.world()->bodies()[1].pixels[0].state.detonation_ticks==3 && !ignition.world()->bodies()[1].pixels[0].state.electrified_ticks,"body conductor heats adjacent insulating TNT through the shared contact phase");ticks(ignition,3);need(ignition.world()->material_count(ignition.world()->material_index("material.charge"))==0 && ignition.published_world()->explosions==1,"electrical body heat triggers the real saved fuse/explosion path");
}
void write_historical_body(const std::filesystem::path& file,const WorldSnapshot& s,std::uint32_t version=8){
    std::vector<unsigned char> bytes;auto put=[&](std::uint64_t v,int n){for(int i=0;i<n;++i)bytes.push_back(static_cast<unsigned char>(v>>(8*i)));};
    auto state=[&](const CellState& p){put(p.temperature,4);put(p.life,2);put(p.fuel,4);put(p.burning,1);put(p.created_tick,8);put(p.velocity_x,4);put(p.velocity_y,4);put(p.vertical_fraction,2);put(p.horizontal_fraction,2);put(p.mobilized,1);put(p.quiet_ticks,2);put(p.adhesion_ticks,2);put(p.adhesion_cooldown,2);put(p.detonation_ticks,2);put(p.electrified_ticks,2);if(version>=10){put(p.liquid_fraction_x,4);put(p.liquid_fraction_y,4);}if(version>=11){put(p.momentum_remainder_x,2);put(p.momentum_remainder_y,2);}};
    put(s.seed,8);put(s.tick,8);put(static_cast<std::uint32_t>(s.bounds.origin.x),4);put(static_cast<std::uint32_t>(s.bounds.origin.y),4);put(s.bounds.width,4);put(s.bounds.height,4);put(64,4);put(8,4);put(s.initial_stone,8);put(s.material_ids.size(),4);
    for(const auto& id:s.material_ids){put(id.size(),4);bytes.insert(bytes.end(),id.begin(),id.end());}put(s.cells.size(),4);for(const auto m:s.cells)put(m,2);put(0,4);put(0,8);put(s.material_lab,1);put(version-1,4);for(const auto& p:s.states)state(p);
    put(s.electric_pulses.size(),4);for(const auto& p:s.electric_pulses){put(p.position.x,4);put(p.position.y,4);put(p.previous.x,4);put(p.previous.y,4);put(p.energy,4);put(p.speed,2);put(p.heat_interval,2);put(p.heat,2);put(p.glow_ticks,2);if(version>=9){put(p.body,4);put(p.previous_body,4);}}
    put(s.next_body_id,4);put(s.bodies.size(),4);for(const auto& b:s.bodies){put(b.id,4);put(b.shape_revision,8);for(const auto value:{b.x,b.y,b.angle,b.vx,b.vy,b.angular_velocity})put(std::bit_cast<std::uint64_t>(value),8);put(b.sleeping,1);put(b.pixels.size(),4);for(const auto& p:b.pixels){put(static_cast<std::uint16_t>(p.local.x),2);put(static_cast<std::uint16_t>(p.local.y),2);put(p.material,2);state(p.state);}}
    const auto payload=bytes;std::uint64_t hash=14695981039346656037ULL;for(const auto v:payload){hash^=v;hash*=1099511628211ULL;}bytes={'P','S','B','W','A','L','L',0};put(version,4);put(payload.size(),8);put(hash,8);bytes.insert(bytes.end(),payload.begin(),payload.end());std::ofstream out(file,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));need(bool(out),"write historical body fixture");
}
void rigid_electric_save(){
    const auto c=rigid_electric_content();GameSession s(c,make_box2d_solver());auto snap=reactive_blank(*c);auto& b=add_body(snap,"material.metal",horizontal(-20,20),{0,64});b.vx=60;b.angular_velocity=0.5;snap.electric_pulses={body_pulse(b.id,{-20,0})};restore(s,snap);ticks(s,3);need(bool(s.pause()),"pause live body pulse");const auto saved=*s.capture_snapshot();need(!s.advance_fixed_step() && s.capture_snapshot()==saved,"live pulse/glow/pose stay frozen before file save");
    Temp temp;const auto file=temp.path/"body-electric.psw";need(persistence::save_world_file(file,saved).ok,"save explicit body-local electrical addresses");const auto read=persistence::load_world_file(file);need(read.snapshot && *read.snapshot==saved && read.snapshot->version==save_format_version,"current format roundtrips exact local pulse history, body state and energy");
    auto defs=c->materials();std::reverse(defs.begin(),defs.end());auto registry=ContentRegistry::build(std::move(defs));need(bool(registry),registry.error);GameSession other(registry.registry,make_box2d_solver());need(other.restore_world(*read.snapshot).ok && other.capture_snapshot()==saved,"registry reorder preserves the saved electrical/material owner mapping");
    need(bool(s.resume()) && bool(other.resume()),"resume saved moving pulse");std::uint64_t spent=3,other_spent=3;
    for(int i=0;i<60;++i){ticks(s,1);ticks(other,1);spent+=s.published_world()->electric_spent;other_spent+=other.published_world()->electric_spent;need(s.capture_snapshot()->electric_pulses==other.capture_snapshot()->electric_pulses,"local electrical future is independent of reconstructed physical contact caches");}
    need(spent==1000 && other_spent==1000 && !s.published_world()->electric_energy && !other.published_world()->electrified_cells && s.published_world()->rigid_pixels==41,"restored pulse exhausts exactly the original energy and all glow expires");
    const auto before=s.capture_snapshot();auto bad=saved;bad.electric_pulses[0].body=9999;need(!s.restore_world(bad) && s.capture_snapshot()==before,"missing current electrical owner fails atomically");bad=saved;bad.electric_pulses[0].position={31,31};need(!s.restore_world(bad),"missing local electrical pixel fails");bad=saved;bad.electric_pulses[0].previous_body=9999;need(!s.restore_world(bad),"missing previous electrical owner fails");bad=saved;bad.electric_pulses.push_back(bad.electric_pulses[0]);need(!s.restore_world(bad),"duplicate local heads fail");bad=saved;bad.bodies[0].pixels[0].state.electrified_ticks=61;need(!s.restore_world(bad) && s.capture_snapshot()==before,"invalid body afterglow fails without replacing the current world");
    auto old=reactive_blank(*c);old.tick=12;old.seed=123456;charge_body(old,{0,64},{{0,0},{1,0}},true);old.bodies[0].pixels[0].state.detonation_ticks=2;for(const int x:{10,11})put(old,{x,64},slot(old,"material.wire"));old.electric_pulses={body_pulse(0,{10,64},1)};write_historical_body(file,old);
    auto migrated=persistence::load_world_file(file);need(migrated.snapshot && *migrated.snapshot==old,"real format 8 bytes retain grid pulse, armed rigid fuel, pose, wide fuel and exact shape with zero body address defaults");GameSession legacy(c,make_box2d_solver());need(legacy.restore_world(*migrated.snapshot).ok,"migrated format 8 remains playable");
    old.electric_pulses={body_pulse(old.bodies[0].id,{0,0})};write_historical_body(file,old,9);migrated=persistence::load_world_file(file);
    need(migrated.snapshot && *migrated.snapshot==old,"real format 9 retains body-local pulses and defaults new liquid credits to zero");
    old.bodies[0].pixels[0].state.liquid_fraction_x=123456;write_historical_body(file,old,10);migrated=persistence::load_world_file(file);
    need(migrated.snapshot && *migrated.snapshot==old,"real format 10 retains Q24 volume credit and defaults fine velocity to zero");
    old.bodies[0].pixels[0].state.momentum_remainder_x=123;write_historical_body(file,old,11);migrated=persistence::load_world_file(file);
    need(migrated.snapshot && *migrated.snapshot==old,"real format 11 retains pressure credit/fine momentum and defaults new combustion state to zero");
}
void rigid_pressure(){
    auto definitions=content()->materials();MaterialDefinition water;water.id="material.water";water.display_name="Water";water.sand=false;water.is_static=false;water.density=4;water.gravity_q8=0;water.damping_milli=1000;definitions.push_back(water);
    const auto built=ContentRegistry::build(std::move(definitions));need(bool(built),built.error);const auto c=built.registry;
    for(int mode=0;mode<3;++mode){auto snap=blank();snap.material_ids.push_back(water.id);
        RigidBodyState body;body.id=1;body.x=6.5;body.y=64.5;body.vx=mode==0?-60:0;
        for(int y=-4;y<=4;++y)for(int x=0;x<3;++x){RigidPixel p;p.local={x,y};p.material=3;p.state.fuel=90000;body.pixels.push_back(p);}snap.bodies.push_back(body);snap.next_body_id=2;
        const int bottom=mode==2?64:69;
        for(int y=60;y<bottom;++y)for(int x=-12;x<=5;++x){put(snap,{x,y},5);if(mode)snap.states[static_cast<std::size_t>(y-32)*96+x+32].velocity_x=256;}
        GameSession session(c,make_box2d_solver());restore(session,snap);const auto quantity=session.world()->material_count(session.world()->material_index(water.id));ticks(session,1);
        const auto& actual=session.world()->bodies().front();need(session.published_world()->liquid_body_impulses>0,"actual moving boundary participates in pressure solve");
        double momentum=54*actual.vx;for(const auto& chunk:session.world()->chunks())for(std::size_t i=0;i<chunk.cells.size();++i)if(static_cast<std::uint16_t>(chunk.cells[i])==5)momentum+=4*(chunk.states[i].velocity_x+chunk.states[i].momentum_remainder_x/256.0)/256*60;
        const double incoming=mode==0?-3240:static_cast<double>(quantity)*4*60;
        bool fluid_moved=false;for(const auto& chunk:session.world()->chunks())for(std::size_t i=0;i<chunk.cells.size();++i)if(static_cast<std::uint16_t>(chunk.cells[i])==5)fluid_moved=fluid_moved || (mode==0?chunk.states[i].velocity_x<0:chunk.states[i].velocity_x<256);
        std::cout<<"pressure body mode="<<mode<<" vx="<<actual.vx<<" spin="<<actual.angular_velocity<<" residual="<<session.published_world()->liquid_divergence_after<<" momentum="<<incoming<<" -> "<<momentum<<'\n';
        need(std::abs(momentum-incoming)<1,"body and free liquid conserve horizontal momentum within Q16 quantization");
        need(fluid_moved,"pressure changes the other participant's actual saved liquid momentum");
        need(mode==0?actual.vx>-60:actual.vx>0,"pressure exchanges momentum in both body-to-fluid and fluid-to-body directions");
        if(mode==2)need(actual.angular_velocity>0,"off-center liquid pressure creates the expected torque");
        need(session.world()->material_count(session.world()->material_index(water.id))==quantity,"pressure contact conserves liquid and body ownership");
        need(session.published_world()->liquid_divergence_after<1e-5,"coupled pressure satisfies the moving boundary divergence constraint");
    }
}
void rigid_fluid(){
    auto defs=content()->materials();
    MaterialDefinition water;water.id="material.water";water.display_name="Water";water.cell_type="liquid";water.sand=false;water.is_static=false;water.density=4;defs.push_back(water);
    auto heavy=defs[2];heavy.id="material.heavy_prop";heavy.source_id="heavy_prop";heavy.density=8;defs.push_back(heavy);
    const auto built=ContentRegistry::build(defs);need(bool(built),built.error);auto c=built.registry;
    for(const bool dense:{false,true}){
        GameSession session(c,make_box2d_solver());auto snap=blank();snap.material_ids.push_back(water.id);snap.material_ids.push_back(heavy.id);floor(snap,110);
        for(int y=32;y<110;++y){put(snap,{-32,y},1);put(snap,{63,y},1);}
        for(int y=70;y<110;++y)for(int x=-31;x<63;++x)put(snap,{x,y},5);
        restore(session,snap);const auto quantity=session.world()->material_count(session.world()->material_index(water.id));
        const auto result=command(session,{CommandKind::spawn_body,{0,63},3,dense?heavy.id:"material.wood_prop"});need(result.ok,result.message);
        auto rotating=*session.capture_snapshot();rotating.bodies.back().angle=0.6;rotating.bodies.back().angular_velocity=1;restore(session,rotating);
        std::uint64_t displaced=0;
        for(int i=0;i<360;++i){
            ticks(session,1);displaced+=session.published_world()->fluid_displaced;
            need(session.world()->material_count(session.world()->material_index(water.id))==quantity,"immersing a body never creates or destroys liquid");
            const auto& b=session.world()->bodies().front();
            for(int y=static_cast<int>(std::floor(b.y))-5;y<=static_cast<int>(std::ceil(b.y))+5;++y)
                for(int x=static_cast<int>(std::floor(b.x))-5;x<=static_cast<int>(std::ceil(b.x))+5;++x)
                    if(session.world()->body_pixel_at({x,y}))need(session.world()->material_at({x,y})!=session.world()->material_index(water.id),"liquid occupies no raster cell covered by the actual body");
        }
        const auto& body=session.world()->bodies().front();
        std::cout<<"fluid density="<<(dense?8:2)<<" y="<<body.y<<" vy="<<body.vy<<" displaced="<<displaced<<'\n';
        need(displaced>0,"real body motion displaces real liquid cells");
        need(dense?body.y>100:(body.y>65 && body.y<78),dense?"denser solid sinks through the pool":"lighter solid floats near the liquid surface");
        need(session.published_world()->rigid_pixels==29,"buoyancy never duplicates body material");
        need(bool(session.pause()),"pause immersed body");const auto saved=*session.capture_snapshot();need(!session.advance_fixed_step() && session.capture_snapshot()==saved,"paused fluid coupling does not run");
        Temp temp;const auto file=temp.path/"immersed.psw";need(persistence::save_world_file(file,saved).ok,"save immersed body and displaced water");const auto read=persistence::load_world_file(file);need(read.snapshot && *read.snapshot==saved,"real file preserves the entire displaced material shape and pose");
        GameSession other(c,make_box2d_solver());need(other.restore_world(*read.snapshot).ok && other.capture_snapshot()==saved,"restore exact fluid/body snapshot before resuming");
        need(bool(session.resume()) && bool(other.resume()),"resume immersed worlds");ticks(session,10);ticks(other,10);
        need(std::abs(session.world()->bodies()[0].y-other.world()->bodies()[0].y)<0.5 && other.world()->material_count(other.world()->material_index(water.id))==quantity,"reconstructed solver keeps immersion and material quantity");
    }
    // A legacy overlapping snapshot in a completely full finite domain has no
    // legal displacement destination. Its material and old pose must survive.
    GameSession full(c,make_box2d_solver());auto snap=blank();snap.material_ids.push_back(water.id);snap.material_ids.push_back(heavy.id);
    for(auto& cell:snap.cells)cell=5;
    RigidBodyState body;body.id=1;body.x=0.5;body.y=64.5;body.vy=20;RigidPixel pixel;pixel.material=6;pixel.state.fuel=90000;body.pixels.push_back(pixel);snap.bodies.push_back(body);snap.next_body_id=2;
    restore(full,snap);ticks(full,1);
    need(full.published_world()->fluid_blocked==1 && full.published_world()->fluid_displaced==0,"no capacity rejects the complete displacement batch");
    need(full.world()->bodies()[0].x==body.x && full.world()->bodies()[0].y==body.y && full.world()->bodies()[0].vy==0,"blocked liquid restores the old pose and arrests motion");
    need(full.capture_snapshot()->cells==snap.cells && full.capture_snapshot()->states==snap.states,"failed displacement preserves all original material and per-cell states");
}

void rigid_fluid_layers(bool source=false){
    auto defs=content()->materials();MaterialDefinition water;water.id="material.water";water.display_name="Water";water.cell_type="liquid";water.is_static=false;water.sand=false;water.density=4;water.source_liquid=source;defs.push_back(water);
    auto oil=water;oil.id="material.oil";oil.display_name="Oil";oil.density=1;oil.fuel=100;oil.flame_output=oil.smoke_output=oil.burn_output="air";oil.electrical_conductivity=true;defs.push_back(oil);
    const auto built=ContentRegistry::build(defs);need(bool(built),built.error);const auto c=built.registry;GameSession session(c,make_box2d_solver());
    auto snap=reactive_blank(*c);snap.tick=10;
    for(int y=32;y<128;++y)for(int x=-32;x<64;++x)put(snap,{x,y},1);
    put(snap,{0,63},0);put(snap,{0,64},slot(snap,oil.id));put(snap,{0,65},slot(snap,oil.id));put(snap,{0,66},slot(snap,water.id));
    const auto index=[&](CellPos p){return static_cast<std::size_t>(p.y-32)*96+p.x+32;};
    snap.states[index({0,64})].fuel=17;snap.states[index({0,64})].created_tick=1;
    snap.states[index({0,65})].fuel=18;snap.states[index({0,65})].created_tick=2;
    snap.states[index({0,66})].created_tick=3;
    if(source){
        snap.states[index({0,64})].liquid_direction=3;snap.states[index({0,65})].liquid_direction=3;
        snap.states[index({0,66})].liquid_fall=0.375f;snap.states[index({0,66})].liquid_lateral=0.9f;snap.states[index({0,66})].liquid_direction=1;
    }
    add_body(snap,"material.wood_prop",{{0,0}},{0,66});
    ElectricPulse pulse;pulse.position=pulse.previous={0,64};pulse.speed=1;pulse.heat=0;snap.electric_pulses={pulse};
    restore(session,snap);ticks(session,1);const auto after=*session.capture_snapshot();
    need(after.cells[index({0,63})]==slot(snap,oil.id) && after.cells[index({0,64})]==slot(snap,oil.id) && after.cells[index({0,65})]==slot(snap,water.id) && after.cells[index({0,66})]==0,"piston pushes each liquid layer one cell instead of teleporting bottom water above oil");
    if(source)need(after.states[index({0,65})].liquid_fall==0.375f && after.states[index({0,65})].liquid_lateral==0.9f && !session.published_world()->liquid_pressure_iterations,"rigid displacement transfers source accumulators without pressure projection");
    need(after.states[index({0,63})].fuel==17 && after.states[index({0,64})].fuel==18 && after.states[index({0,63})].created_tick==1 && after.states[index({0,64})].created_tick==2 && after.states[index({0,65})].created_tick==3,"each actual liquid parcel keeps its own fuel and creation identity");
    for(const auto& p:after.electric_pulses)std::cout<<"layer pulse "<<p.position.x<<","<<p.position.y<<" previous "<<p.previous.x<<","<<p.previous.y<<" energy="<<p.energy<<"\n";
    need(after.electric_pulses.size()==1 && after.electric_pulses[0].position==CellPos{0,64} && after.electric_pulses[0].previous==CellPos{0,63} && after.electric_pulses[0].energy==1000u-static_cast<std::uint32_t>(oil.electric_cost) && session.published_world()->electric_spent==static_cast<std::uint64_t>(oil.electric_cost),"current and previous pulse addresses follow the simultaneous parcel permutation once");
    need(after.states[index({0,63})].electrified_ticks>0 && session.published_world()->electrified_cells==1 && session.published_world()->fluid_displaced==1,"charged liquid glow and displaced-volume counters remain exact");
    need(session.world()->material_count(session.world()->material_index(water.id))==1 && session.world()->material_count(session.world()->material_index(oil.id))==2,"layered displacement conserves each material separately");
    Temp temp;const auto file=temp.path/"layers.psw";need(persistence::save_world_file(file,after).ok,"save layered displacement");const auto loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==after,"actual layered shape, parcel state and pulse path survive format 9");
    GameSession other(c,make_box2d_solver());restore(other,*loaded.snapshot);ticks(session,5);ticks(other,5);
    need(session.capture_snapshot()->cells==other.capture_snapshot()->cells && session.capture_snapshot()->states==other.capture_snapshot()->states && session.capture_snapshot()->electric_pulses==other.capture_snapshot()->electric_pulses,"restored layer/pulse future follows the same material progression");
    // Shared virtual routes must observe earlier shifts and preserve each
    // parcel's original address even when it moves more than once in a batch.
    put(snap,{0,62},0);put(snap,{0,67},slot(snap,water.id));snap.states[index({0,67})].created_tick=4;
    auto extra=snap.bodies[0].pixels[0];extra.local={0,1};snap.bodies[0].pixels.push_back(extra);restore(session,snap);ticks(session,1);
    const auto shared=*session.capture_snapshot();
    need(shared.cells[index({0,62})]==slot(snap,oil.id) && shared.cells[index({0,63})]==slot(snap,oil.id) && shared.cells[index({0,64})]==slot(snap,water.id) && shared.cells[index({0,65})]==slot(snap,water.id) && shared.cells[index({0,66})]==0 && shared.cells[index({0,67})]==0,"two sources share a virtual route without reusing the same terminal air cell");
    for(int i=0;i<4;++i)need(shared.states[index({0,62+i})].created_tick==static_cast<std::uint64_t>(i+1),"shared routes preserve all four unique parcel identities");
    need(shared.electric_pulses.size()==1 && shared.electric_pulses[0].position==CellPos{0,63} && shared.electric_pulses[0].previous==CellPos{0,62} && shared.electric_pulses[0].energy==996 && session.published_world()->fluid_displaced==2,"batch remapping follows the final parcel destination rather than applying each route twice");
    // Only one air cell remains: the first successful virtual route must not
    // leak into the actual world when planning the second route fails.
    snap.electric_pulses.clear();put(snap,{0,62},1);restore(session,snap);ticks(session,1);
    need(session.published_world()->fluid_blocked==1 && session.published_world()->fluid_displaced==0 && session.capture_snapshot()->cells==snap.cells && session.capture_snapshot()->states==snap.states,"late capacity failure rolls back all previously planned layer shifts and parcel states");

}

void rigid_friction(){
    auto registry=[](float ground,float body){
        auto defs=content()->materials();defs[2].solid_friction=body;
        auto floor_material=defs[0];floor_material.id="material.contact_floor";floor_material.solid_friction=ground;defs.push_back(floor_material);
        const auto built=ContentRegistry::build(defs);need(bool(built),built.error);return built.registry;
    };
    auto scene=[](){
        auto snap=blank();snap.material_ids.push_back("material.contact_floor");
        for(int y=110;y<112;++y)for(int x=-32;x<64;++x)put(snap,{x,y},5);
        RigidBodyState body;body.id=1;body.x=-9.5;body.y=108.5;body.vx=64;
        for(int y=0;y<2;++y)for(int x=-2;x<=2;++x){RigidPixel p;p.local={x,y};p.material=3;p.state.fuel=90000;body.pixels.push_back(p);}
        snap.bodies.push_back(body);snap.next_body_id=2;return snap;
    };
    auto slide=[&](float floor_friction,float body_friction){
        GameSession session(registry(floor_friction,body_friction),make_box2d_solver());restore(session,scene());ticks(session,60);
        need(session.published_world()->rigid_pixels==10,"friction changes contact response, never material quantity");
        return std::pair{session.world()->bodies()[0].x,session.world()->bodies()[0].vx};
    };
    const auto ice=slide(0.05f,1),rough=slide(0.9f,1),slick_body=slide(1,0.05f),rough_body=slide(1,0.9f);
    std::cout<<"ground friction low/rough: x="<<ice.first<<"/"<<rough.first<<" vx="<<ice.second<<"/"<<rough.second<<"; body friction low/rough: x="<<slick_body.first<<"/"<<rough_body.first<<'\n';
    need(ice.first>rough.first+20 && ice.second>15 && std::abs(rough.second)<2,"material-specific ice-like ground allows longer sliding than rough ground");
    need(slick_body.first>rough_body.first+20,"the body's own material friction also participates in real contact mixing");
    const auto frictionless=slide(0,1);need(frictionless.first>50 && std::abs(frictionless.second-64)<0.5,"zero friction preserves horizontal speed across a chunk seam");
    GameSession barrier(registry(0.05f,1),make_box2d_solver());auto blocked=scene();
    for(int y=80;y<110;++y)for(int x=0;x<2;++x)put(blocked,{x,y},1);restore(barrier,blocked);ticks(barrier,60);
    need(barrier.world()->bodies()[0].x<-1 && barrier.published_world()->rigid_pixels==10,"internal seam suppression keeps an exposed solid wall collidable");
    auto changing_defs=registry(0.9f,1)->materials();changing_defs[2].fuel=0;
    auto slick=changing_defs.back();slick.id="material.slick_floor";slick.solid_friction=0.05f;changing_defs.push_back(slick);
    changing_defs[4].warmth_threshold=50;changing_defs[4].warmth_output=slick.id;
    const auto changed_content=ContentRegistry::build(changing_defs);need(bool(changed_content),changed_content.error);
    auto changed_scene=scene();changed_scene.material_ids.push_back(slick.id);for(auto& p:changed_scene.bodies[0].pixels)p.state.fuel=0;
    GameSession heated(changed_content.registry,make_box2d_solver()),cold(changed_content.registry,make_box2d_solver());restore(heated,changed_scene);restore(cold,changed_scene);ticks(heated,1);ticks(cold,1);
    need(command(heated,{CommandKind::heat,{0,110},8}).ok,"heat changes the actual contact material");ticks(cold,1);ticks(heated,43);ticks(cold,43);
    std::cout<<"same-geometry material change: heated/cold x="<<heated.world()->bodies()[0].x<<"/"<<cold.world()->bodies()[0].x<<'\n';
    need(heated.world()->material_count(heated.world()->material_index(slick.id))>0 && heated.world()->bodies()[0].x>cold.world()->bodies()[0].x+4,"same-geometry material replacement refreshes cached terrain contact friction");
    GameSession original(registry(0.05f,1),make_box2d_solver());restore(original,scene());ticks(original,30);need(bool(original.pause()),"pause sliding body");const auto saved=*original.capture_snapshot();
    Temp temp;const auto file=temp.path/"friction.psw";need(persistence::save_world_file(file,saved).ok,"save actual sliding body");const auto read=persistence::load_world_file(file);need(read.snapshot && *read.snapshot==saved,"friction needs no hidden state outside the saved pose and material IDs");
    auto defs=original.content().materials();std::reverse(defs.begin(),defs.end());const auto built=ContentRegistry::build(defs);need(bool(built),built.error);GameSession other(built.registry,make_box2d_solver());need(other.restore_world(*read.snapshot).ok && other.capture_snapshot()==saved,"registry reordering preserves contact material meaning");
    need(bool(original.resume()) && bool(other.resume()),"resume sliding worlds");ticks(original,15);ticks(other,15);
    need(std::abs(original.world()->bodies()[0].x-other.world()->bodies()[0].x)<0.5 && std::abs(original.world()->bodies()[0].vx-other.world()->bodies()[0].vx)<1,"reconstructed contact uses the saved material's friction");
    for(const float invalid:{-0.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),10001.0f}){
        auto invalid_defs=content()->materials();invalid_defs[0].solid_friction=invalid;const auto bad=ContentRegistry::build(invalid_defs);
        need(!bad && bad.error.find("solid_friction")!=std::string::npos && bad.error.find("material.stone")!=std::string::npos,"invalid friction reports the exact field and material");
    }
}

}
int main(int argc,char** argv){try{const std::string name=argc==2?argv[1]:"";if(name=="rigid_motion")rigid_motion();else if(name=="rigid_geometry")rigid_geometry();else if(name=="rigid_persistence")rigid_persistence();else if(name=="rigid_thermal")rigid_thermal();else if(name=="rigid_reactions")rigid_reactions();else if(name=="rigid_thermal_persistence")rigid_thermal_persistence();else if(name=="rigid_blast")rigid_blast();else if(name=="rigid_blast_occlusion")rigid_blast_occlusion();else if(name=="rigid_fuses")rigid_fuses();else if(name=="rigid_electricity")rigid_electricity();else if(name=="rigid_electric_changes")rigid_electric_changes();else if(name=="rigid_electric_save")rigid_electric_save();else if(name=="rigid_pressure")rigid_pressure();else if(name=="rigid_fluid")rigid_fluid();else if(name=="rigid_fluid_layers")rigid_fluid_layers();else if(name=="source_liquid_rigid")rigid_fluid_layers(true);else if(name=="rigid_friction")rigid_friction();else throw std::runtime_error("Unknown rigid case.");std::cout<<"PASS "<<name<<'\n';return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
