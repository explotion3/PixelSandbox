#include "session/game_session.hpp"
#include "simulation/material_simulation.hpp"
#include "persistence/world_file.hpp"
#include "content/reaction_table.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <numeric>

using namespace pixel;
namespace {
void need(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
std::shared_ptr<const ContentRegistry> catalog(bool reactions=true){
    std::vector<MaterialDefinition> v;
    auto add=[&](const char* id,const char* type,bool sand,bool fixed,float density,std::vector<std::string> tags){
        MaterialDefinition m;m.id=std::string("material.")+id;m.display_name=id;m.source_id=id;m.cell_type=type;m.sand=sand;m.is_static=fixed;m.density=density;m.tags=std::move(tags);v.push_back(m);
    };
    add("stone","liquid",true,true,10,{"corrodible"});
    add("sand","liquid",true,false,6,{});add("sand_static","liquid",true,true,10,{});
    add("water","liquid",false,false,4,{"water"});v.back().cooling=40;v.back().warmth_threshold=60;v.back().warmth_output="material.steam";
    add("water_static","liquid",false,true,4,{"water"});v.back().cooling=40;
    add("oil","liquid",false,false,1,{"burnable"});v.back().fuel=30;v.back().ignition=10;
    add("wood","liquid",true,true,11,{"burnable","corrodible"});v.back().fuel=30;
    add("steam","gas",false,false,1,{"gas"});v.back().lifetime=1000;
    add("smoke","gas",false,false,1,{"gas"});v.back().lifetime=350;
    add("fire","fire",false,false,1,{"fire","hot"});v.back().lifetime=90;v.back().heat=100;
    add("ice","liquid",true,true,10,{});v.back().warmth_threshold=30;v.back().warmth_output="material.water";
    add("lava","liquid",false,false,6,{"lava"});v.back().heat=90;v.back().flow_interval=4;
    add("acid","liquid",false,false,2.9f,{"acid"});add("acid_gas","gas",false,false,2,{"gas"});v.back().lifetime=450;
    add("salt","liquid",true,false,6,{});
    add("salt_water","liquid",false,false,4.01f,{"water"});
    add("sludge","liquid",false,false,3,{});
    add("cement","liquid",false,false,6,{});
    add("concrete","liquid",true,true,10,{});
    add("slime","liquid",false,false,5,{"slime"});v.back().slime=true;v.back().adhesion_strength_q8=1024;v.back().adhesion_span=4;
    v.back().cold_threshold=-10;v.back().cold_output="material.ice_slime";
    add("ice_slime","liquid",true,true,10,{"meltable_to_slime"});
    add("cloud_slime","liquid",false,false,1,{"slime"});v.back().lifetime=10;v.back().gravity_q8=0;
    add("glue","liquid",true,false,6,{});v.back().adhesion_strength_q8=1024;v.back().gravity_q8=128;
    for(auto& d:v)if(d.is_static && d.sand)d.can_loosen=true;
    v[3].cold_output="material.ice";v[3].cold_threshold=-10;
    std::vector<ReactionDefinition> r;
    if(reactions)r={{"[fire]","material.water","keep","material.steam",1},{"[lava]","[water]","material.stone","material.steam",1},{"[acid]","[corrodible]","keep","material.acid_gas",1}};
    if(reactions){
        r.push_back({"material.salt","material.water","air","material.salt_water",1});
        r.push_back({"material.water","material.sludge","keep","material.water",1});
        r.push_back({"material.cement","air","material.concrete","keep",1,4});
        r.push_back({"[fire]","[meltable_to_slime]","keep","material.slime",1});
        r.push_back({"material.water","[slime]","keep","material.cloud_slime",1});
    }
    auto result=ContentRegistry::build(std::move(v),std::move(r));need(bool(result),result.error);return result.registry;
}
WorldSnapshot blank(const ContentRegistry& content,WorldBounds bounds={{-32,32},96,64}){
    auto s=MaterialSimulation::make_lab(12345,content).snapshot();s.bounds=bounds;
    s.cells.assign(static_cast<std::size_t>(bounds.width)*bounds.height,0);s.states.assign(s.cells.size(),{});
    s.initial_stone=0;s.drops.clear();s.held_stone=0;return s;
}
void put(WorldSnapshot& s,const ContentRegistry& content,CellPos p,const std::string& id){
    const auto i=static_cast<std::size_t>(p.y-s.bounds.origin.y)*s.bounds.width+static_cast<std::size_t>(p.x-s.bounds.origin.x);
    need(s.bounds.contains(p),"fixture position");
    const auto found=std::find(s.material_ids.begin(),s.material_ids.end(),id);need(found!=s.material_ids.end(),"fixture material");
    if(s.cells[i]==1)--s.initial_stone;
    s.cells[i]=static_cast<std::uint16_t>(1+found-s.material_ids.begin());
    if(s.cells[i]==1)++s.initial_stone;
    s.states[i]=MaterialSimulation::initial_state(*content.find_material(id));
}
void rect(WorldSnapshot& s,const ContentRegistry& c,int x,int y,int width,int height,const std::string& id){for(int yy=y;yy<y+height;++yy)for(int xx=x;xx<x+width;++xx)put(s,c,{xx,yy},id);}
void restore(GameSession& s,const WorldSnapshot& snap){const auto r=s.restore_world(snap);need(r.ok,r.error);need(bool(s.resume()),"resume fixture");}
void ticks(GameSession& s,int count){for(int i=0;i<count;++i)need(s.advance_fixed_step(),"fixed step");}
std::uint64_t count(const GameSession& s,const char* id){return s.world()->material_count(s.world()->material_index(id));}
CommandResult command(GameSession& s,WorldCommand c){need(bool(s.submit(std::move(c))),"submit tool");ticks(s,1);return s.command_results().back();}
void movement(){
    auto c=catalog(false);GameSession s(c);auto snap=blank(*c);
    put(snap,*c,{-1,63},"material.sand");restore(s,snap);ticks(s,1);
    need(s.world()->material_at({-1,64})==s.world()->material_index("material.sand"),"powder crosses y=64 exactly one cell per tick");
    const auto changes=s.take_changes();need(changes.full_refresh,"initial full refresh");
    ticks(s,1);need(s.take_changes().dirty_chunks.size()>0,"movement publishes dirty chunks");
    rect(snap,*c,-32,94,96,2,"material.stone");rect(snap,*c,-9,35,18,8,"material.sand");restore(s,snap);
    const auto initial=count(s,"material.sand");ticks(s,180);
    need(count(s,"material.sand")==initial,"powder conserved across chunks and closed bounds");
    for(int y=32;y<94;++y)for(int x=-32;x<64;++x)if(s.world()->material_at({x,y})==s.world()->material_index("material.sand")){
        need(s.world()->material_at({x,y+1})!=Material::air,"settled powder has support");
    }
    // A displaced gas cannot move a second time in the upward pass.
    snap=blank(*c);put(snap,*c,{0,63},"material.sand");put(snap,*c,{0,64},"material.smoke");restore(s,snap);ticks(s,1);
    need(s.world()->material_at({0,63})==s.world()->material_index("material.smoke"),"exchange updates both participants once");
    need(s.world()->material_at({0,64})==s.world()->material_index("material.sand"),"sand sinks through gas");
    // Static fixtures sleep, then removing cross-chunk support wakes the sand.
    snap=blank(*c);rect(snap,*c,-32,64,96,1,"material.stone");rect(snap,*c,-3,63,7,1,"material.sand");restore(s,snap);ticks(s,3);
    need(s.published_world()->moved_cells==0,"supported powder rests");
    command(s,{CommandKind::erase,{0,64},2});ticks(s,2);
    need(s.world()->material_at({0,63})!=s.world()->material_index("material.sand"),"support removal wakes cross-chunk powder");
}
void liquid(){
    auto c=catalog(false);GameSession s(c);auto snap=blank(*c,{{-16,48},32,32});
    rect(snap,*c,-16,78,32,2,"material.stone");rect(snap,*c,-16,48,1,30,"material.stone");rect(snap,*c,15,48,1,30,"material.stone");
    rect(snap,*c,-15,56,10,20,"material.water");restore(s,snap);ticks(s,2000);
    need(count(s,"material.water")==200,"water volume conserved");
    int lowest=1000,highest=-1000;
    for(int x=-15;x<15;++x){int surface=78;for(int y=48;y<78;++y)if(s.world()->material_at({x,y})==s.world()->material_index("material.water")){surface=y;break;}lowest=std::min(lowest,surface);highest=std::max(highest,surface);}
    if(highest-lowest>1){for(int y=48;y<80;++y){for(int x=-16;x<16;++x){auto m=*s.world()->material_at({x,y});std::cout<<(m==Material::air?'.':m==Material::stone?'#':'~');}std::cout<<'\n';}}
    need(highest-lowest<=1,"connected pool surface levels to within one cell; range="+std::to_string(highest-lowest));
    const auto stable=s.capture_snapshot();static_cast<void>(s.take_changes());
    for(int i=0;i<300;++i){ticks(s,1);need(s.published_world()->moved_cells==0,"settled partial top row must stop moving");}
    auto after=*s.capture_snapshot();after.tick=stable->tick;
    need(after==*stable && s.take_changes().dirty_chunks.empty(),"settled liquid has no state churn or texture uploads");
    snap=blank(*c,{{-4,48},8,24});rect(snap,*c,-4,70,8,2,"material.stone");rect(snap,*c,-4,48,1,22,"material.stone");rect(snap,*c,3,48,1,22,"material.stone");
    rect(snap,*c,-3,66,6,4,"material.oil");rect(snap,*c,-3,62,6,4,"material.water");restore(s,snap);ticks(s,250);
    need(count(s,"material.oil")==24 && count(s,"material.water")==24,"density exchanges conserve both liquids");
    for(int x=-3;x<3;++x)need(s.world()->material_at({x,69})==s.world()->material_index("material.water"),"denser water settles below oil");
    snap=blank(*c);put(snap,*c,{-1,63},"material.water_static");restore(s,snap);ticks(s,50);
    need(s.world()->material_at({-1,63})==s.world()->material_index("material.water_static"),"static liquid does not flow");
    need(s.published_world()->active_chunks==0,"cold static world sleeps");
}
void gas_fire(){
    auto c=catalog(false);GameSession s(c);auto snap=blank(*c);
    put(snap,*c,{0,64},"material.smoke");restore(s,snap);ticks(s,1);
    need(s.world()->material_at({0,63})==s.world()->material_index("material.smoke"),"gas rises across boundary");
    rect(snap,*c,-32,32,96,2,"material.stone");restore(s,snap);ticks(s,80);
    need(count(s,"material.smoke")==1,"gas collects below closed ceiling before expiry");
    for(int y=35;y<96;++y)for(int x=-32;x<64;++x)
        need(s.world()->material_at({x,y})!=s.world()->material_index("material.smoke"),"gas reaches the ceiling pocket");
    snap=blank(*c);put(snap,*c,{0,64},"material.fire");restore(s,snap);ticks(s,89);
    need(count(s,"material.fire")==1,"fire finite life survives 89 ticks even against ceiling");ticks(s,1);
    need(count(s,"material.fire")==0,"fire expires at 90 ticks");
    auto definitions=c->materials();auto fire=std::find_if(definitions.begin(),definitions.end(),[](const auto& d){return d.cell_type=="fire";});
    fire->tags.clear();need(!ContentRegistry::build(definitions),"fire tag required");fire->tags={"fire"};fire->collectible=true;need(!ContentRegistry::build(definitions),"fire cannot be collected");
}
void reactions(){
    auto c=catalog();GameSession s(c);auto snap=blank(*c,{{-8,56},16,16});
    // Trap adjacent reagents so the fixture tests reaction, not escape before contact.
    rect(snap,*c,-8,56,16,16,"material.stone");put(snap,*c,{-1,63},"material.lava");put(snap,*c,{0,63},"material.water");
    restore(s,snap);ticks(s,1);need(count(s,"material.lava")==0 && count(s,"material.steam")==1,"lava + water becomes rock + steam across chunk boundary");
    const auto view=*s.published_world();need(view.world_stone+view.dropped_stone+view.held_stone==view.initial_stone,"reaction stone source explicitly accounted");
    snap=blank(*c);put(snap,*c,{0,64},"material.wood");restore(s,snap);command(s,{CommandKind::heat,{0,64},1});
    need(s.world()->cell_state({0,64})->burning,"heat ignites wood");ticks(s,35);
    need(count(s,"material.wood")==0 && count(s,"material.smoke")>0,"burning consumes finite fuel into smoke");
    snap=blank(*c);put(snap,*c,{-1,63},"material.wood");put(snap,*c,{0,63},"material.water_static");
    const auto wood_index=static_cast<std::size_t>(63-snap.bounds.origin.y)*snap.bounds.width+static_cast<std::size_t>(-1-snap.bounds.origin.x);
    snap.states[wood_index].temperature=100;snap.states[wood_index].burning=true;
    restore(s,snap);ticks(s,1);
    need(!s.world()->cell_state({-1,63})->burning && s.world()->cell_state({-1,63})->fuel==30,"adjacent water cools and suppresses burning without consuming fuel");
    snap=blank(*c);put(snap,*c,{0,64},"material.ice");restore(s,snap);command(s,{CommandKind::heat,{0,64},1});
    need(count(s,"material.ice")==0 && count(s,"material.water")==1,"heated ice melts via material-configured threshold");
    snap=blank(*c,{{-8,56},16,16});rect(snap,*c,-8,56,16,16,"material.stone");put(snap,*c,{0,63},"material.acid");restore(s,snap);ticks(s,1);
    need(count(s,"material.acid")==1 && count(s,"material.acid_gas")>=1,"acid retains reagent while corrodible target becomes acid gas");
    snap=blank(*c,{{-8,56},16,16});rect(snap,*c,-8,56,16,16,"material.stone");put(snap,*c,{-1,63},"material.fire");put(snap,*c,{0,63},"material.water");restore(s,snap);ticks(s,1);
    need(count(s,"material.steam")==1,"fire tag + water creates steam");
    auto invalid=c->reactions();invalid[0].output_b="material.absent";need(!ContentRegistry::build(c->materials(),invalid),"reaction output references validated");
}
struct Temp {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("PixelSandbox-material-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){std::filesystem::create_directories(path);}
    ~Temp(){std::error_code e;auto parent=std::filesystem::weakly_canonical(path.parent_path(),e);if(!e && parent==std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),e) && path.filename().string().starts_with("PixelSandbox-material-tests-"))std::filesystem::remove_all(path,e);}
};
void persistence_test(){
    auto c=catalog();GameSession s(c);need(bool(s.create_material_lab(987654321)),"lab create");ticks(s,16);
    command(s,{CommandKind::heat,{65,68},4});ticks(s,8);
    Temp temp;const auto file=temp.path/"materials.psw";auto saved=*s.capture_snapshot();
    const auto result=persistence::save_world_file(file,saved);need(result.ok,result.error);
    const auto loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==saved,"all material/state binary fields roundtrip");
    auto reordered=c->materials();std::reverse(reordered.begin(),reordered.end());
    const auto reordered_content=ContentRegistry::build(std::move(reordered),c->reactions());need(bool(reordered_content),reordered_content.error);
    GameSession other(reordered_content.registry);restore(other,*loaded.snapshot);
    for(int i=0;i<100;++i){ticks(s,1);ticks(other,1);need(s.capture_snapshot()==other.capture_snapshot(),"save/load future simulation deterministic at step "+std::to_string(i));}
    auto bad=*s.capture_snapshot();bad.material_ids[2]="material.missing";const auto before=s.capture_snapshot();
    need(!s.restore_world(bad) && s.capture_snapshot()==before,"missing material rejected atomically");
    bad=*before;bad.states[0].life=65535;need(!s.restore_world(bad) && s.capture_snapshot()==before,"invalid cell state rejected atomically");
    bad=*before;bad.states[0].created_tick=bad.tick+1;need(!s.restore_world(bad) && s.capture_snapshot()==before,"future creation tick rejected atomically");
    bad=*before;bad.states[0].velocity_x=9000;need(!s.restore_world(bad) && s.capture_snapshot()==before,"invalid velocity rejected atomically");
    std::ofstream broken(file,std::ios::binary|std::ios::trunc);broken<<"bad";broken.close();need(!persistence::load_world_file(file).snapshot && s.capture_snapshot()==before,"corrupt file does not affect live world");
    // Construct a genuine v1 byte stream, using its explicit historical field order.
    auto legacy=make_stone_wall_scenario(77).snapshot();std::vector<unsigned char> body;
    auto put_bytes=[&](std::uint64_t value,int bytes){for(int i=0;i<bytes;++i)body.push_back(static_cast<unsigned char>(value>>(i*8)));};
    put_bytes(legacy.seed,8);put_bytes(0,8);put_bytes(static_cast<std::uint32_t>(legacy.bounds.origin.x),4);put_bytes(0,4);
    put_bytes(192,4);put_bytes(96,4);put_bytes(64,4);put_bytes(8,4);put_bytes(7168,8);put_bytes(1,4);put_bytes(14,4);
    const std::string id="material.stone";body.insert(body.end(),id.begin(),id.end());put_bytes(legacy.cells.size(),4);for(auto cell:legacy.cells)put_bytes(cell,2);put_bytes(0,4);put_bytes(0,8);
    std::uint64_t hash=14695981039346656037ULL;for(auto byte:body){hash^=byte;hash*=1099511628211ULL;}
    const auto payload=body;body={'P','S','B','W','A','L','L',0};put_bytes(1,4);put_bytes(payload.size(),8);put_bytes(hash,8);body.insert(body.end(),payload.begin(),payload.end());
    std::ofstream old(file,std::ios::binary|std::ios::trunc);old.write(reinterpret_cast<const char*>(body.data()),static_cast<std::streamsize>(body.size()));old.close();
    const auto migrated=persistence::load_world_file(file);need(migrated.snapshot && *migrated.snapshot==legacy,"real v1 stone save migrates without loss: "+migrated.error);
    body=payload;put_bytes(0,1);put_bytes(1,4);
    for(std::size_t i=0;i<legacy.cells.size();++i){put_bytes(0,4);put_bytes(0,2);put_bytes(0,2);put_bytes(0,1);}
    const auto v2_payload=body;hash=14695981039346656037ULL;for(auto byte:v2_payload){hash^=byte;hash*=1099511628211ULL;}
    body={'P','S','B','W','A','L','L',0};put_bytes(2,4);put_bytes(v2_payload.size(),8);put_bytes(hash,8);body.insert(body.end(),v2_payload.begin(),v2_payload.end());
    std::ofstream v2(file,std::ios::binary|std::ios::trunc);v2.write(reinterpret_cast<const char*>(body.data()),static_cast<std::streamsize>(body.size()));v2.close();
    const auto migrated_v2=persistence::load_world_file(file);need(migrated_v2.snapshot && *migrated_v2.snapshot==legacy,"real v2 thermal save migrates with zero velocity: "+migrated_v2.error);
    body=payload;put_bytes(0,1);put_bytes(2,4);
    for(std::size_t i=0;i<legacy.cells.size();++i){put_bytes(0,4);put_bytes(0,2);put_bytes(0,2);put_bytes(0,1);put_bytes(0,8);put_bytes(0,4);put_bytes(0,4);put_bytes(0,2);put_bytes(0,2);}
    const auto v3_payload=body;hash=14695981039346656037ULL;for(auto byte:v3_payload){hash^=byte;hash*=1099511628211ULL;}
    body={'P','S','B','W','A','L','L',0};put_bytes(3,4);put_bytes(v3_payload.size(),8);put_bytes(hash,8);body.insert(body.end(),v3_payload.begin(),v3_payload.end());
    std::ofstream v3(file,std::ios::binary|std::ios::trunc);v3.write(reinterpret_cast<const char*>(body.data()),static_cast<std::streamsize>(body.size()));v3.close();
    const auto migrated_v3=persistence::load_world_file(file);need(migrated_v3.snapshot && *migrated_v3.snapshot==legacy,"real v3 transport save migrates with fixed phase and zero adhesion timers: "+migrated_v3.error);
    body=payload;put_bytes(0,1);put_bytes(3,4);
    auto v4_expected=legacy;const auto first_stone=static_cast<std::size_t>(std::find(legacy.cells.begin(),legacy.cells.end(),1)-legacy.cells.begin());
    v4_expected.states[first_stone].velocity_x=-128;v4_expected.states[first_stone].vertical_fraction=32;
    for(const auto& state:v4_expected.states){put_bytes(0,4);put_bytes(0,2);put_bytes(0,2);put_bytes(0,1);put_bytes(0,8);put_bytes(static_cast<std::uint32_t>(state.velocity_x),4);put_bytes(0,4);put_bytes(state.vertical_fraction,2);put_bytes(0,2);put_bytes(0,1);put_bytes(0,2);put_bytes(0,2);put_bytes(0,2);}
    const auto v4_payload=body;hash=14695981039346656037ULL;for(auto byte:v4_payload){hash^=byte;hash*=1099511628211ULL;}
    body={'P','S','B','W','A','L','L',0};put_bytes(4,4);put_bytes(v4_payload.size(),8);put_bytes(hash,8);body.insert(body.end(),v4_payload.begin(),v4_payload.end());
    std::ofstream v4(file,std::ios::binary|std::ios::trunc);v4.write(reinterpret_cast<const char*>(body.data()),static_cast<std::streamsize>(body.size()));v4.close();
    const auto migrated_v4=persistence::load_world_file(file);need(migrated_v4.snapshot && *migrated_v4.snapshot==v4_expected,"real v4 save preserves signed velocity/fractions and starts with no fuse: "+migrated_v4.error);

    body=payload;put_bytes(0,1);put_bytes(4,4);
    for(const auto& state:v4_expected.states){put_bytes(0,4);put_bytes(0,2);put_bytes(0,2);put_bytes(0,1);put_bytes(0,8);put_bytes(static_cast<std::uint32_t>(state.velocity_x),4);put_bytes(0,4);put_bytes(state.vertical_fraction,2);put_bytes(0,2);put_bytes(0,1);put_bytes(0,2);put_bytes(0,2);put_bytes(0,2);put_bytes(0,2);}
    const auto v5_payload=body;hash=14695981039346656037ULL;for(auto byte:v5_payload){hash^=byte;hash*=1099511628211ULL;}
    body={'P','S','B','W','A','L','L',0};put_bytes(5,4);put_bytes(v5_payload.size(),8);put_bytes(hash,8);body.insert(body.end(),v5_payload.begin(),v5_payload.end());
    std::ofstream v5(file,std::ios::binary|std::ios::trunc);v5.write(reinterpret_cast<const char*>(body.data()),static_cast<std::streamsize>(body.size()));v5.close();
    const auto migrated_v5=persistence::load_world_file(file);need(migrated_v5.snapshot && *migrated_v5.snapshot==v4_expected,"real v5 save preserves existing fields and starts with no electric pulses: "+migrated_v5.error);

}
void tools(){
    auto c=catalog(false);GameSession s(c);auto snap=blank(*c);restore(s,snap);
    need(bool(s.pause()),"pause lab");need(bool(s.submit({CommandKind::paint,{-1,63},2,"material.sand"})),"paused brush queues");
    auto before=s.capture_snapshot();need(!s.advance_fixed_step() && s.capture_snapshot()==before,"queued lab command does not run while paused");
    need(bool(s.resume()),"resume");ticks(s,1);need(count(s,"material.sand")==13,"brush executes only in fixed step");
    command(s,{CommandKind::paint,{-1,63},2,"material.sand"});need(count(s,"material.sand")<=26,"paint only fills air");
    const auto held=s.published_world()->held_stone;command(s,{CommandKind::pick_up,{}});need(s.published_world()->held_stone==held,"material brush never awards inventory");
    put(snap,*c,{0,63},"material.sand_static");restore(s,snap);command(s,{CommandKind::loosen,{0,63},1});need(count(s,"material.sand_static")==1 && count(s,"material.sand")==0 && s.world()->cell_state({0,64})->mobilized,"static powder loosening preserves material identity");
    command(s,{CommandKind::paint,{10,64},2,"material.stone"});command(s,{CommandKind::erase,{10,64},2});
    const auto v=*s.published_world();need(v.world_stone+v.dropped_stone+v.held_stone==v.initial_stone,"debug stone source/sink maintains explicit budget");
    s.end();need(bool(s.create_empty_world(1)) && !s.published_world()->material_lab,"new empty world inherits no lab state");
}
void settling(){
    auto c=catalog(false);GameSession s(c);auto snap=blank(*c,{{-16,48},32,48});
    rect(snap,*c,-16,94,32,2,"material.stone");rect(snap,*c,-16,48,1,46,"material.stone");rect(snap,*c,15,48,1,46,"material.stone");
    auto settle=[&](){
        int elapsed=0;
        for(;elapsed<8000;++elapsed){
            ticks(s,1);need(count(s,"material.water")==100,"settling preserves every liquid unit");
            if(elapsed>0 && !s.published_world()->active_chunks && !s.published_world()->moved_cells)break;
        }
        need(elapsed<8000,"cold liquid reaches its hydrostatic sleeping state within the bounded observation");
        return elapsed+1;
    };
    rect(snap,*c,-15,78,30,1,"material.stone");rect(snap,*c,-15,68,10,10,"material.water");restore(s,snap);const int first_settle=settle();
    const auto before=*s.capture_snapshot();
    for(int i=0;i<100;++i){ticks(s,1);need(s.published_world()->moved_cells==0,"cold water settles without random lateral walking");}
    need(s.published_world()->active_chunks==0,"settled liquid chunks sleep");
    command(s,{CommandKind::dig,{-1,78},1});ticks(s,150);
    need(count(s,"material.water")==100,"opening a drain preserves water volume");
    bool drained=false;for(int y=80;y<94;++y)for(int x=-15;x<15;++x)if(s.world()->material_at({x,y})==s.world()->material_index("material.water"))drained=true;
    need(drained,"removing support wakes and drains a formerly sleeping pool across x=0");
    restore(s,before);command(s,{CommandKind::push,{0,74},5});const int disturbed_settle=settle();
    std::cout<<"cold pool settle/recover="<<first_settle<<'/'<<disturbed_settle<<'\n';
    need(count(s,"material.water")==100,"impulse preserves volume");
    for(int i=0;i<100;++i){ticks(s,1);need(s.published_world()->moved_cells==0,"disturbed liquid settles again after impulse damping");}
}
void liquid_wake(){
    auto defs=catalog(false)->materials();
    for(auto& d:defs)if(d.id=="material.water")d.flow_search=128;
    const auto built=ContentRegistry::build(std::move(defs));need(bool(built),built.error);
    const auto c=built.registry;GameSession s(c);
    for(const int direction:{1,-1}){
        auto snap=blank(*c,{{-128,56},256,16});
        rect(snap,*c,-128,64,256,1,"material.stone");
        rect(snap,*c,-128,70,256,2,"material.stone");
        rect(snap,*c,-128,56,1,14,"material.stone");
        rect(snap,*c,127,56,1,14,"material.stone");
        put(snap,*c,{-100*direction,63},"material.water");restore(s,snap);ticks(s,5);
        need(s.published_world()->active_chunks==0,"long-range liquid initially sleeps on a level shelf");
        static_cast<void>(s.take_changes());
        command(s,{CommandKind::erase,{20*direction,64},1});ticks(s,1);
        need(s.world()->material_at({-100*direction,63})==Material::air,"opening a reachable outlet wakes liquid two chunks away");
        Temp temp;const auto path=temp.path/"draining.psw";const auto moving=*s.capture_snapshot();
        const auto written=persistence::save_world_file(path,moving);need(written.ok,written.error);
        const auto loaded=persistence::load_world_file(path);need(loaded.snapshot && *loaded.snapshot==moving,"remote draining state survives the real save file");
        GameSession reloaded(c);restore(reloaded,*loaded.snapshot);
        ticks(s,180);ticks(reloaded,180);
        need(s.capture_snapshot()==reloaded.capture_snapshot(),"reconstructed wake state preserves the future drain trajectory");
        need(count(s,"material.water")==1,"remote drain conserves the actual liquid cell");
        bool drained=false;for(int x=-127;x<127;++x)for(int y=65;y<70;++y)
            drained=drained || s.world()->material_at({x,y})==s.world()->material_index("material.water");
        need(drained,"woken liquid follows the shelf and reaches the lower basin");
        ticks(s,10);const auto settled=*s.capture_snapshot();static_cast<void>(s.take_changes());
        for(int i=0;i<60;++i){ticks(s,1);need(s.published_world()->moved_cells==0,"remote drain settles without surface walking");}
        auto after=*s.capture_snapshot();after.tick=settled.tick;
        need(after==settled && s.take_changes().dirty_chunks.empty() && s.published_world()->active_chunks==0,"remote wake leaves no state churn, uploads or permanently awake chunks");
    }
}
void wide_liquid(){
    const auto c=catalog(false);GameSession pool(c);auto snap=blank(*c,{{-128,56},256,40});
    rect(snap,*c,-128,94,256,2,"material.stone");rect(snap,*c,-128,56,1,38,"material.stone");rect(snap,*c,127,56,1,38,"material.stone");
    rect(snap,*c,-127,60,20,25,"material.water");restore(pool,snap);
    int elapsed=0;for(;elapsed<12000;++elapsed){ticks(pool,1);if(elapsed>100 && !pool.published_world()->active_chunks && !pool.published_world()->moved_cells)break;}
    int low=94,high=56;
    for(int x=-127;x<127;++x){int surface=94;for(int y=56;y<94;++y)if(pool.world()->material_at({x,y})==pool.world()->material_index("material.water")){surface=y;break;}low=std::min(low,surface);high=std::max(high,surface);}
    std::cout<<"wide pool surface="<<low<<".."<<high<<" moved="<<pool.published_world()->moved_cells<<" settling_ticks="<<std::min(elapsed+1,12000)<<'\n';
    need(count(pool,"material.water")==500 && high-low<=1,"wide connected pool levels beyond the local search horizon with exact volume");
    const auto stable=*pool.capture_snapshot();static_cast<void>(pool.take_changes());ticks(pool,100);auto steady=*pool.capture_snapshot();steady.tick=stable.tick;
    need(steady==stable && !pool.published_world()->active_chunks && pool.take_changes().dirty_chunks.empty(),"fully level wide pool sleeps without surface churn");
    auto short_defs=c->materials(),long_defs=short_defs;
    for(auto& d:short_defs)if(d.id=="material.water")d.flow_search=1;
    for(auto& d:long_defs)if(d.id=="material.water")d.flow_search=128;
    const auto short_c=ContentRegistry::build(short_defs),long_c=ContentRegistry::build(long_defs);need(bool(short_c) && bool(long_c),"local search budget fixtures");
    snap=blank(*c,{{-64,56},128,16});rect(snap,*c,-64,64,128,1,"material.stone");rect(snap,*c,-64,70,128,2,"material.stone");rect(snap,*c,0,56,1,14,"material.stone");put(snap,*c,{-20,63},"material.water");
    GameSession separated(short_c.registry);restore(separated,snap);ticks(separated,5);command(separated,{CommandKind::erase,{20,64},1});ticks(separated,100);
    need(separated.world()->material_at({-20,63})==separated.world()->material_index("material.water") && !separated.published_world()->moved_cells && !separated.published_world()->active_chunks,"row lookahead cannot see through a solid partition into another basin");
    for(const int direction:{1,-1}){
        snap=blank(*c,{{-256,56},512,16});rect(snap,*c,-256,64,512,1,"material.stone");rect(snap,*c,-256,70,512,2,"material.stone");
        rect(snap,*c,-256,56,1,14,"material.stone");rect(snap,*c,255,56,1,14,"material.stone");put(snap,*c,{-220*direction,63},"material.water");
        GameSession a(short_c.registry),b(long_c.registry);restore(a,snap);restore(b,snap);ticks(a,5);ticks(b,5);
        need(!a.published_world()->active_chunks && !b.published_world()->active_chunks,"remote shelf initially sleeps with either local budget");
        command(a,{CommandKind::erase,{120*direction,64},1});command(b,{CommandKind::erase,{120*direction,64},1});ticks(a,1);ticks(b,1);
        need(a.world()->material_at({-220*direction,63})==Material::air,"outlet more than 300 cells away wakes the correct row across all intervening chunks");
        for(int i=0;i<100;++i){ticks(a,1);ticks(b,1);need(a.capture_snapshot()==b.capture_snapshot(),"local search budget changes cost, not physical trajectory");}
        Temp temp;const auto file=temp.path/"wide-flow.psw";const auto moving=*a.capture_snapshot();need(persistence::save_world_file(file,moving).ok,"save long-range flow");const auto read=persistence::load_world_file(file);need(read.snapshot && *read.snapshot==moving,"real save preserves long-range material state");
        GameSession loaded(short_c.registry);restore(loaded,*read.snapshot);ticks(a,500);ticks(loaded,500);
        need(a.capture_snapshot()==loaded.capture_snapshot() && count(a,"material.water")==1,"derived row cache rebuilds without changing the saved future or quantity");
        bool drained=false;for(int y=65;y<70;++y)for(int x=-255;x<255;++x)drained=drained || a.world()->material_at({x,y})==a.world()->material_index("material.water");
        need(drained,"remote water actually reaches the lower basin rather than merely waking");
        ticks(a,10);const auto rest=*a.capture_snapshot();static_cast<void>(a.take_changes());ticks(a,100);steady=*a.capture_snapshot();steady.tick=rest.tick;
        need(steady==rest && !a.published_world()->active_chunks && a.take_changes().dirty_chunks.empty(),"remote disturbance settles again without perpetual wake propagation");
    }
}
std::shared_ptr<const ContentRegistry> powder_catalog(int liquid_speed=102){
    auto definitions=catalog(false)->materials();
    for(auto& d:definitions){
        if(d.id=="material.sand"){d.gravity_q8=230;d.powder_liquid_speed_q8=liquid_speed;}
        if(d.id=="material.water"){d.source_liquid=true;d.liquid_gravity=1.5f;}
    }
    auto result=ContentRegistry::build(std::move(definitions));need(bool(result),result.error);return result.registry;
}
void powder_block(){
    const auto c=powder_catalog();GameSession s(c);auto snap=blank(*c,{{-32,0},64,256});
    rect(snap,*c,-10,10,20,20,"material.sand");restore(s,snap);
    for(int t=0;t<15;++t){
        ticks(s,1);int top=256,bottom=-1,left=64,right=-64;
        for(int y=0;y<256;++y)for(int x=-32;x<32;++x)if(s.world()->material_at({x,y})==s.world()->material_index("material.sand")){
            top=std::min(top,y);bottom=std::max(bottom,y);left=std::min(left,x);right=std::max(right,x);
        }
        need(count(s,"material.sand")==400 && bottom-top+1==20 && right-left+1==20,"20x20 powder falls as one block, including fractional startup");
        if(t==14)std::cout<<"falling block tick15 bounds="<<left<<','<<top<<".."<<right<<','<<bottom<<'\n';
    }
    snap=blank(*c,{{-32,0},64,128});rect(snap,*c,-32,96,64,1,"material.stone");
    rect(snap,*c,-10,76,20,20,"material.sand");restore(s,snap);ticks(s,180);
    const auto rest=*s.capture_snapshot();static_cast<void>(s.take_changes());
    for(int t=0;t<100;++t){ticks(s,1);auto after=*s.capture_snapshot();after.tick=rest.tick;
        need(after==rest && !s.published_world()->active_chunks && s.take_changes().dirty_chunks.empty(),"stationary powder pile stays asleep with no state or dirty-chunk churn");}
}
void powder_sink(){
    const auto c=powder_catalog();GameSession s(c);auto snap=blank(*c,{{-24,0},48,160});
    rect(snap,*c,-24,0,1,160,"material.stone");rect(snap,*c,23,0,1,160,"material.stone");
    rect(snap,*c,-24,159,48,1,"material.stone");rect(snap,*c,-24,0,48,1,"material.stone");
    rect(snap,*c,-23,60,46,99,"material.water");rect(snap,*c,-10,10,20,20,"material.sand");restore(s,snap);
    const auto water=count(s,"material.water");
    for(int t=0;t<700;++t){ticks(s,1);need(count(s,"material.sand")==400 && count(s,"material.water")==water,"sand/water exchange conserves both counts every step");}
    int top=160,bottom=0;
    for(int y=0;y<160;++y)for(int x=-24;x<24;++x)if(s.world()->material_at({x,y})==s.world()->material_index("material.sand")){top=std::min(top,y);bottom=std::max(bottom,y);}
    std::cout<<"sunken 400-cell block y="<<top<<".."<<bottom<<" water="<<water<<'\n';
    need(bottom==158 && top>=134,"entire sand block sinks to the closed pool bottom");
}
void powder_liquid_rate(){
    for(const int rate:{77,102,128}){
        const auto c=powder_catalog(rate);GameSession s(c);auto snap=blank(*c,{{-1,0},3,160});
        rect(snap,*c,-1,0,3,160,"material.stone");rect(snap,*c,0,1,1,158,"material.water");put(snap,*c,{0,10},"material.sand");
        snap.states[10*3+1].velocity_y=2048;restore(s,snap);
        for(int t=1;t<=100;++t){ticks(s,1);const int expected_y=10+t*rate/256;
            need(s.world()->material_at({0,expected_y})==s.world()->material_index("material.sand"),"submerged fall accumulates the configured fractional rate, including high-speed entry");
            need(count(s,"material.sand")==1 && count(s,"material.water")==157,"fractional swaps preserve both materials");}
        std::cout<<"submerged rate="<<rate<<"/256 displacement100="<<100*rate/256<<'\n';
    }
    const auto c=powder_catalog();GameSession s(c);auto snap=blank(*c,{{-1,0},3,160});
    rect(snap,*c,-1,0,1,160,"material.stone");rect(snap,*c,1,0,1,160,"material.stone");rect(snap,*c,-1,159,3,1,"material.stone");
    rect(snap,*c,0,20,1,139,"material.water");put(snap,*c,{0,10},"material.sand");snap.states[10*3+1].velocity_y=2048;
    restore(s,snap);ticks(s,2);
    need(s.world()->material_at({0,19})==s.world()->material_index("material.sand"),"airborne multi-step movement stops at liquid boundary without spending air speed underwater");
    ticks(s,2);need(s.world()->material_at({0,19})==s.world()->material_index("material.sand"),"liquid entry waits for fractional credit");
    ticks(s,1);need(s.world()->material_at({0,20})==s.world()->material_index("material.sand"),"boundary eventually admits powder at liquid rate");
    snap=blank(*c,{{-2,0},5,12});rect(snap,*c,-2,0,5,12,"material.stone");
    put(snap,*c,{0,2},"material.sand");rect(snap,*c,1,3,1,8,"material.water");restore(s,snap);
    ticks(s,2);need(s.world()->material_at({0,2})==s.world()->material_index("material.sand"),"diagonal liquid entry also waits for fractional credit");
    ticks(s,1);need(s.world()->material_at({1,3})==s.world()->material_index("material.sand") && count(s,"material.water")==8,"diagonal entry exchanges one liquid cell without bypassing sinking rate");
    for(const int invalid:{0,257}){auto definitions=c->materials();definitions[1].powder_liquid_speed_q8=invalid;
        need(!ContentRegistry::build(std::move(definitions)),"invalid liquid sinking rate rejected");}
}
void transport(){
    auto c=catalog(false);GameSession s(c);auto snap=blank(*c);
    put(snap,*c,{0,40},"material.sand");restore(s,snap);ticks(s,3);
    need(s.world()->material_at({0,46})==s.world()->material_index("material.sand"),"gravity accelerates 1+2+3 cells with persistent velocity");
    snap=blank(*c);rect(snap,*c,-32,44,96,1,"material.stone");put(snap,*c,{0,40},"material.sand");restore(s,snap);ticks(s,5);
    need(s.world()->material_at({0,43})==s.world()->material_index("material.sand"),"substeps stop accelerated powder at a one-cell barrier");
    auto slow_defs=c->materials();slow_defs[3].flow_interval=4;slow_defs[3].gravity_q8=128;
    auto slow_content=ContentRegistry::build(std::move(slow_defs));need(bool(slow_content),slow_content.error);
    GameSession slow(slow_content.registry);snap=blank(*c);put(snap,*c,{0,40},"material.water");restore(s,snap);restore(slow,snap);ticks(s,8);ticks(slow,8);
    int fast_y=0,slow_y=0;for(int y=32;y<96;++y)for(int x=-32;x<64;++x){
        if(s.world()->material_at({x,y})==s.world()->material_index("material.water"))fast_y=y;
        if(slow.world()->material_at({x,y})==slow.world()->material_index("material.water"))slow_y=y;
    }
    need(fast_y>slow_y,"configured gravity and viscosity alter transport without material-specific branches");
    ticks(slow,600);const auto slow_before=*slow.capture_snapshot();ticks(slow,50);auto slow_after=*slow.capture_snapshot();slow_after.tick=slow_before.tick;
    need(slow_before==slow_after && slow.published_world()->active_chunks==0,"fractional gravity also reaches a stable sleeping state");
    auto invalid=c->materials();invalid[3].damping_milli=1001;need(!ContentRegistry::build(std::move(invalid)),"invalid transport coefficient rejected");
}
void chemistry(){
    auto c=catalog();GameSession s(c);auto snap=blank(*c,{{-8,56},16,16});
    auto old_mapping=blank(*c);old_mapping.material_ids.resize(14);restore(s,old_mapping);
    need(s.world()->material_index("material.salt")!=Material::air,"loading an older laboratory appends new definitions without changing existing indices");
    need(command(s,{CommandKind::paint,{0,64},1,"material.salt"}).ok && count(s,"material.salt")==5,"new definitions work after loading an older catalog");
    rect(snap,*c,-8,56,16,16,"material.stone");put(snap,*c,{-1,63},"material.salt");put(snap,*c,{0,63},"material.water");restore(s,snap);ticks(s,1);
    need(count(s,"material.salt")==0 && count(s,"material.salt_water")==1,"salt dissolves into salt water across chunks");
    put(snap,*c,{-1,63},"material.water");put(snap,*c,{0,63},"material.sludge");restore(s,snap);ticks(s,1);
    need(count(s,"material.sludge")==0 && count(s,"material.water")==2,"water purifies toxic sludge through configured contact");
    snap=blank(*c);put(snap,*c,{0,64},"material.water");restore(s,snap);command(s,{CommandKind::cool,{0,64},1});
    need(count(s,"material.water")==0 && count(s,"material.ice")==1,"cold brush freezes water through configured cold conversion");
    snap=blank(*c);put(snap,*c,{0,64},"material.cement");restore(s,snap);ticks(s,4);
    need(count(s,"material.concrete")==0,"age-gated reaction does not execute early");ticks(s,1);
    need(count(s,"material.concrete")==1,"age-gated cement reaction wakes and cures on the fifth step");
    auto oxygen_defs=c->materials();oxygen_defs[6].requires_oxygen=true;auto oxygen_content=ContentRegistry::build(std::move(oxygen_defs),c->reactions());need(bool(oxygen_content),oxygen_content.error);
    GameSession oxygen(oxygen_content.registry);snap=blank(*oxygen_content.registry,{{-8,56},16,16});rect(snap,*oxygen_content.registry,-8,56,16,16,"material.stone");put(snap,*oxygen_content.registry,{0,64},"material.wood");restore(oxygen,snap);
    command(oxygen,{CommandKind::heat,{0,64},1});need(!oxygen.world()->cell_state({0,64})->burning,"sealed wood requires oxygen to ignite");
    command(oxygen,{CommandKind::erase,{0,62},1});command(oxygen,{CommandKind::heat,{0,64},1});need(oxygen.world()->cell_state({0,64})->burning,"exposing heated wood to air permits ignition");
}

void gas_transport(){
    auto defs=catalog(false)->materials();
    auto smoke=std::find_if(defs.begin(),defs.end(),[](const auto& d){return d.id=="material.smoke";});
    smoke->gas_upward_speed_q8=128;smoke->gas_horizontal_speed_q8=0;
    auto c=ContentRegistry::build(defs);need(bool(c),c.error);GameSession s(c.registry);
    auto snap=blank(*c.registry);put(snap,*c.registry,{0,64},"material.smoke");restore(s,snap);ticks(s,6);
    need(s.world()->material_at({0,61})==s.world()->material_index("material.smoke"),"fractional gas rise accumulates without liquid-style unlimited acceleration");
    rect(snap,*c.registry,-32,60,96,1,"material.stone");put(snap,*c.registry,{0,61},"material.smoke");
    // Remove the earlier open-air sample.
    snap.cells[static_cast<std::size_t>(64-snap.bounds.origin.y)*snap.bounds.width+32]=0;
    snap.states[static_cast<std::size_t>(64-snap.bounds.origin.y)*snap.bounds.width+32]={};
    restore(s,snap);ticks(s,6);
    need(s.world()->material_at({0,61})==s.world()->material_index("material.smoke") && count(s,"material.smoke")==1,"zero lateral rate holds gas below a ceiling");
    smoke->gas_upward_speed_q8=256;smoke->gas_horizontal_speed_q8=512;
    c=ContentRegistry::build(defs);need(bool(c),c.error);GameSession lateral(c.registry);restore(lateral,snap);ticks(lateral,1);
    need(lateral.world()->material_at({-2,61})==lateral.world()->material_index("material.smoke") || lateral.world()->material_at({2,61})==lateral.world()->material_index("material.smoke"),"configured gas lateral speed traverses two cells below the ceiling");
    smoke->gas_downward_speed_q8=512;
    c=ContentRegistry::build(defs);need(bool(c),c.error);GameSession impulse(c.registry);
    snap=blank(*c.registry);put(snap,*c.registry,{0,64},"material.smoke");
    snap.states[static_cast<std::size_t>(64-snap.bounds.origin.y)*snap.bounds.width+32].velocity_y=1536;
    restore(impulse,snap);ticks(impulse,1);
    need(impulse.world()->material_at({0,66})==impulse.world()->material_index("material.smoke"),"gas downward impulse obeys its own directional speed limit");
    smoke->gas_upward_speed_q8=0;smoke->gas_horizontal_speed_q8=0;smoke->lifetime=0;
    c=ContentRegistry::build(defs);need(bool(c),c.error);GameSession still(c.registry);snap=blank(*c.registry);put(snap,*c.registry,{0,64},"material.smoke");restore(still,snap);ticks(still,12);
    need(still.world()->material_at({0,64})==still.world()->material_index("material.smoke") && still.published_world()->active_chunks==0,"zero directional rates and no lifetime reach a sleeping state");
    smoke->gas_horizontal_speed_q8=-1;need(!ContentRegistry::build(defs),"negative directional gas speed rejected");
    smoke->gas_horizontal_speed_q8=8193;need(!ContentRegistry::build(defs),"excessive directional gas speed rejected");
}
void three_reagents(){
    auto defs=catalog(false)->materials();
    auto brine=std::find_if(defs.begin(),defs.end(),[](const auto& d){return d.id=="material.salt_water";});brine->is_static=true;
    auto table=parse_reaction_table("# Original three-input salt-water evaporation\r\n [fire] ; material.salt_water ; keep ; material.salt ; 1 ; 0 ; -1000 ; 10000 ; material.salt_water ; material.steam \r\n");
    need(bool(table) && table.reactions.size()==1,table.error);
    auto c=ContentRegistry::build(defs,table.reactions);need(bool(c),c.error);GameSession s(c.registry);
    // Isolate chemistry with stationary brine and stone; transport has separate tests.
    auto snap=blank(*c.registry,{{-8,56},16,16});rect(snap,*c.registry,-8,56,16,16,"material.stone");
    put(snap,*c.registry,{-1,63},"material.fire");put(snap,*c.registry,{0,63},"material.salt_water");
    restore(s,snap);ticks(s,1);
    need(count(s,"material.salt_water")==1 && count(s,"material.salt")==0 && count(s,"material.steam")==0,"a matching cell cannot satisfy two reagent slots");
    put(snap,*c.registry,{1,64},"material.salt_water");restore(s,snap);ticks(s,1);
    need(count(s,"material.salt_water")==2 && count(s,"material.salt")==0,"a disconnected third reagent is not consumed");
    put(snap,*c.registry,{1,64},"material.stone");put(snap,*c.registry,{0,64},"material.salt_water");restore(s,snap);ticks(s,1);
    need(count(s,"material.salt_water")==0 && count(s,"material.salt")==1 && count(s,"material.steam")==1 && count(s,"material.fire")==1,"three distinct connected reagents commit all outputs across x=0/y=64");
    need(s.published_world()->reactions==1,"the catalyst cannot trigger a second reaction in the same phase");
    for(int shift=0;shift<3;++shift){
        auto rotated=table.reactions;
        const auto& r=table.reactions[0];
        if(shift==1){rotated[0].input_a=r.input_b;rotated[0].output_a=r.output_b;rotated[0].input_b=r.input_c;rotated[0].output_b=r.output_c;rotated[0].input_c=r.input_a;rotated[0].output_c=r.output_a;}
        if(shift==2){rotated[0].input_a=r.input_c;rotated[0].output_a=r.output_c;rotated[0].input_b=r.input_a;rotated[0].output_b=r.output_a;rotated[0].input_c=r.input_b;rotated[0].output_c=r.output_b;}
        auto ordered=ContentRegistry::build(defs,rotated);need(bool(ordered),ordered.error);GameSession candidate(ordered.registry);restore(candidate,snap);ticks(candidate,1);
        need(count(candidate,"material.salt")==1 && count(candidate,"material.steam")==1,"third-input matching is independent of reagent slot order");
    }
    auto age=table.reactions;age[0].minimum_age=4;c=ContentRegistry::build(defs,age);need(bool(c),c.error);GameSession delayed(c.registry);restore(delayed,snap);ticks(delayed,4);
    need(count(delayed,"material.salt_water")==2,"three-input age check leaves every reagent intact before threshold");ticks(delayed,1);need(count(delayed,"material.salt_water")==0,"three-input reaction waits for first reagent age");
    auto air=parse_reaction_table("air;air;material.water;material.ice;1;0;-1000;10000;material.salt;air");
    c=ContentRegistry::build(defs,air.reactions);need(bool(c),c.error);GameSession air_case(c.registry);
    snap=blank(*c.registry,{{-8,56},16,16});rect(snap,*c.registry,-8,56,16,16,"material.stone");put(snap,*c.registry,{0,63},"material.salt");
    for(auto p:{CellPos{0,62},CellPos{-1,63}}){auto i=static_cast<std::size_t>(p.y-56)*16+static_cast<std::size_t>(p.x+8);snap.cells[i]=0;snap.states[i]={};--snap.initial_stone;}
    restore(air_case,snap);ticks(air_case,1);need(count(air_case,"material.salt")==0 && count(air_case,"material.water")==1 && count(air_case,"material.ice")==1,"nonair third reagent anchors two distinct air contacts");
    for(const auto* bad:{"a;b;c;d;1;;0", "a;b;c;d;1;0;4294967296", "a;b;c;d;1;0;-1000;10000;e", "a;b;c;d;0", "a;b;c;d;1;0;100;20"}){
        auto rejected=parse_reaction_table(bad);need(!rejected && rejected.reactions.empty() && rejected.error.find("line 1")!=std::string::npos,"malformed tables fail with line number and no partial results");
    }
    auto invalid=table.reactions;invalid[0].input_c="material.missing";need(!ContentRegistry::build(defs,invalid),"missing third material rejected");
    invalid=table.reactions;invalid[0].input_c.clear();need(!ContentRegistry::build(defs,invalid),"third output without third input rejected");
    auto pair=parse_reaction_table("\xef\xbb\xbfmaterial.water;material.wood;keep;air;1");need(bool(pair) && pair.reactions[0].input_c.empty(),"BOM and legacy two-input format remain supported");
}
void mechanical(){
    auto defs=catalog(false)->materials();for(auto& d:defs)if(d.can_loosen)d.settle_ticks=4;
    auto built=ContentRegistry::build(defs);need(bool(built),built.error);auto c=built.registry;GameSession s(c);
    auto snap=blank(*c,{{-16,48},32,48});rect(snap,*c,-16,94,32,2,"material.stone");
    put(snap,*c,{-1,63},"material.stone");put(snap,*c,{1,63},"material.wood");put(snap,*c,{3,63},"material.sand_static");put(snap,*c,{6,63},"material.water_static");
    restore(s,snap);ticks(s,10);need(s.world()->material_at({-1,63})==Material::stone,"unsupported fixed material waits for an explicit disturbance");
    const auto total=s.published_world()->initial_stone;
    command(s,{CommandKind::loosen,{-1,63},1});
    need(s.world()->material_at({-1,64})==Material::stone && s.world()->cell_state({-1,64})->mobilized,"loosened stone crosses y=64 without changing its material ID");
    command(s,{CommandKind::loosen,{1,63},1});command(s,{CommandKind::loosen,{3,63},1});
    need(count(s,"material.wood")==1 && count(s,"material.sand_static")==1 && count(s,"material.sand")==0,"different fixed materials use the same mechanical phase without conversion");
    need(command(s,{CommandKind::loosen,{6,63},1}).amount==0 && !s.world()->cell_state({6,63})->mobilized,"static liquid is not a loosenable solid");
    Temp temp;const auto file=temp.path/"moving.psw";const auto moving=*s.capture_snapshot();
    need(persistence::save_world_file(file,moving).ok,"save mobilized material in flight");auto read=persistence::load_world_file(file);
    need(read.snapshot && *read.snapshot==moving,"mechanical phase and counters roundtrip exactly");GameSession other(c);restore(other,*read.snapshot);
    for(int i=0;i<100;++i){ticks(s,1);ticks(other,1);need(s.capture_snapshot()==other.capture_snapshot(),"loaded mechanical future matches uninterrupted simulation");}
    need(s.published_world()->initial_stone==total && count(s,"material.stone")==total && s.published_world()->held_stone==0 && s.published_world()->dropped_stone==0,"falling stone does not duplicate drops or alter the stone budget");
    auto partway=*s.capture_snapshot();
    for(std::size_t n=0;n<partway.cells.size();++n)if(partway.cells[n]==static_cast<std::uint16_t>(s.world()->material_index("material.sand_static"))){partway.states[n].mobilized=true;partway.states[n].quiet_ticks=2;}
    need(persistence::save_world_file(file,partway).ok,"save a partially completed settling counter");
    read=persistence::load_world_file(file);need(read.snapshot && *read.snapshot==partway,"nonzero settling counter survives the binary file");
    const auto settled=*s.capture_snapshot();
    for(const auto& state:settled.states)need(!state.mobilized && !state.quiet_ticks,"supported loose materials return to fixed phase");
    CellPos rock{};bool found=false;
    for(int y=48;y<94;++y)for(int x=-16;x<16;++x)if(s.world()->material_at({x,y})==Material::stone){rock={x,y};found=true;}
    need(found && rock.y==93,"loosened rock reaches the floor");
    command(s,{CommandKind::erase,{rock.x,95},1});ticks(s,5);
    need(s.world()->material_at(rock)==Material::stone && !s.world()->cell_state(rock)->mobilized,"a re-fixed material does not turn into universal structural collapse");
    command(s,{CommandKind::loosen,rock,1});ticks(s,3);need(s.world()->material_at(rock)!=Material::stone,"a new disturbance mobilizes the re-fixed stone again");
    restore(s,snap);command(s,{CommandKind::push,{-2,63},2});
    const auto impacted=*s.capture_snapshot();
    need(std::any_of(impacted.states.begin(),impacted.states.end(),[](const auto& v){return v.mobilized;}),"impact can mobilize a fixed solid and apply momentum");
    const auto before=*s.capture_snapshot();auto invalid=before;auto i=static_cast<std::size_t>(63-48)*32+22;
    invalid.states[i].mobilized=true;need(!s.restore_world(invalid) && s.capture_snapshot()==before,"invalid mechanical phase on static liquid is rejected atomically");
    invalid=before;invalid.states[i].quiet_ticks=1;need(!s.restore_world(invalid) && s.capture_snapshot()==before,"quiet counter on a fixed phase is rejected");
    defs[4].can_loosen=true;need(!ContentRegistry::build(defs),"non-powder static liquid cannot register as loosenable");
}
void adhesion(){
    auto defs=catalog(false)->materials();defs[0].can_loosen=false;
    auto built=ContentRegistry::build(defs);need(bool(built),built.error);auto c=built.registry;GameSession s(c);
    auto snap=blank(*c,{{-16,48},32,48});rect(snap,*c,-16,63,32,1,"material.stone");rect(snap,*c,-16,94,32,2,"material.stone");
    put(snap,*c,{-1,64},"material.glue");put(snap,*c,{3,64},"material.water");restore(s,snap);ticks(s,50);
    need(s.world()->material_at({-1,64})==s.world()->material_index("material.glue") && s.world()->material_at({3,64})!=s.world()->material_index("material.water"),"ceiling adhesion holds glue but ordinary water falls");
    need(s.published_world()->active_chunks==0,"indefinite cold adhesion can sleep without internal churn");
    command(s,{CommandKind::erase,{-1,62},1});ticks(s,8);
    need(count(s,"material.glue")==1 && s.world()->material_at({-1,64})!=s.world()->material_index("material.glue"),"removing cross-chunk ceiling support wakes adhesive material without deleting it");
    restore(s,snap);command(s,{CommandKind::push,{-1,63},2});ticks(s,2);
    need(count(s,"material.glue")==1 && s.world()->material_at({-1,64})!=s.world()->material_index("material.glue"),"a sufficiently strong impulse detaches glue from an intact ceiling");
    auto glue=std::find_if(defs.begin(),defs.end(),[](const auto& d){return d.id=="material.glue";});glue->adhesion_hold_ticks=4;glue->flow_interval=4;
    built=ContentRegistry::build(defs);need(bool(built),built.error);GameSession drip(built.registry);restore(drip,snap);ticks(drip,3);
    need(drip.world()->cell_state({-1,64})->adhesion_ticks==3,"adhesion lifetime counts fixed steps");
    Temp temp;const auto file=temp.path/"adhesion.psw";const auto held=*drip.capture_snapshot();need(persistence::save_world_file(file,held).ok,"save during an adhesion hold");
    auto loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==held,"hold timer persists");GameSession resumed(built.registry);restore(resumed,*loaded.snapshot);
    ticks(drip,1);ticks(resumed,1);const auto released=*drip.capture_snapshot();
    need(drip.world()->cell_state({-1,64})->adhesion_cooldown==16,"fixed-step expiry creates a cooldown even between viscosity updates");
    need(persistence::save_world_file(file,released).ok,"save immediately after adhesive release");loaded=persistence::load_world_file(file);
    need(loaded.snapshot && *loaded.snapshot==released,"nonzero release cooldown survives the binary file");GameSession cooldown_reload(built.registry);restore(cooldown_reload,*loaded.snapshot);
    for(int n=0;n<20;++n){ticks(drip,1);ticks(resumed,1);ticks(cooldown_reload,1);need(drip.capture_snapshot()==resumed.capture_snapshot() && drip.capture_snapshot()==cooldown_reload.capture_snapshot(),"adhesion expiry and release cooldown continue identically after reload");}
    need(drip.world()->material_at({-1,64})!=drip.world()->material_index("material.glue"),"timed adhesion releases and cooldown prevents immediate reattachment under fractional gravity");
    snap=blank(*c,{{-16,48},32,48});rect(snap,*c,-16,63,32,1,"material.stone");rect(snap,*c,-16,94,32,2,"material.stone");rect(snap,*c,-1,64,1,5,"material.slime");
    restore(s,snap);ticks(s,15);
    for(int y=64;y<68;++y)need(s.world()->material_at({-1,y})==s.world()->material_index("material.slime"),"bounded cohesive slime strand hangs from fixed support");
    need(s.world()->material_at({-1,68})!=s.world()->material_index("material.slime") && count(s,"material.slime")==5,"strand beyond the configured support span falls without loss");
    command(s,{CommandKind::erase,{-1,62},1});ticks(s,20);
    need(s.world()->material_at({-1,64})!=s.world()->material_index("material.slime") && count(s,"material.slime")==5,"a slime chain cannot keep floating after the anchor is removed");
    auto bad=*s.capture_snapshot();auto i=std::find_if(bad.cells.begin(),bad.cells.end(),[&](auto m){return m==static_cast<std::uint16_t>(s.world()->material_index("material.slime"));})-bad.cells.begin();
    bad.states[static_cast<std::size_t>(i)].adhesion_cooldown=17;need(!s.restore_world(bad),"invalid adhesion cooldown rejected");
    glue->adhesion_strength_q8=8193;need(!ContentRegistry::build(defs),"invalid adhesion force rejected");
}
void slime_reactions(){
    auto c=catalog();GameSession s(c);auto snap=blank(*c,{{-8,56},16,16});rect(snap,*c,-8,56,16,16,"material.stone");
    put(snap,*c,{-1,63},"material.fire");put(snap,*c,{0,63},"material.ice_slime");restore(s,snap);ticks(s,1);
    need(count(s,"material.ice_slime")==0 && count(s,"material.slime")==1,"fire contact melts frozen slime through the source material tag");
    snap=blank(*c);put(snap,*c,{0,64},"material.slime");restore(s,snap);command(s,{CommandKind::cool,{0,64},1});
    need(count(s,"material.ice_slime")==1 && count(s,"material.slime")==0,"slime freezes through its configured cold material");
    snap=blank(*c,{{-8,56},16,16});rect(snap,*c,-8,56,16,16,"material.stone");
    put(snap,*c,{-1,63},"material.water");put(snap,*c,{0,63},"material.slime");restore(s,snap);ticks(s,1);
    need(count(s,"material.water")==1 && count(s,"material.cloud_slime")==1 && count(s,"material.slime")==0,"water removes slime through finite source vapour instead of inventing water volume");
    ticks(s,10);need(count(s,"material.cloud_slime")==0 && count(s,"material.water")==1,"finite slime vapour expires without a collectible reward");
}
std::shared_ptr<const ContentRegistry> blast_catalog(ExplosionDefinition profile={}){
    auto defs=catalog(false)->materials();defs[0].durability=10;defs[0].hit_points=100;
    MaterialDefinition powder;powder.id="material.explosive_powder";powder.display_name="Explosive powder";powder.is_static=false;powder.sand=true;
    powder.density=7;powder.gravity_q8=0;powder.fuel=100;powder.ignition=10;
    powder.explosion.radius=2;powder.explosion.ray_energy=1000;powder.explosion.heat=0;powder.explosion.impulse_q8=0;
    powder.explosion.delay_ticks=3;powder.explosion.trigger_energy=10;powder.explosion.output="air";powder.explosion.fire_material="air";powder.hit_points=1;
    defs.push_back(powder);auto probe=powder;probe.id="material.blast_profile";probe.display_name="Blast profile";probe.explosion=std::move(profile);probe.explosion.radius=4;defs.push_back(probe);
    MaterialDefinition shield;shield.id="material.shield";shield.display_name="Shield";shield.durability=100;shield.hit_points=100;defs.push_back(shield);
    auto result=ContentRegistry::build(std::move(defs));need(bool(result),result.error);return result.registry;
}
void explosion(){
    ExplosionDefinition profile;profile.heat=0;profile.fire_material="air";
    auto c=blast_catalog(profile);GameSession s(c);auto snap=blank(*c,{{-24,40},48,48});
    rect(snap,*c,-10,62,21,5,"material.stone");restore(s,snap);const auto original=count(s,"material.stone");
    need(bool(s.pause()) && bool(s.submit({CommandKind::blast,{0,64},2,"material.blast_profile"})),"queue a paused blast");
    const auto queued=s.capture_snapshot();need(!s.advance_fixed_step() && s.capture_snapshot()==queued,"blast cannot modify a paused world before a fixed step");need(bool(s.resume()),"resume blast");ticks(s,1);
    need(s.published_world()->explosions==1 && s.published_world()->blast_destroyed>0 && count(s,"material.stone")<original,"cross-chunk explosion removes actual terrain and publishes one event");
    need(s.published_world()->blast_mobilized>0 && s.published_world()->initial_stone==count(s,"material.stone") && s.published_world()->dropped_stone==0 && s.published_world()->held_stone==0,"outer blast mobilizes surviving material without duplicate inventory rewards");
    need(s.take_changes().dirty_chunks.size()>=2,"cross-boundary blast publishes affected chunk textures");
    snap=blank(*c,{{-24,40},48,48});rect(snap,*c,0,40,1,48,"material.shield");put(snap,*c,{2,64},"material.wood");restore(s,snap);
    command(s,{CommandKind::blast,{-3,64},2,"material.blast_profile"});
    need(s.world()->material_at({0,64})==s.world()->material_index("material.shield") && s.world()->material_at({2,64})==s.world()->material_index("material.wood") && s.world()->cell_state({2,64})->temperature==0,"durability gate shields the far side of a one-cell wall");
    profile.ray_energy=200;profile.impulse_q8=0;c=blast_catalog(profile);GameSession weak(c);snap=blank(*c,{{-24,40},48,48});
    rect(snap,*c,0,40,4,48,"material.stone");put(snap,*c,{5,64},"material.wood");restore(weak,snap);command(weak,{CommandKind::blast,{-3,64},2,"material.blast_profile"});
    need(weak.published_world()->blast_destroyed>0 && weak.world()->material_at({3,64})==Material::stone && weak.world()->material_at({5,64})==weak.world()->material_index("material.wood"),"per-ray HP consumption stops penetration before the rear of a thick wall");
    profile.ray_energy=400000;profile.hole_enabled=false;profile.impulse_q8=1536;c=blast_catalog(profile);GameSession push(c);snap=blank(*c,{{-24,40},48,48});
    rect(snap,*c,-3,63,6,2,"material.water");rect(snap,*c,-3,66,6,2,"material.oil");put(snap,*c,{5,64},"material.stone");restore(push,snap);
    command(push,{CommandKind::blast,{0,64},2,"material.blast_profile"});
    need(count(push,"material.water")==12 && count(push,"material.oil")==12 && count(push,"material.stone")==1 && push.published_world()->blast_destroyed==0,"non-excavating blast conserves liquids and solids while applying impulse");
    profile.hole_enabled=true;profile.impulse_q8=0;c=blast_catalog(profile);GameSession preserve(c);snap=blank(*c,{{-24,40},48,48});rect(snap,*c,-2,63,4,2,"material.water");restore(preserve,snap);
    command(preserve,{CommandKind::blast,{0,64},1,"material.blast_profile"});need(count(preserve,"material.water")==8,"hole_destroy_liquid=false preserves non-heated liquid");
    profile.destroy_liquids=true;c=blast_catalog(profile);GameSession remove(c);restore(remove,snap);command(remove,{CommandKind::blast,{0,64},1,"material.blast_profile"});need(count(remove,"material.water")==0,"liquid removal occurs only with its explicit destruction flag");
    profile.destroy_liquids=false;profile.hole_enabled=false;profile.heat=120;c=blast_catalog(profile);GameSession ignition(c);snap=blank(*c);put(snap,*c,{1,64},"material.wood");restore(ignition,snap);
    command(ignition,{CommandKind::blast,{0,64},1,"material.blast_profile"});const auto hot=*ignition.capture_snapshot();bool burning_wood=false;
    for(std::size_t n=0;n<hot.cells.size();++n)if(hot.cells[n]==static_cast<std::uint16_t>(ignition.world()->material_index("material.wood")) && hot.states[n].burning)burning_wood=true;
    need(burning_wood,"blast heat ignites exposed fuel without requiring an excavation");
    auto invalid=c->materials();invalid.back().explosion.ray_energy=0;need(!ContentRegistry::build(invalid),"invalid explosion energy rejected");
    invalid=c->materials();auto bomb=std::find_if(invalid.begin(),invalid.end(),[](const auto& d){return d.id=="material.explosive_powder";});bomb->explosion.output="material.absent";need(!ContentRegistry::build(invalid),"missing explosion residue rejected");
}
void chain_reaction(){
    ExplosionDefinition profile;profile.heat=0;profile.fire_material="air";auto c=blast_catalog(profile);GameSession s(c);
    auto snap=blank(*c,{{-24,40},48,48});put(snap,*c,{-2,63},"material.explosive_powder");put(snap,*c,{-1,63},"material.explosive_powder");put(snap,*c,{1,63},"material.explosive_powder");restore(s,snap);
    command(s,{CommandKind::heat,{-3,63},1});need(s.world()->cell_state({-2,63})->detonation_ticks==3,"heated explosive starts a finite fuse");
    need(bool(s.pause()),"pause lit explosive");const auto armed=*s.capture_snapshot();need(!s.advance_fixed_step() && s.capture_snapshot()==armed,"pause freezes an armed fuse");
    Temp temp;const auto path=temp.path/"fuse.psw";need(persistence::save_world_file(path,armed).ok,"save an armed fuse");auto loaded=persistence::load_world_file(path);need(loaded.snapshot && *loaded.snapshot==armed,"nonzero fuse survives actual file encoding");
    GameSession resumed(c);restore(resumed,*loaded.snapshot);need(bool(s.resume()),"resume source fuse");ticks(s,2);ticks(resumed,2);need(s.capture_snapshot()==resumed.capture_snapshot() && count(s,"material.explosive_powder")==3,"fuse does not detonate early after reload");
    ticks(s,1);ticks(resumed,1);need(s.capture_snapshot()==resumed.capture_snapshot() && s.published_world()->explosions==1 && count(s,"material.explosive_powder")==1,"connected fuel aggregates into one explosion instead of one event per pixel");
    need(s.world()->cell_state({1,63})->detonation_ticks==3,"pressure primes a separate pocket without same-step recursive detonation");
    for(int i=0;i<3;++i){ticks(s,1);ticks(resumed,1);need(s.capture_snapshot()==resumed.capture_snapshot(),"chain future matches after an armed save/load");}
    need(count(s,"material.explosive_powder")==0 && s.published_world()->explosions==1,"separate pocket detonates only after its own fuse");
    snap=blank(*c,{{-64,0},128,128});for(int i=0;i<65;++i){const CellPos p{-60+(i%16)*8,8+(i/16)*8};put(snap,*c,p,"material.explosive_powder");snap.states[static_cast<std::size_t>(p.y)*128+static_cast<std::size_t>(p.x+64)].detonation_ticks=1;}
    restore(s,snap);ticks(s,1);need(s.published_world()->explosions==64 && count(s,"material.explosive_powder")==1,"bounded explosion batch retains excess ready fuel");
    const auto waiting=*s.capture_snapshot();need(persistence::save_world_file(path,waiting).ok,"save a deferred ready explosion");loaded=persistence::load_world_file(path);GameSession deferred(c);need(loaded.snapshot.has_value(),loaded.error);restore(deferred,*loaded.snapshot);ticks(s,1);ticks(deferred,1);
    need(count(s,"material.explosive_powder")==0 && s.capture_snapshot()==deferred.capture_snapshot(),"deferred event survives save and runs next step without being dropped");
    auto bad=*s.capture_snapshot();auto before=s.capture_snapshot();bad.states[0].detonation_ticks=601;need(!s.restore_world(bad) && s.capture_snapshot()==before,"invalid fuse does not replace current state");
}
std::shared_ptr<const ContentRegistry> electric_catalog(){
    auto original=blast_catalog();auto defs=original->materials();
    for(auto& d:defs)if(d.id=="material.water" || d.id=="material.water_static" || d.id=="material.salt_water")d.electrical_conductivity=true;
    for(const auto* name:{"steel","copper","brass","silver"}){
        MaterialDefinition m;m.id=std::string("material.")+name;m.display_name=name;m.is_static=std::string(name)=="steel";
        m.sand=true;m.density=8;m.electrical_conductivity=true;m.electric_cost=1;m.can_loosen=m.is_static;defs.push_back(m);
    }
    ReactionDefinition alloy{"material.copper","material.brass","material.silver","material.smoke",1};alloy.input_c="material.water";alloy.output_c="material.smoke";
    auto built=ContentRegistry::build(std::move(defs),{alloy});need(bool(built),built.error);return built.registry;
}
ElectricPulse pulse(CellPos p,std::uint32_t energy=1000,std::uint16_t speed=32,std::uint16_t heat=0){ElectricPulse result;result.position=result.previous=p;result.energy=energy;result.speed=speed;result.heat=heat;return result;}
void electricity(){
    auto c=electric_catalog();GameSession s(c);auto snap=blank(*c);rect(snap,*c,-16,63,33,1,"material.steel");rect(snap,*c,16,63,1,16,"material.steel");
    snap.electric_pulses={pulse({-16,63},2000,4)};restore(s,snap);ticks(s,1);
    auto view=*s.published_world();need(view.electric_pulses==1 && view.electric_energy==1996 && view.electric_spent==4,"pulse obeys per-tick speed and material cost");
    need(s.world()->cell_state({-13,63})->electrified_ticks && !s.world()->cell_state({-12,63})->electrified_ticks,"propagation does not update a whole wire instantaneously");
    std::uint64_t spent=view.electric_spent;for(int i=0;i<30;++i){ticks(s,1);spent+=s.published_world()->electric_spent;}
    need(spent==2000 && !s.published_world()->electric_energy && !s.published_world()->electric_pulses,"finite pulse energy is not copied or left alive in loops");
    need(!s.published_world()->electrified_cells && !s.published_world()->active_chunks,"afterglow expires and cold circuit sleeps");
    static_cast<void>(s.take_changes());ticks(s,5);need(s.take_changes().dirty_chunks.empty(),"discharged circuits stop texture uploads");
    snap=blank(*c);rect(snap,*c,-4,64,9,1,"material.steel");rect(snap,*c,0,60,1,9,"material.steel");snap.electric_pulses={pulse({0,64},1000,1)};restore(s,snap);ticks(s,1);
    need(s.published_world()->electric_pulses==4 && s.published_world()->electric_energy==999 && s.published_world()->electric_spent==1,"junction divides energy among four children");
    snap=blank(*c);rect(snap,*c,-4,64,9,1,"material.steel");rect(snap,*c,-4,65,9,1,"material.stone");put(snap,*c,{0,63},"material.stone");put(snap,*c,{0,64},"material.oil");snap.electric_pulses={pulse({-4,64})};restore(s,snap);ticks(s,1);
    need(!s.world()->cell_state({1,64})->electrified_ticks && !s.published_world()->electric_energy,"oil blocks conduction");
    need(count(s,"material.steel")==8 && count(s,"material.oil")==1,"cold conduction preserves material");
    snap=blank(*c);put(snap,*c,{-1,63},"material.steel");put(snap,*c,{0,64},"material.steel");snap.electric_pulses={pulse({-1,63})};restore(s,snap);ticks(s,1);
    need(!s.world()->cell_state({0,64})->electrified_ticks,"electricity cannot cross air or diagonal corners");
    snap=blank(*c,{{-8,56},16,24});rect(snap,*c,-5,67,10,1,"material.stone");rect(snap,*c,-5,62,1,5,"material.stone");rect(snap,*c,4,62,1,5,"material.stone");rect(snap,*c,-4,63,8,4,"material.water");snap.electric_pulses={pulse({-4,63})};restore(s,snap);ticks(s,1);
    need(s.world()->cell_state({-3,64})->electrified_ticks && count(s,"material.water")==32,"liquid conducts across y=64 without water loss when heat is disabled");
    need(bool(s.pause()) && bool(s.submit({CommandKind::electrify,{-4,63},1})),"electrical command queues while paused");const auto before=s.capture_snapshot();need(!s.advance_fixed_step() && s.capture_snapshot()==before,"paused input cannot inject energy");
    need(bool(s.resume()),"resume electricity");ticks(s,1);need(s.command_results().back().ok && s.command_results().back().amount==1000,"brush injects one finite pulse");
    snap=blank(*c);put(snap,*c,{0,64},"material.oil");restore(s,snap);need(!command(s,{CommandKind::electrify,{0,64},1}).ok,"insulating brush target reports failure");
    auto invalid=c->materials();invalid[0].electric_cost=0;need(!ContentRegistry::build(std::move(invalid)),"zero travel cost rejected");
}
void electric_persistence(){
    auto c=electric_catalog();GameSession s(c);auto snap=blank(*c);rect(snap,*c,-32,63,80,1,"material.steel");snap.electric_pulses={pulse({-32,63},2000,4)};restore(s,snap);ticks(s,3);need(bool(s.pause()),"pause pulse");const auto saved=*s.capture_snapshot();
    need(!saved.electric_pulses.empty() && !s.advance_fixed_step() && s.capture_snapshot()==saved,"paused pulse and afterglow unchanged");
    Temp temp;const auto file=temp.path/"electric.psw";need(persistence::save_world_file(file,saved).ok,"save electricity");auto read=persistence::load_world_file(file);need(read.snapshot && *read.snapshot==saved,"position, direction, energy, parameters and afterglow roundtrip");
    auto reordered=c->materials();std::reverse(reordered.begin(),reordered.end());auto built=ContentRegistry::build(reordered,c->reactions());need(bool(built),built.error);GameSession other(built.registry);restore(other,*read.snapshot);need(bool(s.resume()),"resume source");
    for(int i=0;i<100;++i){ticks(s,1);ticks(other,1);need(s.capture_snapshot()==other.capture_snapshot() && s.published_world()->electric_energy==other.published_world()->electric_energy,"electric future matches after registry reordering");}
    const auto before=s.capture_snapshot();auto bad=saved;bad.electric_pulses.push_back(bad.electric_pulses.front());need(!s.restore_world(bad) && s.capture_snapshot()==before,"duplicate heads rejected atomically");
    bad=saved;bad.electric_pulses[0].energy=0;need(!s.restore_world(bad),"zero energy rejected");bad=saved;bad.electric_pulses[0].speed=65;need(!s.restore_world(bad),"invalid speed rejected");
    bad=saved;bad.electric_pulses[0].position.x+=2;need(!s.restore_world(bad),"nonadjacent previous position rejected");bad=saved;bad.states[0].electrified_ticks=61;need(!s.restore_world(bad),"invalid afterglow rejected");
    auto changed=saved;const auto head=changed.electric_pulses.front().position;const auto i=static_cast<std::size_t>(head.y-changed.bounds.origin.y)*changed.bounds.width+static_cast<std::size_t>(head.x-changed.bounds.origin.x);changed.cells[i]=0;changed.states[i]={};
    need(persistence::save_world_file(file,changed).ok,"save pulse after its conducting cell changes");read=persistence::load_world_file(file);GameSession stale(c);need(read.snapshot.has_value(),read.error);restore(stale,*read.snapshot);ticks(stale,1);need(!stale.published_world()->electric_energy,"removed conductor dissipates pulse after valid reload");
    s.end();need(bool(s.create_empty_world(7)) && s.capture_snapshot()->electric_pulses.empty() && !s.published_world()->electrified_cells,"new world inherits no electricity");
}
void electric_effects(){
    auto c=electric_catalog();GameSession s(c);auto snap=blank(*c);put(snap,*c,{0,64},"material.steel");put(snap,*c,{1,64},"material.wood");auto p=pulse({0,64},1000,32,120);p.heat_interval=1;snap.electric_pulses={p};restore(s,snap);ticks(s,1);
    need(s.world()->cell_state({1,64})->burning && !s.world()->cell_state({1,64})->electrified_ticks,"contact heat ignites insulating fuel without making it conductive");
    snap=blank(*c);put(snap,*c,{0,64},"material.steel");put(snap,*c,{1,64},"material.explosive_powder");snap.electric_pulses={p};restore(s,snap);ticks(s,1);
    need(s.world()->cell_state({1,64})->detonation_ticks==3,"electric heat primes explosive fuse");ticks(s,3);need(count(s,"material.explosive_powder")==0 && s.published_world()->explosions==1,"electrical ignition uses real explosion phase");
    snap=blank(*c,{{-8,56},16,16});rect(snap,*c,-8,56,16,16,"material.stone");put(snap,*c,{-1,63},"material.copper");put(snap,*c,{0,63},"material.brass");put(snap,*c,{0,64},"material.water");restore(s,snap);ticks(s,1);
    need(count(s,"material.copper")==0 && count(s,"material.brass")==0 && count(s,"material.water")==0 && count(s,"material.silver")==1 && count(s,"material.smoke")==2,"source copper/brass/water transmutation works with conductive metals");
}
std::shared_ptr<const ContentRegistry> blob_catalog(std::string_view table){
    std::vector<MaterialDefinition> defs;
    for(const auto* id:{"stone","a","a_variant","b","c","d"}){
        MaterialDefinition d;d.id=std::string("material.")+id;d.display_name=id;
        if(d.id=="material.a" || d.id=="material.a_variant")d.tags={"reactant"};
        defs.push_back(std::move(d));
    }
    const auto parsed=parse_reaction_table(table);need(bool(parsed),parsed.error);
    const auto result=ContentRegistry::build(std::move(defs),parsed.reactions);need(bool(result),result.error);return result.registry;
}
WorldSnapshot blob_fixture(const ContentRegistry& c){
    auto snap=blank(c,{{-16,56},32,24});
    rect(snap,c,-8,63,8,1,"material.a");rect(snap,c,0,63,9,1,"material.b");
    rect(snap,c,-3,64,1,4,"material.a"); // Connected branch crosses y=64.
    put(snap,c,{-1,66},"material.a"); // Same ID, disconnected inside the disk.
    put(snap,c,{-1,64},"material.a_variant"); // Same input tag, different actual material.
    return snap;
}
void reaction_blobs(){
    auto c=blob_catalog("[reactant];material.b;material.stone;material.c;1;blob_radius_a=6;blob_radius_b=3");
    GameSession s(c);auto snap=blob_fixture(*c);restore(s,snap);static_cast<void>(s.take_changes());ticks(s,1);
    need(count(s,"material.a")==2 && count(s,"material.stone")==11 && count(s,"material.c")==4,"one contact converts the exact restricted disk across x=0/y=64");
    need(s.world()->material_at({-8,63})==s.world()->material_index("material.a") && s.world()->material_at({4,63})==s.world()->material_index("material.b"),"both configured radius limits are respected");
    need(s.world()->material_at({-1,66})==s.world()->material_index("material.a") && count(s,"material.a_variant")==1,"blobs do not jump empty gaps or convert a different material sharing the selector tag");
    need(s.published_world()->reactions==1 && s.published_world()->initial_stone==11 && !s.published_world()->held_stone && !s.published_world()->dropped_stone,"a regional event updates real counts and the stone budget once without inventory awards");
    const auto changes=s.take_changes();need(changes.dirty_chunks.size()>=3,"regional writes publish every affected render chunk");
    const auto result=s.capture_snapshot();ticks(s,4);auto stable=*s.capture_snapshot();stable.tick=result->tick;
    need(stable==*result && s.published_world()->active_chunks==0,"finished blobs leave no pending state or continuing writes");
    auto swapped=blob_catalog("material.b;[reactant];material.c;material.stone;1;blob_radius_a=3;blob_radius_b=6");
    GameSession reverse(swapped);restore(reverse,blob_fixture(*swapped));ticks(reverse,1);
    need(reverse.capture_snapshot()->cells==result->cells,"reversing reagent slots preserves regional geometry and outputs");
    auto triple=blob_catalog("material.a;material.b;material.stone;material.c;1;0;-1000;10000;material.d;keep;blob_radius_a=2");
    GameSession t(triple);snap=blank(*triple,{{-8,56},16,16});rect(snap,*triple,-3,63,3,1,"material.a");put(snap,*triple,{0,63},"material.b");restore(t,snap);ticks(t,1);
    need(count(t,"material.a")==3 && !count(t,"material.stone"),"missing third reagent leaves the entire blob unchanged");
    put(snap,*triple,{0,64},"material.d");restore(t,snap);ticks(t,1);
    need(count(t,"material.stone")==3 && count(t,"material.c")==1 && count(t,"material.d")==1 && t.published_world()->reactions==1,"third input is resolved before any regional output is committed");
    auto overlap=blob_catalog("material.a;material.a;material.stone;material.c;1;blob_radius_a=2;blob_radius_b=2");
    GameSession o(overlap);snap=blank(*overlap);rect(snap,*overlap,-1,63,3,1,"material.a");restore(o,snap);ticks(o,1);
    need(count(o,"material.stone")==1 && count(o,"material.c")==2 && !count(o,"material.a") && o.published_world()->reactions==1,"overlap partitions cells by nearest anchor and preserves each distinct anchor output");
    auto chain=blob_catalog("material.a;material.b;material.c;keep;1;blob_radius_a=3\nmaterial.c;air;material.d;keep;1");
    GameSession next(chain);snap=blank(*chain);rect(snap,*chain,-3,63,3,1,"material.a");put(snap,*chain,{0,63},"material.b");restore(next,snap);ticks(next,1);
    need(count(next,"material.c")==3 && !count(next,"material.d"),"regional products cannot react twice in one contact phase");ticks(next,1);need(count(next,"material.d")==3,"regional products become eligible on the next fixed step");
    auto edge=blob_catalog("material.a;material.b;material.c;keep;1;blob_radius_a=64");
    GameSession clipped(edge);snap=blank(*edge,{{-4,-4},8,8});rect(snap,*edge,-4,-4,2,8,"material.a");put(snap,*edge,{-2,-4},"material.b");restore(clipped,snap);ticks(clipped,1);
    need(count(clipped,"material.c")==16 && count(clipped,"material.b")==1,"maximum-radius blob clips safely at finite negative-coordinate boundaries");
    auto gated=c->reactions();gated[0].minimum_age=4;auto built=ContentRegistry::build(c->materials(),gated);need(bool(built),built.error);GameSession age(built.registry);restore(age,blob_fixture(*built.registry));ticks(age,4);
    need(!count(age,"material.stone"),"regional age failure has no partial output");ticks(age,1);need(count(age,"material.stone")==11,"pending age-gated contact remains awake and commits on time");
    for(const auto* bad:{"a;b;c;d;1;blob_radius_a=-1","a;b;c;d;1;blob_radius_a=65","a;b;c;d;1;blob_radius_a=4294967296","a;b;c;d;1;blob_radius_a=1;blob_radius_a=2","a;b;c;d;1;blob_radius_x=2","a;b;c;d;1;blob_radius_a=1;4","a;b;c;d;1;blob_radius_a="}){
        const auto rejected=parse_reaction_table(std::string("# header\n")+bad);need(!rejected && rejected.reactions.empty() && rejected.error.find("line 2")!=std::string::npos,"invalid blob table rejects all partial results with source line");
    }
    auto invalid=c->reactions();invalid[0].blob_radius_a=65;need(!ContentRegistry::build(c->materials(),invalid),"direct native registration validates blob size");
    invalid=c->reactions();invalid[0].input_a="air";need(!ContentRegistry::build(c->materials(),invalid),"air cannot seed a restricted material blob");
}
void blob_persistence(){
    auto c=blob_catalog("material.a;material.b;material.stone;material.c;1;4;0;100;blob_radius_a=6;blob_radius_b=3");
    GameSession s(c);restore(s,blob_fixture(*c));ticks(s,2);need(bool(s.pause()),"pause before regional reaction");const auto saved=*s.capture_snapshot();
    need(!count(s,"material.stone") && !s.advance_fixed_step() && s.capture_snapshot()==saved,"paused regional contact does not commit");
    Temp temp;const auto file=temp.path/"blob.psw";need(persistence::save_world_file(file,saved).ok,"save age-gated regional contact");auto loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==saved,"pre-reaction shapes and ages roundtrip through a real file");
    auto defs=c->materials();std::reverse(defs.begin(),defs.end());auto built=ContentRegistry::build(defs,c->reactions());need(bool(built),built.error);GameSession other(built.registry);restore(other,*loaded.snapshot);need(bool(s.resume()),"resume blob");
    for(int i=0;i<12;++i){ticks(s,1);ticks(other,1);need(s.capture_snapshot()==other.capture_snapshot(),"regional future and shape match after saved-age restoration and registry reorder");}
    need(count(s,"material.stone")==11 && count(s,"material.c")==4,"saved pending contact produces the real regional outputs");
    const auto finished=*s.capture_snapshot();need(persistence::save_world_file(file,finished).ok,"save completed regional geometry");loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==finished,"finished regional geometry, counts, seed and tick survive file reload");
    std::ofstream broken(file,std::ios::binary|std::ios::trunc);broken<<"bad blob save";broken.close();loaded=persistence::load_world_file(file);
    need(!loaded.snapshot && s.capture_snapshot()==finished,"a damaged file cannot replace the existing post-reaction world");
}
std::shared_ptr<const ContentRegistry> phase_catalog(){
    auto defs=catalog(false)->materials();defs[3].is_static=true;defs[10].tags.push_back("frozen");
    auto add=[&](const char* id,const char* source,const char* tag,bool molten){
        MaterialDefinition d;d.id=id;d.source_id=source;d.display_name=source;d.tags={tag};d.is_static=false;d.sand=!molten;d.density=molten?3.0f:8.0f;
        if(molten){d.fuel=90000;d.initially_burning=true;d.burn_heat=50;d.flame_output=d.smoke_output="air";d.burn_output=d.id;}
        defs.push_back(std::move(d));
    };
    // Deliberately different project IDs prove that families use source_id.
    add("material.red_metal","copper","meltable_metal",false);add("material.red_liquid","copper_molten","molten",true);
    add("material.blue_metal","silver","meltable_metal",false);add("material.blue_liquid","silver_molten","molten",true);
    add("material.wax","wax","meltable",false);add("material.liquid_wax","wax_molten","molten",true);
    add("material.burning_liquid","liquid_fire","fire_strong",true);defs.back().fuel=1000000;defs.back().burn_heat=150;
    const auto table=parse_reaction_table(
        "[meltable_metal];[lava];[meltable_metal]_molten;keep;1\n"
        "[fire_strong];[meltable_metal];[fire_strong];[meltable_metal]_molten;1\n"
        "[fire];[meltable];material.smoke;[meltable]_molten;1\n"
        "[meltable]_molten;air;[meltable];keep;1\n"
        "material.red_liquid;[water];material.red_metal;material.steam;1\n"
        "material.blue_liquid;[water];material.blue_metal;material.steam;1\n"
        "material.red_liquid;[frozen];material.red_metal;[frozen];1");
    need(bool(table),table.error);auto built=ContentRegistry::build(std::move(defs),table.reactions);need(bool(built),built.error);return built.registry;
}
void phase_variants(){
    const auto c=phase_catalog();need(c->reactions().size()==9,"two source metal families expand both melting rules without material switches");
    for(const auto& r:c->reactions())need(r.input_a.find("]_")==std::string::npos && r.input_b.find("]_")==std::string::npos && !r.output_a.starts_with('[') && !r.output_b.starts_with('['),"all derived selectors/outputs resolve before simulation");
    for(const auto& d:c->materials()){
        const auto candidates=c->reaction_candidates(d.id);std::size_t previous=0;bool first=true;
        for(const auto i:candidates){need(i<c->reactions().size() && (first || i>previous),"dispatch preserves rule priority and has no duplicate entries");previous=i;first=false;}
    }
    need(c->reaction_candidates("material.stone").empty() && c->reaction_candidates("material.missing").empty(),"nonreactive and unknown materials have no reaction candidates");
    auto defs=c->materials();defs.erase(std::remove_if(defs.begin(),defs.end(),[](const auto& d){return d.source_id=="copper_molten";}),defs.end());
    const auto raw=parse_reaction_table("[meltable_metal];[lava];[meltable_metal]_molten;keep;1");
    auto rejected=ContentRegistry::build(defs,raw.reactions);need(!rejected && rejected.error.find("copper_molten")!=std::string::npos,"missing phase target rejects registry with concrete source name");
    defs=c->materials();auto duplicate=*c->find_material("material.red_liquid");duplicate.id="material.duplicate";defs.push_back(duplicate);need(!ContentRegistry::build(defs,raw.reactions),"ambiguous source variant fails before world replacement");
    defs=c->materials();for(auto& d:defs)if(d.id=="material.red_metal")d.source_id.clear();need(!ContentRegistry::build(defs,raw.reactions),"phase base without source identity rejected");
    auto cross=parse_reaction_table("[meltable_metal];[lava];[lava];keep;1");need(!ContentRegistry::build(c->materials(),cross.reactions),"unimplemented cross-slot tag bindings fail explicitly");
    auto invalid=c->materials();invalid.back().fuel=1000000001;need(!ContentRegistry::build(invalid),"oversized uint32 fuel rejected");invalid=c->materials();invalid.back().fuel=0;need(!ContentRegistry::build(invalid),"initially burning requires finite positive fuel");
}
void phase_physics(){
    auto c=phase_catalog();GameSession s(c);
    for(const auto* solid:{"material.red_metal","material.blue_metal"}){
        const char* liquid=std::string_view(solid)=="material.red_metal"?"material.red_liquid":"material.blue_liquid";
        auto snap=blank(*c,{{-8,56},16,16});rect(snap,*c,-8,56,16,16,"material.stone");put(snap,*c,{-1,63},solid);put(snap,*c,{0,63},"material.lava");restore(s,snap);ticks(s,1);
        need(!count(s,solid) && count(s,liquid)==1 && count(s,"material.lava")==1,"lava melts each metal into its own real liquid across chunks");
        need(s.world()->cell_state({-1,63})->fuel==89999 && s.world()->cell_state({-1,63})->burning,"phase product starts burning with untruncated 90000 fuel");
        snap=*s.capture_snapshot();put(snap,*c,{0,63},"material.water");restore(s,snap);ticks(s,1);
        need(count(s,solid)==1 && !count(s,liquid) && count(s,"material.steam")==1 && !count(s,"material.water"),"water quench retains metal identity and converts one water unit to steam");
    }
    auto snap=blank(*c,{{-8,56},16,16});rect(snap,*c,-8,56,16,16,"material.stone");put(snap,*c,{-1,63},"material.red_liquid");put(snap,*c,{0,63},"material.ice");restore(s,snap);ticks(s,1);
    need(count(s,"material.red_metal")==1 && count(s,"material.ice")==1,"frozen contact quenches metal while preserving the exact ice material");
    snap=blank(*c,{{-8,56},16,16});rect(snap,*c,-8,56,16,16,"material.stone");put(snap,*c,{-1,63},"material.wax");put(snap,*c,{0,63},"material.fire");restore(s,snap);ticks(s,1);
    need(!count(s,"material.wax") && count(s,"material.liquid_wax")==1 && count(s,"material.smoke")==1,"source wax family melts and consumes the fire into smoke");
    snap=blank(*c);put(snap,*c,{0,63},"material.liquid_wax");restore(s,snap);ticks(s,1);need(count(s,"material.wax")==1 && !count(s,"material.liquid_wax"),"derived molten selector resolves wax back to its own source family in air");
    snap=blank(*c);put(snap,*c,{0,63},"material.red_liquid");restore(s,snap);ticks(s,3);
    need(s.world()->material_at({0,63})==s.world()->material_index("material.red_liquid") && s.world()->cell_state({0,63})->velocity_y>0,"shared physical gravity accumulates sub-cell molten momentum before the first whole-cell displacement");ticks(s,13);
    need(s.world()->material_at({0,69})==s.world()->material_index("material.red_liquid") && count(s,"material.red_liquid")==1,"molten metal uses actual accelerating liquid transport without losing matter");
    snap=blank(*c,{{-8,56},16,16});rect(snap,*c,-8,56,16,16,"material.stone");put(snap,*c,{-1,63},"material.red_metal");put(snap,*c,{0,63},"material.burning_liquid");restore(s,snap);ticks(s,1);
    need(count(s,"material.red_liquid")==1 && count(s,"material.burning_liquid")==1 && s.world()->cell_state({0,63})->fuel==999999,"strong burning liquid melts metal and preserves million-unit fuel");
    auto defs=c->materials();for(auto& d:defs)if(d.id=="material.red_liquid")d.fuel=2;
    auto built=ContentRegistry::build(defs);need(bool(built),built.error);GameSession exhausted(built.registry);snap=blank(*built.registry);put(snap,*built.registry,{0,63},"material.red_liquid");restore(exhausted,snap);ticks(exhausted,5);
    const auto final=*exhausted.capture_snapshot();const auto cell=std::find(final.cells.begin(),final.cells.end(),static_cast<std::uint16_t>(exhausted.world()->material_index("material.red_liquid")));
    need(cell!=final.cells.end() && final.states[static_cast<std::size_t>(cell-final.cells.begin())].fuel==0 && !final.states[static_cast<std::size_t>(cell-final.cells.begin())].burning,"identity burnout retains material, commits zero fuel and cannot underflow");
}
void phase_persistence(){
    const auto c=phase_catalog();GameSession s(c);auto snap=blank(*c,{{-8,56},16,16});rect(snap,*c,-8,56,16,16,"material.stone");put(snap,*c,{-1,63},"material.red_liquid");put(snap,*c,{0,63},"material.burning_liquid");restore(s,snap);ticks(s,2);need(bool(s.pause()),"pause high-fuel cells");const auto saved=*s.capture_snapshot();
    Temp temp;const auto file=temp.path/"phase.psw";need(persistence::save_world_file(file,saved).ok,"save high-fuel current format");auto loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==saved,"wide fuel and initial burning survive current file");
    auto defs=c->materials();std::reverse(defs.begin(),defs.end());auto built=ContentRegistry::build(defs,c->reactions());need(bool(built),built.error);GameSession other(built.registry);restore(other,*loaded.snapshot);need(bool(s.resume()),"resume phase source");
    for(int i=0;i<30;++i){ticks(s,1);ticks(other,1);need(s.capture_snapshot()==other.capture_snapshot(),"wide fuel progression matches after registry reorder and reload");}
    // Genuine version 6 stream: historical 16-bit fuel and 40-byte state layout.
    auto legacy=saved;for(auto& state:legacy.states)state.fuel=std::min<std::uint32_t>(state.fuel,50000);
    legacy.electric_pulses={pulse({-1,63},75,3,0)};
    legacy.states[static_cast<std::size_t>(63-legacy.bounds.origin.y)*legacy.bounds.width+static_cast<std::size_t>(-1-legacy.bounds.origin.x)].electrified_ticks=4;
    std::vector<unsigned char> body;auto put_bytes=[&](std::uint64_t value,int bytes){for(int i=0;i<bytes;++i)body.push_back(static_cast<unsigned char>(value>>(i*8)));};
    put_bytes(legacy.seed,8);put_bytes(legacy.tick,8);put_bytes(static_cast<std::uint32_t>(legacy.bounds.origin.x),4);put_bytes(static_cast<std::uint32_t>(legacy.bounds.origin.y),4);put_bytes(legacy.bounds.width,4);put_bytes(legacy.bounds.height,4);put_bytes(64,4);put_bytes(8,4);put_bytes(legacy.initial_stone,8);put_bytes(legacy.material_ids.size(),4);
    for(const auto& id:legacy.material_ids){put_bytes(id.size(),4);body.insert(body.end(),id.begin(),id.end());}put_bytes(legacy.cells.size(),4);for(auto cell:legacy.cells)put_bytes(cell,2);put_bytes(0,4);put_bytes(0,8);put_bytes(1,1);put_bytes(5,4);
    for(const auto& state:legacy.states){put_bytes(static_cast<std::uint32_t>(state.temperature),4);put_bytes(state.life,2);put_bytes(state.fuel,2);put_bytes(state.burning?1:0,1);put_bytes(state.created_tick,8);put_bytes(static_cast<std::uint32_t>(state.velocity_x),4);put_bytes(static_cast<std::uint32_t>(state.velocity_y),4);put_bytes(state.vertical_fraction,2);put_bytes(state.horizontal_fraction,2);put_bytes(state.mobilized?1:0,1);put_bytes(state.quiet_ticks,2);put_bytes(state.adhesion_ticks,2);put_bytes(state.adhesion_cooldown,2);put_bytes(state.detonation_ticks,2);put_bytes(state.electrified_ticks,2);}
    put_bytes(legacy.electric_pulses.size(),4);for(const auto& p:legacy.electric_pulses){put_bytes(static_cast<std::uint32_t>(p.position.x),4);put_bytes(static_cast<std::uint32_t>(p.position.y),4);put_bytes(static_cast<std::uint32_t>(p.previous.x),4);put_bytes(static_cast<std::uint32_t>(p.previous.y),4);put_bytes(p.energy,4);put_bytes(p.speed,2);put_bytes(p.heat_interval,2);put_bytes(p.heat,2);put_bytes(p.glow_ticks,2);}
    const auto payload=body;std::uint64_t hash=14695981039346656037ULL;for(auto byte:body){hash^=byte;hash*=1099511628211ULL;}body={'P','S','B','W','A','L','L',0};put_bytes(6,4);put_bytes(payload.size(),8);put_bytes(hash,8);body.insert(body.end(),payload.begin(),payload.end());
    std::ofstream old(file,std::ios::binary|std::ios::trunc);old.write(reinterpret_cast<const char*>(body.data()),static_cast<std::streamsize>(body.size()));old.close();loaded=persistence::load_world_file(file);need(loaded.snapshot && *loaded.snapshot==legacy,"real version 6 bytes widen fuel without changing shape or state: "+loaded.error);
    const auto before=s.capture_snapshot();auto invalid=*before;for(auto& state:invalid.states)if(state.fuel){state.fuel=1000000001;break;}need(!s.restore_world(invalid) && s.capture_snapshot()==before,"invalid wide fuel rejected without replacing the current world");
}
void extensibility(){
    const auto original=catalog();auto definitions=original->materials();
    MaterialDefinition added;added.id="material.blue_dust";added.display_name="Blue dust";added.is_static=false;added.sand=true;added.density=9;
    definitions.push_back(added);
    for(int i=0;i<270;++i){auto extra=added;extra.id="material.extra_"+std::to_string(i);definitions.push_back(extra);}
    auto reactions=original->reactions();reactions.push_back({added.id,"[water]","material.stone","material.steam",1});
    const auto content=ContentRegistry::build(std::move(definitions),std::move(reactions));need(bool(content),content.error);
    GameSession s(content.registry);auto snap=blank(*content.registry);put(snap,*content.registry,{-1,63},added.id);restore(s,snap);ticks(s,1);
    need(s.world()->material_at({-1,64})==s.world()->material_index(added.id),"new ID uses configured powder movement without C++ dispatch changes");
    snap=blank(*content.registry);put(snap,*content.registry,{-1,63},"material.extra_269");restore(s,snap);ticks(s,1);
    need(static_cast<std::uint16_t>(s.world()->material_index("material.extra_269"))>255 && count(s,"material.extra_269")==1,"16-bit storage supports catalogs beyond 255 definitions");
    snap=blank(*content.registry,{{-8,56},16,16});rect(snap,*content.registry,-8,56,16,16,"material.stone");
    put(snap,*content.registry,{-1,63},added.id);put(snap,*content.registry,{0,63},"material.water");restore(s,snap);ticks(s,1);
    need(count(s,added.id.c_str())==0 && count(s,"material.steam")==1,"new material participates in configured reaction without simulation code changes");
}
}
int main(int argc,char** argv){try{
    const std::string name=argc==2?argv[1]:"";
    if(name=="powder_block")powder_block();else if(name=="powder_sink")powder_sink();else if(name=="powder_liquid_rate")powder_liquid_rate();else if(name=="movement")movement();else if(name=="liquid")liquid();else if(name=="gas_fire")gas_fire();else if(name=="reactions")reactions();else if(name=="lab_persistence")persistence_test();else if(name=="lab_tools")tools();else if(name=="extensibility")extensibility();else if(name=="settling")settling();else if(name=="liquid_wake")liquid_wake();else if(name=="wide_liquid")wide_liquid();else if(name=="transport")transport();else if(name=="chemistry")chemistry();else if(name=="gas_transport")gas_transport();else if(name=="three_reagents")three_reagents();else if(name=="mechanical")mechanical();else if(name=="adhesion")adhesion();else if(name=="slime_reactions")slime_reactions();else if(name=="explosion")explosion();else if(name=="chain_reaction")chain_reaction();else if(name=="electricity")electricity();else if(name=="electric_persistence")electric_persistence();else if(name=="electric_effects")electric_effects();else if(name=="reaction_blobs")reaction_blobs();else if(name=="blob_persistence")blob_persistence();else if(name=="phase_variants")phase_variants();else if(name=="phase_physics")phase_physics();else if(name=="phase_persistence")phase_persistence();else throw std::runtime_error("Unknown material test case.");
    std::cout<<"PASS "<<name<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
